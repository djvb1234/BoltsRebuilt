// Original CPU-test dependency substitute. Never include this directory in a
// production target. The actual deferred command writer/replayer is compiled.
#pragma once
#ifndef NB_NATIVE_DEFERRED_REPLAY_TEST
#error This header is only for the isolated deferred replay CPU control.
#endif
#include <rex/ui/d3d12/d3d12_api.h>
#include <unordered_map>
namespace rex::graphics::d3d12 {
class D3D12CommandProcessor {
 public:
  std::unordered_map<void*, ID3D12PipelineState*> pipelines;
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    const auto found = pipelines.find(handle);
    return found == pipelines.end() ? nullptr : found->second;
  }
};
}
