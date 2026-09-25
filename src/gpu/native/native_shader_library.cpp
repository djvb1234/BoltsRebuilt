// See native_shader_library.h.

#include "native_shader_library.h"
#include "native_alpha_test_variant.h"
#include "native_constant_layout.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

#include <rex/logging.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(nb_native_pair_lookup_memo, false, "nb",
                    "Reuse the last ready native shader-pair lookup while retaining compilation, hash, usability and filter checks")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(nb_native_async_shader_reads, true, "nb",
                    "Read first-use native HLSL inside the existing background compile job")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace nb::gpu {

namespace {

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : text) {
    if (c == separator) {
      if (!current.empty()) parts.push_back(current);
      current.clear();
    } else if (c != ' ' && c != '\r') {
      current += c;
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

bool MatchesAny(const std::string& stem, const std::vector<std::string>& needles) {
  for (const std::string& needle : needles) {
    if (stem.find(needle) != std::string::npos) return true;
  }
  return false;
}

// Background compiles at once; shaders past the cap wait and their draws stay emulated meanwhile. One
// more may run beside them: a pixel shader's form without AlphaTest, which never takes one of these slots
// (native_alpha_test_variant.h). Computed once: a draw waiting for that slot asks on every draw.
uint32_t MaxCompilesInFlight() {
  static const uint32_t max_compiles = [] {
    const uint32_t threads = std::thread::hardware_concurrency();
    return threads > 4 ? threads / 2 : 2;
  }();
  return max_compiles;
}

std::string HexHash(uint64_t hash) {
  char text[17];
  std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(hash));
  return text;
}

// One map holds both stages, so mix the stage into the key.
uint64_t ShaderKey(uint64_t hash, bool vertex) { return vertex ? hash : ~hash; }

uint64_t PairKey(uint64_t vs_hash, uint64_t ps_hash) {
  return vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull) ^ (ps_hash >> 29);
}

}  // namespace

bool NativeShaderLibrary::ParseMeta(const std::string& path, StageShader& shader) const {
  std::string text;
  if (!ReadFile(path, text)) {
    return false;
  }
  std::istringstream lines(text);
  std::string line;
  bool have_hash = false;
  bool have_texture_dimensions = false;
  NativeConstantLayoutMetadata<NativeGeometryPass::ConstantRun> constant_layout;
  NativeAlphaTestVariantSidecar alpha_test_variant;
  while (std::getline(lines, line)) {
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    if (!value.empty() && value.back() == '\r') value.pop_back();
    alpha_test_variant.Observe(key, value);
    if (key == "hash") {
      shader.hash = std::strtoull(value.c_str(), nullptr, 16);
      have_hash = true;
    } else if (key == "interp") {
      shader.interpolators = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "consts") {
      if (!constant_layout.SetUpperBound(value)) return false;
    } else if (key == "cruns") {
      if (!constant_layout.SetRuns(value)) return false;
    } else if (key == "writes_depth") {
      shader.writes_depth = value == "1";
    } else if (key == "tex") {
      for (const std::string& item : Split(value, ',')) {
        shader.texture_fetch_constants.push_back(
            static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 10)));
      }
    } else if (key == "texdim") {
      if (have_texture_dimensions) return false;
      have_texture_dimensions = true;
      for (const std::string& item : Split(value, ',')) {
        if (item != "2" && item != "3" && item != "4") return false;
        shader.texture_dimensions.push_back(static_cast<uint32_t>(item[0] - '0'));
      }
    } else if (key == "streams") {
      for (const std::string& item : Split(value, ',')) {
        const size_t colon = item.find(':');
        if (colon == std::string::npos) return false;
        Stream stream;
        stream.fetch_constant = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 10));
        stream.stride_dwords = static_cast<uint32_t>(std::strtoul(item.c_str() + colon + 1, nullptr, 10));
        if (stream.fetch_constant >= 96 || stream.stride_dwords > 255) return false;
        shader.streams.push_back(stream);
      }
    }
  }
  if (!constant_layout.valid()) return false;
  shader.constant_count = constant_layout.upper_bound;
  shader.has_packed_layout = constant_layout.has_packed_layout;
  shader.packed_constants = constant_layout.packed_registers;
  shader.constant_runs = std::move(constant_layout.runs);
  shader.no_alpha_test_eligible = !shader.vertex && alpha_test_variant.eligible();
  // Sidecars generated before dimensional texture bindings described only 2D instructions.
  if (!have_texture_dimensions) {
    shader.texture_dimensions.assign(shader.texture_fetch_constants.size(), 2u);
  }
  if (shader.texture_dimensions.size() != shader.texture_fetch_constants.size()) return false;
  // The limits the generated HLSL and NativeGeometryPass::RootConstants are built for.
  const size_t texture_limit = shader.vertex
                                   ? NativeGeometryPass::kTextureSlots - NativeGeometryPass::kVertexTextureSlotBase
                                   : NativeGeometryPass::kVertexTextureSlotBase;
  if (!have_hash || shader.texture_fetch_constants.size() > texture_limit ||
      shader.streams.size() > NativeGeometryPass::kMaxVertexStreams ||
      shader.interpolators > 16) {
    return false;
  }
  return true;
}

size_t NativeShaderLibrary::Load(const std::string& directory, const std::string& filter,
                                  const std::string& exclude, bool effective_samplers, bool direct_guest_reads) {
  ready_pair_memo_.Reset();
  effective_samplers_ = effective_samplers;
  direct_guest_reads_ = direct_guest_reads;
  directory_ = directory;
  shaders_.clear();
  pairs_.clear();
  compiling_.clear();
  no_alpha_test_compiling_ = 0;
  filters_ = Split(filter, ',');
  excludes_ = Split(exclude, ',');
  vertex_shaders_ = pixel_shaders_ = 0;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    REXLOG_WARN("rexgpu-nb: native shader library: '{}' is not a directory; no generated shaders", directory);
    return 0;
  }
  size_t bad = 0;
  for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
    if (!item.is_regular_file() || item.path().extension() != ".meta") continue;
    const std::string stem = item.path().stem().string();
    const bool vertex = stem.rfind("vs_", 0) == 0;
    if (!vertex && stem.rfind("ps_", 0) != 0) continue;
    auto shader = std::make_unique<StageShader>();
    shader->stem = stem;
    shader->vertex = vertex;
    if (!ParseMeta(item.path().string(), *shader)) {
      if (bad++ < 4) {
        REXLOG_WARN("rexgpu-nb: native shader library: bad sidecar {}", item.path().string());
      }
      continue;
    }
    (vertex ? vertex_shaders_ : pixel_shaders_)++;
    shaders_[ShaderKey(shader->hash, vertex)] = std::move(shader);
  }
  REXLOG_INFO("rexgpu-nb: native shader library: {} vertex + {} pixel shaders from {}{}{}", vertex_shaders_,
              pixel_shaders_, directory, bad ? ", bad sidecars: " : "",
              bad ? std::to_string(bad) : std::string());
  if (!filters_.empty() || !excludes_.empty()) {
    REXLOG_INFO("rexgpu-nb: native shader library: pair filter '{}', exclude '{}'", filter, exclude);
  }
  return shaders_.size();
}

NativeShaderLibrary::StageShader* NativeShaderLibrary::Find(uint64_t hash, bool vertex) {
  auto it = shaders_.find(ShaderKey(hash, vertex));
  return it == shaders_.end() ? nullptr : it->second.get();
}

bool NativeShaderLibrary::PairEnabled(const std::string& stem) const {
  return (filters_.empty() || MatchesAny(stem, filters_)) && !MatchesAny(stem, excludes_);
}

void NativeShaderLibrary::ReapCompiles() {
  for (size_t i = 0; i < compiling_.size();) {
    Variant* variant = compiling_[i];
    if (variant->compile.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++i;
      continue;
    }
    try {
      Compiled compiled = variant->compile.get();
      variant->blob = std::move(compiled.blob);
      variant->source_hash = compiled.source_hash;
    } catch (const std::exception& error) {
      REXLOG_WARN("rexgpu-nb: native shader library: background source read/compile failed: {}", error.what());
      variant->blob.Reset();
    } catch (...) {
      REXLOG_WARN("rexgpu-nb: native shader library: background source read/compile failed");
      variant->blob.Reset();
    }
    variant->state = variant->blob ? State::kReady : State::kFailed;
    if (variant->no_alpha_test) --no_alpha_test_compiling_;
    compiling_[i] = compiling_.back();
    compiling_.pop_back();
  }
}

NativeShaderLibrary::Variant* NativeShaderLibrary::EnsureCompiled(StageShader& shader,
                                                                  uint32_t interpolators, bool alpha_test) {
  const uint32_t key = shader.vertex ? interpolators : (alpha_test ? 0 : kNoAlphaTestKey);
  auto it = shader.variants.find(key);
  if (it == shader.variants.end()) {
    const uint32_t max_compiles = MaxCompilesInFlight();
    // A form without AlphaTest is optional (its draws already have the generated form), so it never
    // takes one of the cap's slots: at most one runs beside them, started only while first-use
    // compiles are below the cap.
    if (!alpha_test) {
      if (!NativeNoAlphaTestCompileAdmitted(compiling_.size(), no_alpha_test_compiling_, max_compiles)) {
        return nullptr;  // the draw keeps the generated form meanwhile
      }
    } else if (!NativeBaseSlotFree(compiling_.size(), no_alpha_test_compiling_, max_compiles)) {
      return nullptr;  // stays emulated until a compile slot frees up
    }
    // The form without AlphaTest must be built from the same body as the generated form this pass
    // already draws with; a library regenerated mid-run leaves it unavailable instead.
    size_t expected_hash = 0;
    if (!alpha_test) {
      const auto base = shader.variants.find(0);
      if (base != shader.variants.end() && base->second->state == State::kReady) {
        expected_hash = base->second->source_hash;
      }
    }
    const bool hash_source = !shader.vertex && alpha_test && shader.no_alpha_test_eligible;
    const bool async_reads = REXCVAR_GET(nb_native_async_shader_reads);
    std::string source_path = (std::filesystem::path(directory_) / (shader.stem + ".hlsl")).string();
    std::string hlsl;
    if (!async_reads && !ReadFile(source_path, hlsl)) {
      REXLOG_WARN("rexgpu-nb: native shader library: {}: source missing next to the sidecar", shader.stem);
      auto failed = std::make_unique<Variant>();
      failed->state = State::kFailed;
      Variant* result = failed.get();
      shader.variants[key] = std::move(failed);
      return result;
    }
    auto variant = std::make_unique<Variant>();
    variant->no_alpha_test = !alpha_test;
    const bool vertex = shader.vertex;
    const uint32_t compile_interpolators = vertex ? interpolators : 0;
    const uint32_t packed = shader.packed_constants;
    const bool effective_samplers = effective_samplers_;
    const bool direct_guest_reads = direct_guest_reads_;
    // Allocate tracking storage before starting work, and publish ownership
    // before its raw pointer. A failed map allocation must not leave a dangling
    // entry for ReapCompiles; the local future still joins its job on unwinding.
    compiling_.reserve(max_compiles + 1);
    variant->compile =
        std::async(std::launch::async, [name = alpha_test ? shader.stem : shader.stem + "_noalphatest", path = std::move(source_path), source = std::move(hlsl), vertex, compile_interpolators, packed, effective_samplers, direct_guest_reads, async_reads, alpha_test, hash_source, expected_hash]() mutable {
          if (async_reads && !ReadFile(path, source)) {
            REXLOG_WARN("rexgpu-nb: native shader library: {}: source missing next to the sidecar", name);
            return Compiled{};
          }
          Compiled compiled;
          if (hash_source || !alpha_test) {
            compiled.source_hash = std::hash<std::string>{}(source);
          }
          if (!alpha_test && (expected_hash == 0 || compiled.source_hash != expected_hash)) {
            REXLOG_WARN("rexgpu-nb: native shader library: {}: source differs from the generated form's; not compiled", name);
            return Compiled{};
          }
          compiled.blob = NativeGeometryPass::CompileShader(name.c_str(), source, vertex, compile_interpolators, packed, effective_samplers, direct_guest_reads, alpha_test);
          return compiled;
        });
    variant->state = State::kCompiling;
    Variant* result = variant.get();
    shader.variants[key] = std::move(variant);
    compiling_.push_back(result);
    no_alpha_test_compiling_ += alpha_test ? 0 : 1;
    return result;
  }
  return it->second.get();
}

NativeShaderLibrary::Lookup NativeShaderLibrary::FindPair(uint64_t vs_hash, uint64_t ps_hash,
                                                         ID3D12Device* device, Pair** pair_out) {
  if (!compiling_.empty()) {
    ReapCompiles();
  }
  const bool memo_enabled = REXCVAR_GET(nb_native_pair_lookup_memo);
  if (memo_enabled) {
    if (Pair* pair = ready_pair_memo_.Find(vs_hash, ps_hash)) {
      if (pair->vs->hash != vs_hash || (pair->ps ? pair->ps->hash : 0) != ps_hash) {
        return Lookup::kUnusable;
      }
      if (!pair->usable) {
        return Lookup::kUnusable;
      }
      if (!PairEnabled(pair->stem)) {
        return Lookup::kDisabled;
      }
      *pair_out = pair;
      return Lookup::kReady;
    }
  }
  const uint64_t pair_key = PairKey(vs_hash, ps_hash);
  auto pair_it = pairs_.find(pair_key);
  if (pair_it != pairs_.end()) {
    Pair& pair = *pair_it->second;
    if (pair.vs->hash != vs_hash || (pair.ps ? pair.ps->hash : 0) != ps_hash) {
      return Lookup::kUnusable;  // hash fold collision: leave this draw emulated
    }
    if (!pair.usable) {
      return Lookup::kUnusable;
    }
    if (!PairEnabled(pair.stem)) {
      return Lookup::kDisabled;
    }
    *pair_out = &pair;
    if (memo_enabled) ready_pair_memo_.Publish(vs_hash, ps_hash, &pair);
    return Lookup::kReady;
  }

  StageShader* vs = Find(vs_hash, true);
  StageShader* ps = ps_hash ? Find(ps_hash, false) : nullptr;
  if (!vs || (ps_hash && !ps)) {
    return Lookup::kUnknownShader;
  }
  const std::string stem = HexHash(vs_hash) + "_" + HexHash(ps_hash);
  if (!PairEnabled(stem)) {
    return Lookup::kDisabled;
  }
  // The vertex shader declares whatever the pixel shader reads, even past what it exports: on the guest
  // those registers are undefined, here they are zero, and the signatures have to line up either way.
  const uint32_t interpolators = std::max(vs->interpolators, ps ? ps->interpolators : 0u);
  Variant* vs_variant = EnsureCompiled(*vs, interpolators);
  Variant* ps_variant = ps ? EnsureCompiled(*ps, 0) : nullptr;
  if (!vs_variant || (ps && !ps_variant)) {
    return Lookup::kNotReady;
  }
  if (vs_variant->state == State::kFailed || (ps_variant && ps_variant->state == State::kFailed)) {
    return Lookup::kUnusable;
  }
  if (vs_variant->state != State::kReady || (ps_variant && ps_variant->state != State::kReady)) {
    return Lookup::kNotReady;
  }
  if (!root_signature_) {
    root_signature_ = NativeGeometryPass::CreateRootSignature(device);
    if (!root_signature_) {
      return Lookup::kUnusable;
    }
  }
  if (!root_cbv_signature_) {
    root_cbv_signature_ = NativeGeometryPass::CreateRootSignature(device, true);
  }

  auto pair = std::make_unique<Pair>();
  pair->vs = vs;
  pair->ps = ps;
  pair->stem = stem;
  if (!pair->pass.Initialize(device, stem.c_str(), vs_variant->blob.Get(),
                             ps_variant ? ps_variant->blob.Get() : nullptr, root_signature_.Get(),
                             root_cbv_signature_.Get(), direct_guest_reads_)) {
    pair->usable = false;
  }
  Pair* result = pair.get();
  pairs_[pair_key] = std::move(pair);
  if (!result->usable) {
    return Lookup::kUnusable;
  }
  *pair_out = result;
  if (memo_enabled) ready_pair_memo_.Publish(vs_hash, ps_hash, result);
  return Lookup::kReady;
}

NativeShaderLibrary::VariantStatus NativeShaderLibrary::RequestNoAlphaTest(Pair& pair) {
  if (pair.pass.has_no_alpha_test_pixel_shader()) {
    return pair.pass.no_alpha_test_failed() ? VariantStatus::kUnavailable : VariantStatus::kReady;
  }
  if (!pair.ps || !pair.ps->no_alpha_test_eligible || pair.no_alpha_test_refused) {
    return VariantStatus::kUnavailable;
  }
  // Pairs built before the compile finished (or before any draw asked) pick it up here, whichever
  // FindPair path returned them. The Variant pointer stays valid for the library's lifetime.
  if (!pair.no_alpha_test) {
    // Another pair with this pixel shader may have started or finished its form already: adopt it.
    // Only a new compile waits for the one variant compile slot, the common case during warm-up, and
    // that wait is answered before the shaders_ lookup a start needs.
    const auto existing = pair.ps->variants.find(kNoAlphaTestKey);
    if (existing != pair.ps->variants.end()) {
      pair.no_alpha_test = existing->second.get();
    } else if (!NativeNoAlphaTestCompileAdmitted(compiling_.size(), no_alpha_test_compiling_,
                                                 MaxCompilesInFlight())) {
      return VariantStatus::kPending;
    } else {
      StageShader* ps = Find(pair.ps->hash, false);  // pair.ps itself, as the library's mutable entry
      pair.no_alpha_test = ps ? EnsureCompiled(*ps, 0, false) : nullptr;
      if (!pair.no_alpha_test) {
        return VariantStatus::kPending;
      }
    }
  }
  if (pair.no_alpha_test->state == State::kCompiling) {
    return VariantStatus::kPending;
  }
  if (pair.no_alpha_test->state == State::kReady &&
      pair.pass.SetNoAlphaTestPixelShader(pair.no_alpha_test->blob.Get())) {
    return VariantStatus::kReady;
  }
  pair.no_alpha_test_refused = true;
  return VariantStatus::kUnavailable;
}

}  // namespace nb::gpu
