// See nb_cp_records.h.

#include "nb_cp_records.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(nb_cp_records_enable, false, "nb",
                    "Write CP-side draw/resolve records (JSON lines) for the frame inventory");
REXCVAR_DEFINE_STRING(nb_cp_records_file, "nb_records/cp_records.jsonl", "nb",
                      "Path of the CP record file (relative to the working directory)");
REXCVAR_DEFINE_UINT32(nb_cp_records_period, 600, "nb",
                      "Record the first nb_cp_records_frames frames of every period-frame window (0 = every frame)");
REXCVAR_DEFINE_UINT32(nb_cp_records_frames, 2, "nb", "Consecutive frames recorded per window");

namespace nb::gpu {

namespace {

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

float BitsToFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

}  // namespace

NbCpRecorder::~NbCpRecorder() {
  if (file_) {
    std::fflush(file_);
    std::fclose(file_);
  }
  delete[] buffer_;
}

bool NbCpRecorder::WantsFrame(uint64_t frame) const {
  if (!REXCVAR_GET(nb_cp_records_enable)) return false;
  const uint32_t period = REXCVAR_GET(nb_cp_records_period);
  if (period == 0) return true;
  return (frame % period) < REXCVAR_GET(nb_cp_records_frames);
}

NbCpRecord* NbCpRecorder::NextSlot() {
  if (!buffer_) {
    buffer_ = new NbCpRecord[kFrameBufferMax];
  }
  if (buffered_ >= kFrameBufferMax) {
    ++dropped_;
    return nullptr;
  }
  return &buffer_[buffered_++];
}

void NbCpRecorder::FillState(NbCpRecord* rec, const rex::graphics::RegisterFile* regs,
                             const rex::graphics::Shader* vs, const rex::graphics::Shader* ps) {
  using namespace rex::graphics;
  rec->ts_ns = NowNs();
  rec->vs_ucode_hash = vs ? vs->ucode_data_hash() : 0;
  rec->ps_ucode_hash = ps ? ps->ucode_data_hash() : 0;
  if (!regs) return;

  const auto surface = regs->Get<reg::RB_SURFACE_INFO>();
  rec->rb_surface_pitch = surface.surface_pitch;
  rec->rb_surface_msaa = static_cast<uint32_t>(surface.msaa_samples);
  const Register color_regs[4] = {XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO, XE_GPU_REG_RB_COLOR2_INFO,
                                  XE_GPU_REG_RB_COLOR3_INFO};
  for (int i = 0; i < 4; ++i) {
    const auto color = regs->Get<reg::RB_COLOR_INFO>(color_regs[i]);
    rec->rb_color_base[i] = color.color_base | (color.color_base_bit_11 << 11);
    rec->rb_color_format[i] = static_cast<uint32_t>(color.color_format);
  }
  const auto depth = regs->Get<reg::RB_DEPTH_INFO>();
  rec->rb_depth_base = depth.depth_base | (depth.depth_base_bit_11 << 11);
  rec->rb_depth_format = static_cast<uint32_t>(depth.depth_format);
  rec->rb_edram_mode = static_cast<uint32_t>(regs->Get<reg::RB_MODECONTROL>().edram_mode);
  rec->rb_depthcontrol = (*regs)[XE_GPU_REG_RB_DEPTHCONTROL];
  rec->rb_blendcontrol0 = (*regs)[XE_GPU_REG_RB_BLENDCONTROL0];
  rec->rb_colorcontrol = (*regs)[XE_GPU_REG_RB_COLORCONTROL];
  rec->pa_su_sc_mode_cntl = (*regs)[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  rec->pa_cl_vte_cntl = (*regs)[XE_GPU_REG_PA_CL_VTE_CNTL];
  rec->vport_xscale = BitsToFloat((*regs)[XE_GPU_REG_PA_CL_VPORT_XSCALE]);
  rec->vport_xoffset = BitsToFloat((*regs)[XE_GPU_REG_PA_CL_VPORT_XOFFSET]);
  rec->vport_yscale = BitsToFloat((*regs)[XE_GPU_REG_PA_CL_VPORT_YSCALE]);
  rec->vport_yoffset = BitsToFloat((*regs)[XE_GPU_REG_PA_CL_VPORT_YOFFSET]);
  rec->scissor_tl = (*regs)[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  rec->scissor_br = (*regs)[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];

  // The bindings exist only after translation; with async_shader_compilation on, a shader seen for
  // the first time is still untranslated at its first draws (docs/research/kimi_swarm23: T review).
  // Record with async_shader_compilation = false for a complete inventory.
  if (vs) {
    for (const auto& binding : vs->vertex_bindings()) {
      if (rec->vfetch_count >= 8) break;
      const auto fetch = regs->GetVertexFetch(binding.fetch_constant);
      auto& out = rec->vfetch[rec->vfetch_count++];
      out.fetch_constant = binding.fetch_constant;
      out.base = fetch.address * 4;
      out.size_bytes = fetch.size * 4;
      out.stride_words = binding.stride_words;
    }
  }
  if (ps) {
    for (const auto& binding : ps->texture_bindings()) {
      if (rec->tfetch_count >= 8) break;
      const auto fetch = regs->GetTextureFetch(binding.fetch_constant);
      auto& out = rec->tfetch[rec->tfetch_count++];
      out.fetch_constant = binding.fetch_constant;
      out.base = fetch.base_address << 12;
      out.mip_address = fetch.mip_address << 12;
      out.format = static_cast<uint32_t>(fetch.format);
      out.dimension = static_cast<uint32_t>(fetch.dimension);
      switch (fetch.dimension) {
        case xenos::DataDimension::k1D:
          out.width = fetch.size_1d.width + 1;
          out.height = 1;
          break;
        case xenos::DataDimension::k3D:
          out.width = fetch.size_3d.width + 1;
          out.height = fetch.size_3d.height + 1;
          break;
        default:  // 2D and cube use size_2d
          out.width = fetch.size_2d.width + 1;
          out.height = fetch.size_2d.height + 1;
          break;
      }
    }
  }
}

void NbCpRecorder::RecordDraw(uint64_t frame, rex::graphics::xenos::PrimitiveType prim_type,
                              uint32_t index_count, uint32_t ib_guest_base, uint32_t ib_count,
                              uint32_t ib_format, bool major_mode_explicit,
                              const rex::graphics::RegisterFile* regs, const rex::graphics::Shader* vs,
                              const rex::graphics::Shader* ps) {
  NbCpRecord* rec = NextSlot();
  if (!rec) return;
  *rec = NbCpRecord{};
  rec->kind = 0;
  rec->frame = frame;
  rec->sequence = sequence_++;
  rec->copies_before = copies_;
  rec->prim_type = static_cast<uint32_t>(prim_type);
  rec->index_count = index_count;
  rec->ib_guest_base = ib_guest_base;
  rec->ib_count = ib_count;
  rec->ib_format = ib_format;
  rec->major_mode_explicit = major_mode_explicit ? 1u : 0u;
  FillState(rec, regs, vs, ps);
}

void NbCpRecorder::RecordCopy(uint64_t frame, const rex::graphics::RegisterFile* regs,
                              const rex::graphics::Shader* vs, const rex::graphics::Shader* ps) {
  NbCpRecord* rec = NextSlot();
  if (rec) {
    *rec = NbCpRecord{};
    rec->kind = 1;
    rec->frame = frame;
    rec->sequence = sequence_;
    rec->copies_before = copies_;
    FillState(rec, regs, vs, ps);
  }
  ++copies_;
}

void NbCpRecorder::EndFrameAndFlush(uint64_t frame) {
  if (buffered_) {
    if (buffered_ < kFrameBufferMax) {
      NbCpRecord* stats = &buffer_[buffered_++];
      *stats = NbCpRecord{};
      stats->kind = 2;
      stats->frame = frame;
      stats->sequence = sequence_;
      stats->copies_before = copies_;
      stats->ts_ns = NowNs();
    }
    if (!file_ && !open_attempted_) {
      open_attempted_ = true;
      OpenFile();
    }
    if (file_) {
      for (size_t i = 0; i < buffered_; ++i) WriteRecord(buffer_[i]);
      std::fflush(file_);
    }
  }
  buffered_ = 0;
  sequence_ = 0;
  copies_ = 0;
}

bool NbCpRecorder::OpenFile() {
  const std::string path = REXCVAR_GET(nb_cp_records_file);
  const std::filesystem::path fs_path(path);
  if (fs_path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(fs_path.parent_path(), ec);
  }
  file_ = std::fopen(path.c_str(), "a");
  if (file_) {
    REXLOG_INFO("rexgpu-nb: CP records -> {}", path);
  } else {
    REXLOG_WARN("rexgpu-nb: cannot open {} for CP records", path);
  }
  return file_ != nullptr;
}

void NbCpRecorder::WriteRecord(const NbCpRecord& rec) {
  char buf[3072];
  char* p = buf;
  char* const end = buf + sizeof(buf);
  auto put = [&](const char* fmt, auto... args) {
    if (p < end) {
      const int n = std::snprintf(p, static_cast<size_t>(end - p), fmt, args...);
      if (n > 0) p += (n < end - p) ? n : (end - p - 1);
    }
  };
  if (rec.kind == 2) {
    put("{\"type\":\"cp_frame_stats\",\"frame\":%llu,\"draws\":%u,\"copies\":%u,\"ts_ns\":%llu}\n",
        static_cast<unsigned long long>(rec.frame), rec.sequence, rec.copies_before,
        static_cast<unsigned long long>(rec.ts_ns));
    std::fwrite(buf, 1, static_cast<size_t>(p - buf), file_);
    return;
  }
  put("{\"type\":\"%s\",\"frame\":%llu,\"seq\":%u,\"copies_before\":%u,\"prim\":%u,\"index_count\":%u,"
      "\"ib_base\":\"0x%08X\",\"ib_count\":%u,\"ib_format\":%u,\"mm_explicit\":%u,"
      "\"vs_ucode\":\"%016llX\",\"ps_ucode\":\"%016llX\",",
      rec.kind == 0 ? "cp_draw" : "cp_copy", static_cast<unsigned long long>(rec.frame), rec.sequence,
      rec.copies_before, rec.prim_type, rec.index_count, rec.ib_guest_base, rec.ib_count, rec.ib_format,
      rec.major_mode_explicit, static_cast<unsigned long long>(rec.vs_ucode_hash),
      static_cast<unsigned long long>(rec.ps_ucode_hash));
  put("\"rb\":{\"pitch\":%u,\"msaa\":%u,\"color\":[[%u,%u],[%u,%u],[%u,%u],[%u,%u]],\"depth\":[%u,%u],"
      "\"edram_mode\":%u,\"depthcontrol\":\"0x%08X\",\"blend0\":\"0x%08X\",\"colorcontrol\":\"0x%08X\","
      "\"su_sc_mode\":\"0x%08X\",\"vte\":\"0x%08X\"},",
      rec.rb_surface_pitch, rec.rb_surface_msaa, rec.rb_color_base[0], rec.rb_color_format[0], rec.rb_color_base[1],
      rec.rb_color_format[1], rec.rb_color_base[2], rec.rb_color_format[2], rec.rb_color_base[3],
      rec.rb_color_format[3], rec.rb_depth_base, rec.rb_depth_format, rec.rb_edram_mode, rec.rb_depthcontrol,
      rec.rb_blendcontrol0, rec.rb_colorcontrol, rec.pa_su_sc_mode_cntl, rec.pa_cl_vte_cntl);
  put("\"viewport\":[%.3f,%.3f,%.3f,%.3f],\"scissor\":[\"0x%08X\",\"0x%08X\"],\"vfetch\":[",
      static_cast<double>(rec.vport_xscale), static_cast<double>(rec.vport_xoffset),
      static_cast<double>(rec.vport_yscale), static_cast<double>(rec.vport_yoffset), rec.scissor_tl,
      rec.scissor_br);
  for (uint32_t i = 0; i < rec.vfetch_count; ++i) {
    put("%s{\"fc\":%u,\"base\":\"0x%08X\",\"size\":%u,\"stride_words\":%u}", i ? "," : "",
        rec.vfetch[i].fetch_constant, rec.vfetch[i].base, rec.vfetch[i].size_bytes, rec.vfetch[i].stride_words);
  }
  put("],\"tfetch\":[");
  for (uint32_t i = 0; i < rec.tfetch_count; ++i) {
    put("%s{\"fc\":%u,\"base\":\"0x%08X\",\"w\":%u,\"h\":%u,\"format\":%u,\"dim\":%u,\"mip\":\"0x%08X\"}",
        i ? "," : "", rec.tfetch[i].fetch_constant, rec.tfetch[i].base, rec.tfetch[i].width, rec.tfetch[i].height,
        rec.tfetch[i].format, rec.tfetch[i].dimension, rec.tfetch[i].mip_address);
  }
  put("],\"ts_ns\":%llu}\n", static_cast<unsigned long long>(rec.ts_ns));
  std::fwrite(buf, 1, static_cast<size_t>(p - buf), file_);
}

}  // namespace nb::gpu
