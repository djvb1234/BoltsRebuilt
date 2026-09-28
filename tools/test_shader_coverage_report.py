"""Synthetic checks for the refusal census and its draw weighting; no game shader data.

Run with: python -m unittest discover -s tools -p test_shader_coverage_report.py
"""

import contextlib
import csv
import io
import pathlib
import tempfile
import unittest
import unittest.mock

import shader_coverage_report as report
import ucode2hlsl

VS_OK = "1111111111111111"
VS_LOOP = "2222222222222222"
PS_OK = "3333333333333333"
PS_JUMP = "4444444444444444"
PS_UNDUMPED = "5555555555555555"


class RefusalCategoryTests(unittest.TestCase):
    def test_per_shader_detail_is_folded(self):
        self.assertEqual(ucode2hlsl.refusal_category("backward jump to L12"), "backward jump")
        self.assertEqual(ucode2hlsl.refusal_category("jump to missing label L3"), "jump to missing label")
        self.assertEqual(ucode2hlsl.refusal_category("operand c[4+aL].xyzw"), "operand relative to aL")
        self.assertEqual(ucode2hlsl.refusal_category("operand i0"), "operand")
        self.assertEqual(ucode2hlsl.refusal_category("stream vf7 fetched with two strides"),
                         "stream fetched with two strides")
        self.assertEqual(ucode2hlsl.refusal_category("fxc: x.hlsl(3,1): error X3000"), "fxc")

    def test_implementable_names_are_kept(self):
        for reason in ("control flow loop_start", "op loop", "vertex format FMT_8", "UseRegisterLOD",
                       "more than 4 texture fetch constants in one vs"):
            self.assertEqual(ucode2hlsl.refusal_category(reason), reason)


class CensusTests(unittest.TestCase):
    def test_census_rows_and_unchanged_summary_tail(self):
        with tempfile.TemporaryDirectory() as tmp:
            dump = pathlib.Path(tmp, "dump")
            dump.mkdir()
            (dump / f"shader_{PS_OK}.ucode.frag").write_text("exec\nmul oC0, r0, c0\n")
            (dump / f"shader_{PS_JUMP}.ucode.frag").write_text("label L1\nexec\nmul oC0, r0, c0\njmp L1\n")
            (dump / f"shader_{VS_LOOP}.ucode.vert").write_text("loop_start i0, L2\n")
            census = pathlib.Path(tmp, "census.csv")
            out = io.StringIO()
            with unittest.mock.patch("sys.argv", ["ucode2hlsl.py", str(dump), "-o", str(pathlib.Path(tmp, "o")),
                                                  "--census", str(census)]), \
                    contextlib.redirect_stdout(out):
                ucode2hlsl.main()
            with open(census, encoding="utf-8", newline="") as f:
                rows = {(r["stage"], r["hash"]): r for r in csv.DictReader(f)}
            self.assertEqual(rows[("ps", PS_OK)]["status"], "ok")
            self.assertEqual(rows[("ps", PS_JUMP)]["category"], "backward jump")
            self.assertEqual(rows[("ps", PS_JUMP)]["reason"], "backward jump to L1")
            self.assertEqual(rows[("vs", VS_LOOP)]["category"], "control flow loop_start")
            text = out.getvalue()
            self.assertIn("vs: 1 stages, 0 ok\n     1 unsupported: control flow loop_start", text)
            # gen_native_shaders.ps1 prints the log's last lines: the old summary still ends it.
            self.assertTrue(text.rstrip().splitlines()[-3].startswith("3 shaders: 1 ok, 2 unusable"))


class DrawWeightTests(unittest.TestCase):
    CENSUS = {("vs", VS_OK): ("ok", ""), ("vs", VS_LOOP): ("unsupported", "control flow loop_start"),
              ("ps", PS_OK): ("ok", ""), ("ps", PS_JUMP): ("unsupported", "backward jump")}

    def test_causes(self):
        self.assertEqual(report.draw_causes(VS_LOOP, PS_OK, self.CENSUS, None),
                         {("vs", VS_LOOP): "control flow loop_start"})
        self.assertEqual(report.draw_causes(VS_OK, PS_UNDUMPED, self.CENSUS, None),
                         {("ps", PS_UNDUMPED): report.NOT_DUMPED})
        self.assertEqual(report.draw_causes(VS_OK, report.NO_PIXEL_SHADER, self.CENSUS, None),
                         {("vs", VS_OK): report.NOT_INSTALLED})
        library = {("vs", VS_OK)}
        self.assertEqual(report.draw_causes(VS_OK, PS_OK, self.CENSUS, library),
                         {("ps", PS_OK): report.NOT_INSTALLED})

    def test_report_weights_by_draws_and_counts_sole_blockers(self):
        missing = [(VS_LOOP, PS_OK, 300)] * 3 + [(VS_LOOP, PS_JUMP, 6), (VS_OK, PS_JUMP, 6)]
        native = [(VS_OK, PS_OK, 36)] * 5
        text = report.build_report(missing, native, self.CENSUS, None)
        self.assertIn("logged draws: 5 native, 5 missing from the library (180 / 912 indices)", text)
        self.assertIn("        4        906       3        900      1  vs: control flow loop_start", text)
        self.assertIn("        2         12       1          6      1  ps: backward jump", text)
        self.assertIn(f"        4        906     2  vs_{VS_LOOP}  control flow loop_start", text)

    def test_log_parsing(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = pathlib.Path(tmp, "nb.log")
            log.write_text(
                f"[info] rexgpu-nb: native library missing at frame 1800: vs {VS_LOOP.lower()} ps {PS_OK}, prim 4 x96\n"
                f"[info] rexgpu-nb: frameseq 1 pair {VS_OK}_{PS_OK} prim 4 x6 zenable 1 zwrite 1 zfunc 3 rtv0 0 dsv 0\n"
                "[info] rexgpu-nb: frame 1800: 10 draws (1 native)\n")
            missing, native = report.read_draws([str(log)])
            self.assertEqual(missing, [(VS_LOOP, PS_OK, 96)])
            self.assertEqual(native, [(VS_OK, PS_OK, 6)])


if __name__ == "__main__":
    unittest.main()
