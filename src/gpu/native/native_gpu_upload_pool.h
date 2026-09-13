// nb - native constant slices in CPU-writable video memory.
//
// Every native draw reads three constant buffers the CPU wrote this frame: the 208-byte root b0
// packet, the vertex packet (b1) and the pixel packet (b2). The SDK serves all of them from
// D3D12_HEAP_TYPE_UPLOAD, which is host memory: the GPU fetches those bytes across PCIe, and a
// Showdown Town frame issues about 7,400 native draws. Backend elapsed measurements alone do not
// establish whether these reads limit the frame rate.
//
// D3D12_HEAP_TYPE_GPU_UPLOAD puts the same bytes in video memory behind the resizable BAR, so the
// GPU reads them at local-memory latency. The trade is the CPU side: writes go to uncached
// write-combined video memory, which a scratch probe measured at 1.30x (contiguous) to 1.45x
// (fragmented) the cost of writing an ordinary upload heap. Whether that trade pays is a game
// measurement, not an argument, so this pool exists only to make that measurement possible and is
// off unless nb_native_constant_gpu_upload says otherwise.
//
// The page lifetime is the SDK's, unchanged: GraphicsUploadBufferPool owns the page list, hands out
// bump-allocated slices from one open page, retires a full page to the submitted list, and hands a
// retired page back only after Reclaim is told that its index completed. In-flight bytes are
// therefore never rewritten. This derives from GraphicsUploadBufferPool rather than
// D3D12UploadBufferPool because that class's page type is private, so its pages cannot be
// downcast from here.
//
// Two independent limits keep video memory bounded: a byte budget
// stops new GPU_UPLOAD pages once it is reached, and any creation or mapping failure latches so it
// is not retried per page. Both fall back to an ordinary upload page, and a pool that cannot
// allocate at all returns null so the caller uses the command processor's own pool.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <d3d12.h>
#include <wrl/client.h>

#include <rex/ui/graphics_upload_buffer_pool.h>

namespace nb::gpu {

struct NativeGpuUploadPoolStats {
  uint64_t gpu_pages = 0;         // pages actually created in GPU_UPLOAD (BAR-visible video memory)
  uint64_t upload_pages = 0;      // pages that fell back to an ordinary UPLOAD heap
  uint64_t creation_failures = 0; // GPU_UPLOAD create/map failures; the first one latches the fallback
  uint64_t budget_stops = 0;      // GPU_UPLOAD refused because the byte budget was already reached
  uint64_t requests = 0;          // slices served
  uint64_t request_failures = 0;  // slices the pool could not serve (the caller uses the SDK pool)
  uint64_t requested_bytes = 0;
  uint64_t reclaims = 0;
  uint64_t gpu_bytes = 0;         // GPU_UPLOAD bytes held, including unproven retained references
  uint64_t retained_resources = 0; // detached at shutdown because completion was not proven
};

class NativeConstantUploadPool final : public rex::ui::GraphicsUploadBufferPool {
 public:
  // One page holds roughly a sixth of a Town frame's native constants. A measured Showdown Town run
  // settled at a 48-page, 96 MiB high-water, so the budget is set well above that: reaching it would
  // silently mix ordinary upload pages into the pool and quietly spoil any comparison, which is a
  // worse failure than holding a little more video memory on a card that has gigabytes spare.
  static constexpr size_t kDefaultPageBytes = size_t(2) << 20;
  static constexpr uint64_t kDefaultGpuByteBudget = uint64_t(256) << 20;

  NativeConstantUploadPool(ID3D12Device* device, size_t page_bytes = kDefaultPageBytes,
                           uint64_t gpu_byte_budget = kDefaultGpuByteBudget);
  // Optional construction must not turn host allocation pressure into a failed draw.
  static std::unique_ptr<NativeConstantUploadPool> TryCreate(
      ID3D12Device* device, size_t page_bytes = kDefaultPageBytes,
      uint64_t gpu_byte_budget = kDefaultGpuByteBudget) noexcept;
  ~NativeConstantUploadPool() override;

  // False when the adapter does not report D3D12_OPTIONS16 GPUUploadHeapSupported. The pool still
  // works in that case, entirely out of ordinary upload pages, which is the honest control rather
  // than a silent no-op.
  bool gpu_upload_supported() const { return gpu_upload_supported_; }

  // Same shape as D3D12UploadBufferPool::Request. Null means "not served"; the caller must fall
  // back to the command processor's pool rather than refusing the draw. The first
  // failure disables later requests; output arguments remain untouched on failure.
  uint8_t* Request(uint64_t submission_index, size_t size, size_t alignment,
                   ID3D12Resource** buffer_out, size_t* offset_out,
                   D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out) noexcept;

  // Terminal and idempotent. The owner must prove completion of every submission
  // that can reference this pool, with a healthy device, before passing true.
  // Otherwise resource references are deliberately retained until process exit.
  // Destruction without this explicit proof follows the same conservative path.
  void Shutdown(bool completion_proven) noexcept;

  // Callers tag slices with the command processor's frame index, so this must be given
  // GetCompletedFrame(), exactly as the SDK reclaims its own constant pool with frame_completed_.
  void ReclaimCompleted(uint64_t completed_index);

  NativeGpuUploadPoolStats stats() const;

 protected:
  Page* CreatePageImplementation() override;

 private:
  struct OwnedPage final : Page {
    ~OwnedPage() override;
    // Shared with the pool so the budget is released when the page dies, including from the base
    // class's ClearCache, which runs after this object's own members are gone.
    std::shared_ptr<std::atomic<uint64_t>> gpu_bytes;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint8_t* mapped = nullptr;
    // Resolved once at creation: GetGPUVirtualAddress is a virtual call, and this is on the path of
    // every native draw's three constant slices.
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address = 0;
    bool gpu = false;
    bool retained = false;
    size_t bytes = 0;
  };

  Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(D3D12_HEAP_TYPE type, size_t bytes) const;

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  bool gpu_upload_supported_ = false;
  bool gpu_upload_failed_ = false;  // sticky: one failure stops every later attempt
  bool requests_disabled_ = false; // sticky: an unserved request uses only the caller's fallback
  bool shutdown_ = false;
  uint64_t gpu_byte_budget_ = 0;
  // Video memory held by GPU_UPLOAD resources. A normally destroyed page gives
  // its bytes back; resources detached without completion remain counted.
  std::shared_ptr<std::atomic<uint64_t>> gpu_bytes_;
  NativeGpuUploadPoolStats stats_;

  // These base operations require external completion/timeline proofs. Expose
  // only the terminal Shutdown contract for this native constant pool.
  using GraphicsUploadBufferPool::ChangeSubmissionTimeline;
  using GraphicsUploadBufferPool::ClearCache;
};

}  // namespace nb::gpu
