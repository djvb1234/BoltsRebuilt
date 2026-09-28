// Standalone check of the Phase 3 step 5 tile pass counters in
// src/gpu/vendored/include/rex/graphics/phase3_counters.h: pass ordinals,
// repeat-pass draw and index attribution, predication counts, the per-frame
// reset that keeps the other counters cumulative, and the JSON line.
#include "../src/gpu/vendored/include/rex/graphics/phase3_counters.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
using rex::graphics::Phase3Counters;
constexpr uint64_t kDefault = 0xFFFFFFFFull;

uint64_t checks = 0;
void Check(bool condition, const char* why) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << why << '\n';
    std::exit(1);
  }
}

// One frame as the guest issues it: untiled draws, two bands that repeat the
// same scene draws, then the bin state restored and untiled draws again.
void TwoBandFrame(Phase3Counters& c) {
  c.NoteDraw(kDefault, kDefault, 6);  // before tiling
  c.NoteBinSelect(1, 3);
  c.NoteDraw(1, 3, 300);
  c.NoteDraw(1, 3, 36);
  c.NoteBinSelect(1, 3);  // same select again: still pass 1
  c.NoteBinSelect(2, 3);
  c.NoteDraw(2, 3, 300);
  c.NoteDraw(2, 3, 36);
  c.NoteBinSelect(kDefault, kDefault);  // back to defaults
  c.NoteDraw(kDefault, kDefault, 4);    // after tiling (HUD, post)
}
}  // namespace

int main() {
  Check(!Phase3Counters::IsBinningActive(kDefault, kDefault), "defaults are not binning");
  Check(Phase3Counters::IsBinningActive(1, 3), "select 1 under mask 3 bins");
  Check(!Phase3Counters::IsBinningActive(4, 3), "a select outside the mask enables no bin");

  Phase3Counters c;
  TwoBandFrame(c);
  Check(c.tile_pass_ordinal_max == 2, "two passes in the frame");
  Check(c.tiled_draw_count == 4, "only draws under binning are tiled");
  Check(c.tiled_draw_indices == 672, "tiled index sum");
  Check(c.duplicated_draw_across_passes_count == 2, "second band draws are repeats");
  Check(c.repeat_pass_draw_indices == 336, "repeat index sum");

  // Without the dump, the pass state resets per frame but counters accumulate.
  c.ResetFramePassState();
  Check(c.tile_pass_ordinal_max == 0 && c.pass_bin_select == Phase3Counters::kNoBinSelect,
        "frame pass state resets");
  Check(c.tiled_draw_count == 4, "counters survive the pass reset");
  TwoBandFrame(c);
  Check(c.tile_pass_ordinal_max == 2, "second frame starts again at pass 1");
  Check(c.duplicated_draw_across_passes_count == 4, "repeats accumulate across frames");

  // Without a reset the next frame's first band would count as a repeat; the
  // reset above is what prevents that.
  Phase3Counters stale;
  TwoBandFrame(stale);
  stale.NoteBinSelect(1, 3);
  stale.NoteDraw(1, 3, 10);
  Check(stale.duplicated_draw_across_passes_count == 3, "a stale ordinal miscounts, as expected");

  Phase3Counters p;
  p.NotePredicatedPacket(true, false);
  p.NotePredicatedPacket(true, true);
  p.NotePredicatedPacket(false, true);
  Check(p.predicated_packet_count == 3 && p.predicated_packet_skip_count == 2,
        "predicated packets and skips");
  Check(p.predicated_draw_count == 2 && p.predicated_draw_skip_count == 1,
        "predicated draws and skips");

  c.screen_extent_query_count = 7;
  c.cond_write_count = 5;
  std::FILE* f = std::tmpfile();
  Check(f != nullptr, "tmpfile");
  c.WriteJsonLine(f, 42);
  std::rewind(f);
  char line[2048] = {};
  Check(std::fgets(line, sizeof(line), f) != nullptr, "json line written");
  std::fclose(f);
  const char* expected[] = {"\"frame\":42,", "\"duplicated_draws_across_passes\":4,",
                            "\"tiled_draws\":8,", "\"tiled_draw_indices\":1344,",
                            "\"repeat_pass_draw_indices\":672,",
                            "\"tile_pass_ordinal_max\":2,", "\"screen_extent_queries\":7,",
                            "\"cond_writes\":5}"};
  for (const char* key : expected) {
    Check(std::strstr(line, key) != nullptr, key);
  }

  c.Reset();
  Check(c.tiled_draw_count == 0 && c.pass_bin_select == Phase3Counters::kNoBinSelect,
        "Reset clears counters and pass state");

  std::cout << "test_phase3_tile_pass_counters: " << checks << " checks passed\n";
  return 0;
}
