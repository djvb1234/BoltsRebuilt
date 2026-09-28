import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import tile_band_summary as tbs  # noqa: E402


def frame(**kw):
    base = {k: 0 for k in tbs.FIELDS}
    base.update(kw)
    return json.dumps(base)


class TileBandSummaryTest(unittest.TestCase):
    def test_untiled_frames_are_ignored(self):
        self.assertEqual(tbs.summarize(tbs.load([frame(tiled_draws=5)]))["frames"], 0)

    def test_unconditional_replay(self):
        lines = [frame(tile_pass_ordinal_max=2, tiled_draws=400,
                       duplicated_draws_across_passes=200, tiled_draw_indices=90000,
                       repeat_pass_draw_indices=45000)] * 3
        out = tbs.summarize(tbs.load(lines))
        self.assertEqual(out["frames"], 3)
        self.assertTrue(out["pass_model_holds"])
        self.assertAlmostEqual(out["repeat_share_of_tiled_indices"], 0.5)
        self.assertIn("unconditionally", out["verdict"])

    def test_predicated_with_extents(self):
        lines = [frame(tile_pass_ordinal_max=2, tiled_draws=400,
                       duplicated_draws_across_passes=200, predicated_draws=400,
                       screen_extent_queries=30)]
        self.assertIn("honest extents", tbs.summarize(tbs.load(lines))["verdict"])

    def test_pass_model_broken(self):
        lines = [frame(tile_pass_ordinal_max=2, tiled_draws=400,
                       duplicated_draws_across_passes=350)]
        self.assertFalse(tbs.summarize(tbs.load(lines))["pass_model_holds"])


if __name__ == "__main__":
    unittest.main()
