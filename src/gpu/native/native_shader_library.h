// nb - the library of generated native shaders: our HLSL translation of the guest's own vertex and pixel
// shaders, produced offline by tools/ucode2hlsl.py from the SDK's ucode disassembly of the user's dump.
//
// Each guest shader is translated on its own, not per (vertex, pixel) pair: the interpolator layout is
// fixed by semantic index, the pixel shader's texture fetch constants live in root-constant slots 0..15
// and the vertex shader's in 16..19, and param_gen comes from the draw's registers. Any vertex shader can then
// be paired with any pixel shader, so the library covers combinations no capture happened to record.
//
// The library is a directory (nb_native_shader_dir, default <exe dir>/native_shaders) of
// vs_<hash>.hlsl / ps_<hash>.hlsl with a .meta sidecar each. Shaders compile in the background the first
// time a draw needs them (D3DCompile of a 300-line pixel shader takes tens of milliseconds; compiling a
// frame's worth synchronously would stall the command processor for seconds), and until a compile lands
// the draw stays emulated. A pair is built the first time it is drawn and keeps its own pipeline cache.

#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "native_geometry_pass.h"
#include "native_ready_pair_memo.h"

namespace nb::gpu {

class NativeShaderLibrary {
 public:
  struct Stream {
    uint32_t fetch_constant;  // vertex fetch constant index 0..95
    uint32_t stride_dwords;   // from the vfetch_full instruction, the authority (not the fetch constant)
  };
  enum class State { kIdle, kCompiling, kReady, kFailed };

  // What a background compile hands back: the blob, and for a pixel shader eligible for the form
  // without AlphaTest, std::hash of the body it compiled, which that form must be built from too.
  struct Compiled {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    size_t source_hash = 0;  // 0: not recorded
  };
  // One compiled form of a shader. A vertex shader has one per interpolator count a pair asks for
  // (almost always one); a pixel shader has its generated form and, on request, the form without its
  // alpha-test discard (native_alpha_test_variant.h).
  struct Variant {
    State state = State::kIdle;
    std::future<Compiled> compile;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    size_t source_hash = 0;      // Compiled::source_hash
    bool no_alpha_test = false;  // compiles beside the first-use cap (EnsureCompiled)
  };
  // The pixel-shader variant key of the form without AlphaTest; the generated form keeps key 0.
  static constexpr uint32_t kNoAlphaTestKey = 1;

  // One translated guest shader.
  struct StageShader {
    uint64_t hash = 0;
    std::string stem;                               // "vs_<hash>" / "ps_<hash>"
    bool vertex = false;
    std::vector<uint32_t> texture_fetch_constants;  // in slot order (vertex: slots 16.., pixel: slots 0..)
    // Instruction dimensions (2 = 2D, 3 = stacked, 4 = cube), parallel to fetch constants. A fetch constant used by both
    // dimensions occupies two slots because their coordinate and layer metadata differ.
    std::vector<uint32_t> texture_dimensions;
    std::vector<Stream> streams;                    // vertex shaders only
    uint32_t interpolators = 0;                     // written (vertex) or read (pixel)
    uint32_t constant_count = 256;                  // highest float4 constant the shader reads, plus one
    // The constant registers it reads, as [first, last] spans: only these are uploaded per draw, and
    // their total sizes the stage's constant buffer.
    std::vector<NativeGeometryPass::ConstantRun> constant_runs;
    uint32_t packed_constants = 0;
    bool has_packed_layout = false;  // cruns was present; empty means no guest float reads
    bool writes_depth = false;                      // pixel shaders only
    // Pixel shaders only: the sidecar says kills=0 and writes_depth=0, so dropping the alpha-test
    // discard leaves a shader the driver can run after an early depth/stencil test.
    bool no_alpha_test_eligible = false;
    // By interpolator count (vertex), or 0 / kNoAlphaTestKey (pixel).
    std::unordered_map<uint32_t, std::unique_ptr<Variant>> variants;
  };

  // One (vertex, pixel) combination actually drawn, with its own pipeline cache.
  struct Pair {
    const StageShader* vs = nullptr;
    const StageShader* ps = nullptr;  // null for a depth-only draw (the guest bound no pixel shader)
    std::string stem;                 // "<vs hash>_<ps hash>", the pass name in logs
    bool usable = true;
    NativeGeometryPass pass;
    uint32_t draws = 0;
    uint32_t skips = 0;
    // The pixel shader without AlphaTest, requested by the first draw that selects it and added to
    // `pass` once compiled (RequestNoAlphaTest). Failing it disables only this form, never the pair.
    Variant* no_alpha_test = nullptr;
    bool no_alpha_test_refused = false;
  };

  // Reads every .meta in `directory`. `filter` (comma-separated substrings of "<vs>_<ps>", empty = all)
  // marks the pairs that may be drawn; `exclude` (same form) removes pairs from that set.
  size_t Load(const std::string& directory, const std::string& filter, const std::string& exclude,
              bool effective_samplers = true, bool direct_guest_reads = false);

  enum class Lookup { kReady, kUnknownShader, kNotReady, kUnusable, kDisabled };
  // Finds (and on first use builds) the pass for this shader pair. `pair_out` is set only for kReady.
  Lookup FindPair(uint64_t vs_hash, uint64_t ps_hash, ID3D12Device* device, Pair** pair_out);

  enum class VariantStatus { kReady, kPending, kUnavailable };
  // For a draw that selected the pixel shader without AlphaTest (nb_native_alpha_test_variant): adds
  // it to the pair's pass once compiled, starting that compile on first use. kPending and kUnavailable
  // leave the draw on the pair's generated pixel shader; neither changes the pair's usability.
  VariantStatus RequestNoAlphaTest(Pair& pair);

  size_t shader_count() const { return shaders_.size(); }
  const std::string& directory() const { return directory_; }
  const NativeReadyPairMemoStats& pair_lookup_memo_stats() const {
    return ready_pair_memo_.stats();
  }

 private:
  StageShader* Find(uint64_t hash, bool vertex);
  bool ParseMeta(const std::string& path, StageShader& shader) const;
  // Starts the background compile if a slot is free, and finishes any that are done. `interpolators` is
  // the count a vertex shader must declare for the pair being built; `alpha_test` false asks for a pixel
  // shader's form without AlphaTest, which runs beside the cap and never takes a first-use slot.
  Variant* EnsureCompiled(StageShader& shader, uint32_t interpolators, bool alpha_test = true);
  void ReapCompiles();
  bool PairEnabled(const std::string& stem) const;

  std::string directory_;
  bool effective_samplers_ = true;
  bool direct_guest_reads_ = false;
  // Keyed by the ucode hash mixed with the stage, so a vertex and a pixel shader cannot collide.
  std::unordered_map<uint64_t, std::unique_ptr<StageShader>> shaders_;
  std::unordered_map<uint64_t, std::unique_ptr<Pair>> pairs_;
  std::vector<Variant*> compiling_;
  uint32_t no_alpha_test_compiling_ = 0;  // the no_alpha_test entries of compiling_
  std::vector<std::string> filters_;
  std::vector<std::string> excludes_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_cbv_signature_;
  uint32_t vertex_shaders_ = 0;
  uint32_t pixel_shaders_ = 0;
  NativeReadyPairMemo<Pair> ready_pair_memo_;
};

}  // namespace nb::gpu
