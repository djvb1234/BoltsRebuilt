#include "native/native_gpu_upload_pool.h"

#include <algorithm>
#include <new>

#include <rex/logging.h>
#include <rex/math.h>

namespace nb::gpu {

namespace {
// D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT is what the heap behind a committed buffer is rounded
// to anyway, so aligning the page up to it costs nothing and stops the pool wasting the remainder.
size_t AlignedPageBytes(size_t page_bytes) {
  return rex::align(std::max<size_t>(page_bytes, size_t(64) << 10),
                    size_t(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT));
}
}  // namespace

NativeConstantUploadPool::OwnedPage::~OwnedPage() {
  // The mapping is released implicitly with the resource, the way the SDK's own D3D12 pool leaves
  // it. The budget counter is shared rather than reached through the pool, because the base class
  // deletes pages from its own destructor, after the derived object's members are gone.
  if (gpu && !retained && gpu_bytes) {
    gpu_bytes->fetch_sub(uint64_t(bytes), std::memory_order_relaxed);
  }
}

NativeConstantUploadPool::NativeConstantUploadPool(ID3D12Device* device, size_t page_bytes,
                                                   uint64_t gpu_byte_budget)
    : GraphicsUploadBufferPool(AlignedPageBytes(page_bytes)),
      device_(device),
      gpu_byte_budget_(gpu_byte_budget),
      gpu_bytes_(std::make_shared<std::atomic<uint64_t>>(0)) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 options{};
  gpu_upload_supported_ =
      device_ != nullptr &&
      SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options,
                                             sizeof(options))) &&
      options.GPUUploadHeapSupported != FALSE;
  if (!gpu_upload_supported_) {
    REXLOG_WARN(
        "rexgpu-nb: GPU_UPLOAD constant pool requested but the adapter reports no GPU upload heap "
        "support; every page will be an ordinary upload heap");
  }
}

NativeConstantUploadPool::~NativeConstantUploadPool() {
  Shutdown(false);
}

std::unique_ptr<NativeConstantUploadPool> NativeConstantUploadPool::TryCreate(
    ID3D12Device* device, size_t page_bytes, uint64_t gpu_byte_budget) noexcept {
  try {
    return std::make_unique<NativeConstantUploadPool>(device, page_bytes, gpu_byte_budget);
  } catch (const std::bad_alloc&) {
    // Do not allocate or log on the host allocation failure path.
    return nullptr;
  }
}

void NativeConstantUploadPool::Shutdown(bool completion_proven) noexcept {
  if (shutdown_) return;
  shutdown_ = true;
  requests_disabled_ = true;
  if (!completion_proven) {
    // Both lists may contain resources referenced by submitted draws. The
    // current writable page can also contain such immutable slices. No GPU
    // wait, Map/Unmap or allocation is safe or necessary on this failure path.
    const auto retain = [this](Page* first) noexcept {
      for (Page* current = first; current; current = current->next_) {
        auto* page = static_cast<OwnedPage*>(current);
        if (page->resource.Get()) {
          page->resource.Detach();
          page->retained = true;
          ++stats_.retained_resources;
        }
      }
    };
    retain(writable_first_);
    retain(submitted_first_);
  }
  // Page metadata is ours and can always be destroyed. On an unproven shutdown
  // its COM references have already been detached; the base destructor then
  // sees empty lists. A later call with true cannot undo conservative retention.
  GraphicsUploadBufferPool::ClearCache();
}

Microsoft::WRL::ComPtr<ID3D12Resource> NativeConstantUploadPool::CreateBuffer(D3D12_HEAP_TYPE type,
                                                                             size_t bytes) const {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = type;
  heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Alignment = 0;
  desc.Width = UINT64(bytes);
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  // Both heap types accept GENERIC_READ for a buffer that is only ever read by shaders, and a
  // GPU_UPLOAD resource may not be transitioned out of it, which is exactly what a constant slice
  // needs.
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }
  return resource;
}

rex::ui::GraphicsUploadBufferPool::Page* NativeConstantUploadPool::CreatePageImplementation() {
  if (!device_) {
    return nullptr;
  }
  auto page = std::make_unique<OwnedPage>();
  page->gpu_bytes = gpu_bytes_;
  const bool budget_available =
      gpu_bytes_->load(std::memory_order_relaxed) + uint64_t(page_size_) <= gpu_byte_budget_;
  bool use_gpu = gpu_upload_supported_ && !gpu_upload_failed_ && budget_available;
  if (gpu_upload_supported_ && !gpu_upload_failed_ && !budget_available) {
    ++stats_.budget_stops;
  }
  if (use_gpu) {
    page->resource = CreateBuffer(D3D12_HEAP_TYPE_GPU_UPLOAD, page_size_);
    void* mapping = nullptr;
    const D3D12_RANGE no_read{0, 0};
    if (!page->resource || FAILED(page->resource->Map(0, &no_read, &mapping)) || !mapping) {
      // One failure is enough: retrying per page would spend the same failing call thousands of
      // times a frame. Ordinary upload pages take over from here for the life of the pool.
      page->resource.Reset();
      gpu_upload_failed_ = true;
      ++stats_.creation_failures;
      use_gpu = false;
      REXLOG_WARN(
          "rexgpu-nb: GPU_UPLOAD constant page of {} bytes could not be created or mapped; "
          "falling back to ordinary upload pages for the rest of the run",
          page_size_);
    } else {
      page->mapped = static_cast<uint8_t*>(mapping);
      page->gpu_address = page->resource->GetGPUVirtualAddress();
      page->gpu = true;
      page->bytes = page_size_;
      gpu_bytes_->fetch_add(uint64_t(page_size_), std::memory_order_relaxed);
      ++stats_.gpu_pages;
    }
  }
  if (!use_gpu) {
    page->resource = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, page_size_);
    void* mapping = nullptr;
    const D3D12_RANGE no_read{0, 0};
    if (!page->resource || FAILED(page->resource->Map(0, &no_read, &mapping)) || !mapping) {
      REXLOG_ERROR("rexgpu-nb: could not create a {}-byte native constant upload page", page_size_);
      return nullptr;
    }
    page->mapped = static_cast<uint8_t*>(mapping);
    page->gpu_address = page->resource->GetGPUVirtualAddress();
    page->gpu = false;
    page->bytes = page_size_;
    ++stats_.upload_pages;
  }
  return page.release();
}

uint8_t* NativeConstantUploadPool::Request(uint64_t submission_index, size_t size, size_t alignment,
                                           ID3D12Resource** buffer_out, size_t* offset_out,
                                           D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out) noexcept {
  if (requests_disabled_ || !device_ || !size || size > page_size_) {
    requests_disabled_ = true;
    ++stats_.request_failures;
    return nullptr;
  }
  size_t offset = 0;
  OwnedPage* page = nullptr;
  try {
    page = static_cast<OwnedPage*>(
        GraphicsUploadBufferPool::Request(submission_index, size, alignment, offset));
  } catch (const std::bad_alloc&) {
    // The base may have retired its open page before allocating the next one.
    // Keep every submitted page alive and switch all later slices to the SDK.
  }
  if (!page || !page->mapped) {
    requests_disabled_ = true;
    if (!writable_first_) {
      // The SDK leaves these offsets behind when replacement creation fails.
      // Repair only the empty writable-page state, never submitted contents.
      current_page_used_ = 0;
      current_page_flushed_ = 0;
    }
    ++stats_.request_failures;
    return nullptr;
  }
  if (buffer_out) {
    *buffer_out = page->resource.Get();
  }
  if (offset_out) {
    *offset_out = offset;
  }
  if (gpu_address_out) {
    *gpu_address_out = page->gpu_address + UINT64(offset);
  }
  ++stats_.requests;
  stats_.requested_bytes += uint64_t(size);
  return page->mapped + offset;
}

NativeGpuUploadPoolStats NativeConstantUploadPool::stats() const {
  NativeGpuUploadPoolStats copy = stats_;
  copy.gpu_bytes = gpu_bytes_ ? gpu_bytes_->load(std::memory_order_relaxed) : 0;
  return copy;
}

void NativeConstantUploadPool::ReclaimCompleted(uint64_t completed_index) {
  if (shutdown_) return;
  ++stats_.reclaims;
  Reclaim(completed_index);
}

}  // namespace nb::gpu
