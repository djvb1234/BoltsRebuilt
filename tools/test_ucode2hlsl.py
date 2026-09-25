"""Original synthetic checks for texture instruction translation; no game shader data.

Run with: python -m unittest discover -s tools -p test_ucode2hlsl.py
"""

import pathlib
import tempfile
import unittest

import ucode2hlsl


class TextureInstructionTests(unittest.TestCase):
    def translate(self, *instructions, stage="ps"):
        translator = ucode2hlsl.Translator(stage, ucode2hlsl.parse_listing("\n".join(instructions)), 0)
        translator.translate_body()
        return translator, "\n".join(translator.lines)

    def test_existing_2d_call_unchanged(self):
        translator, body = self.translate(
            "tfetch2D r1, r2.zy, tf7, MagFilter=linear, OffsetX=-0.5, "
            "OffsetY=1.5, LODBias=-2, UnnormalizedTextureCoords=true")
        self.assertEqual(body,
                         "  float4 f1 = Tex2D(0u, (float2(r2.z, r2.y) / TexSize(0u)), "
                         "2u, float2(-0.5, 1.5), false, -2.0);\n  r1 = f1;")
        self.assertEqual(translator.tex_slots, [7])
        self.assertEqual(translator.tex_dimensions, [2])

    def test_3d_coordinates_and_offsets(self):
        translator, body = self.translate(
            "tfetch3D r1, -r2.xzy, tf5, OffsetX=-0.5, OffsetY=1.5, OffsetZ=2, "
            "LODBias=0.75, UnnormalizedTextureCoords=true")
        self.assertIn("Tex3DTexel(0u, float3((-r2.x), (-r2.z), (-r2.y)), "
                      "0u, float3(-0.5, 1.5, 2.0), false, 0.75)", body)
        self.assertNotIn("TexSize", body)
        self.assertEqual(translator.tex_dimensions, [3])
        # A separate unswizzled coordinate confirms raw Z reaches the texel helper unchanged.
        _, body = self.translate("tfetch3D r1, r0.xyz, tf5, UnnormalizedTextureCoords=true")
        self.assertIn("Tex3DTexel(0u, float3(r0.x, r0.y, r0.z),", body)
        self.assertNotIn("TexSize3D", body)

    def test_fetch_dimension_is_part_of_slot_key(self):
        translator, body = self.translate(
            "tfetch2D r1, r0.xy, tf5",
            "tfetch3D r1, r0.xyz, tf5",
            "tfetch2D r1, r0.yx, tf5",
            "tfetch3D r1, r0.zyx, tf5")
        self.assertEqual(translator.tex_slots, [5, 5])
        self.assertEqual(translator.tex_dimensions, [2, 3])
        self.assertEqual(body.count("Tex2D(0u,"), 2)
        self.assertEqual(body.count("Tex3D(1u,"), 2)

    def test_vertex_slots_and_lod(self):
        translator, body = self.translate("tfetch3D r1, r0.xyz, tf2", stage="vs")
        self.assertIn("Tex3D(16u, float3(r0.x, r0.y, r0.z), "
                      "0u, float3(0.0, 0.0, 0.0), true, 0.0)", body)
        self.assertEqual(translator.tex_dimensions, [3])

    def test_disabled_computed_lod(self):
        _, body = self.translate("tfetch3D r1, r0.xyz, tf2, UseComputedLOD=false")
        self.assertIn("float3(0.0, 0.0, 0.0), true, 0.0)", body)

    def test_default_volume_filters(self):
        for value in ("keep", "use_fetch_const", "default"):
            with self.subTest(value=value):
                _, body = self.translate("tfetch3D r1, r0.xyz, tf2, "
                                         f"VolMagFilter={value}, VolMinFilter={value}")
                self.assertIn("Tex3D(", body)

    def test_refuses_explicit_volume_filters(self):
        for attribute in ("VolMagFilter", "VolMinFilter"):
            for value in ("point", "linear", "basemap", "unknown"):
                with self.subTest(attribute=attribute, value=value):
                    with self.assertRaisesRegex(ucode2hlsl.NotSupported,
                                                f"{attribute}={value}"):
                        self.translate(f"tfetch3D r1, r0.xyz, tf2, {attribute}={value}")

    def test_lod_and_gradient_refusals_remain(self):
        for dimension in (2, 3):
            coords = "r0.xy" if dimension == 2 else "r0.xyz"
            for option in ("UseRegisterLOD=true", "UseRegisterGradients=true", "MipFilter=basemap"):
                with self.subTest(dimension=dimension, option=option):
                    with self.assertRaises(ucode2hlsl.NotSupported):
                        self.translate(f"tfetch{dimension}D r1, {coords}, tf2, {option}")

    def test_cube_fetch_has_distinct_metadata_and_three_coordinates(self):
        translator, body = self.translate("tfetchCube r1, r0.yxw, tf2")
        self.assertEqual(translator.tex_dimensions, [4])
        self.assertIn("TexCube(0u, float3(r0.y, r0.x, r0.w), 0u, "
                      "float3(0.0, 0.0, 0.0), false, 0.0)", body)

    def test_cube_unnormalized_face_offsets_and_vertex_lod(self):
        translator, body = self.translate(
            "tfetchCube r1.zx10, -r0.xzw, tf31, UnnormalizedTextureCoords=true, "
            "OffsetX=-0.5, OffsetY=1.5, OffsetZ=-2.5, LODBias=-1.25, "
            "MagFilter=linear, MinFilter=point, MipFilter=linear, AnisoFilter=max4to1",
            stage="vs")
        self.assertEqual(translator.tex_slots, [31])
        self.assertEqual(translator.tex_dimensions, [4])
        self.assertIn("TexCubeTexel(16u, float3((-r0.x), (-r0.z), (-r0.w)), 1u, "
                      "float3(-0.5, 1.5, -2.5), true, -1.25)", body)
        self.assertNotIn("TexSize", body)
        self.assertIn("r1.x = f1.z;", body)
        self.assertIn("r1.z = 1.0;", body)
        self.assertIn("r1.w = 0.0;", body)

    def test_cube_alu_uses_swizzled_first_source_and_keeps_parallel_reads(self):
        _, body = self.translate("cube_sat r0.xy_w, -r0.zzxy, r0.yxzz",
                                 "+ adds r2.x___, r0.x, r1.x")
        self.assertIn("XenosCube((-r0.zzxy))", body)
        self.assertNotIn("XenosCube(r0)", body)
        self.assertLess(body.index("float s"), body.index("r0.x ="))
        self.assertIn("saturate(v1)", body)
        self.assertNotIn("r0.z =", body)
        self.assertIn("r0.w =", body)

    def test_cube_alu_operand_count_fails_closed(self):
        with self.assertRaisesRegex(ucode2hlsl.NotSupported, "cube operands"):
            self.translate("cube r0, r1.zzxy")

    def test_cube_unsupported_options_fail_closed(self):
        for option in ("UseRegisterLOD=true", "UseRegisterGradients=true", "MipFilter=basemap",
                       "FetchValidOnly=false", "VolMinFilter=linear", "MagFilter=unknown",
                       "AnisoFilter=unknown", "UseComputedLOD=maybe", "LODBias=nan",
                       "OffsetZ=inf", "Unimplemented=true"):
            with self.subTest(option=option):
                with self.assertRaises(ucode2hlsl.NotSupported):
                    self.translate(f"tfetchCube r1, r0.xyz, tf2, {option}")

    def test_cube_explicit_pixel_lod(self):
        _, body = self.translate("tfetchCube r1, r0.xyz, tf2, UseComputedLOD=false, LODBias=0.5")
        self.assertIn("float3(0.0, 0.0, 0.0), true, 0.5)", body)

    def test_dimension_slots_count_toward_stage_limit(self):
        instructions = ["tfetch2D r1, r0.xy, tf0", "tfetch3D r1, r0.xyz, tf0",
                        "tfetch2D r1, r0.xy, tf1", "tfetch3D r1, r0.xyz, tf1"]
        translator, _ = self.translate(*instructions, stage="vs")
        self.assertEqual(translator.tex_dimensions, [2, 3, 2, 3])
        with self.assertRaisesRegex(ucode2hlsl.NotSupported, "more than 4"):
            self.translate(*instructions, "tfetch3D r1, r0.xyz, tf2", stage="vs")

    def test_stage_metadata_and_sidecar_dimensions(self):
        for stage in ("vs", "ps"):
            with self.subTest(stage=stage):
                translate = ucode2hlsl.translate_vs if stage == "vs" else ucode2hlsl.translate_ps
                _, meta = translate("tfetch2D r1, r0.xy, tf3\ntfetch3D r1, r0.xyz, tf3\n"
                                    "tfetchCube r1, r0.xyz, tf3")
                self.assertEqual(meta["tex"], [3, 3, 3])
                self.assertEqual(meta["texdim"], [2, 3, 4])
                with tempfile.TemporaryDirectory() as directory:
                    path = pathlib.Path(directory) / "synthetic.meta"
                    ucode2hlsl.write_sidecar(path, "0000000000000001", meta)
                    self.assertIn("tex=3,3,3\ntexdim=2,3,4\n", path.read_text())

    def test_no_texture_metadata(self):
        _, meta = ucode2hlsl.translate_ps("add oC0, c0, c1")
        self.assertEqual(meta["tex"], [])
        self.assertEqual(meta["texdim"], [])


class VertexStreamTests(unittest.TestCase):
    def test_every_vertex_format_carries_its_stream_slot_to_the_load_helper(self):
        for fmt, (helper, count) in ucode2hlsl.Translator.FORMAT_FETCH.items():
            with self.subTest(fmt=fmt):
                translator = ucode2hlsl.Translator("vs", [], 0)
                translator.translate_body()
                for index in range(3):
                    translator.stream_slot(index, 4)
                instruction = ucode2hlsl.parse_instruction(
                    f"vfetch_full r1, r0.x, vf90, DataFormat={fmt}, Stride=8, ExpAdjust=2", False)
                translator.emit_vfetch(instruction)
                body = "\n".join(translator.lines)
                self.assertIn("StreamAddress(3u,", body)
                self.assertRegex(body, rf"{helper}\(.*StreamEndian\(3u\), .*3u\) \* 4\.0")

    def test_three_through_sixteen_streams_are_preserved_in_order(self):
        for count in range(3, 17):
            with self.subTest(count=count):
                source = "\n".join(f"vfetch_full r1, r0.x, vf{95-i}, "
                                   f"DataFormat=FMT_32_FLOAT, Stride={i}" for i in range(count))
                _, meta = ucode2hlsl.translate_vs(source)
                self.assertEqual(meta["streams"], [{"vf": i, "stride": i} for i in range(count)])

    def test_seventeenth_distinct_stream_fails_closed(self):
        translator = ucode2hlsl.Translator("vs", [], 0)
        for index in range(16):
            self.assertEqual(translator.stream_slot(index, 4), index)
        self.assertEqual(translator.stream_slot(0, 4), 0)
        with self.assertRaisesRegex(ucode2hlsl.NotSupported, "more than 16"):
            translator.stream_slot(16, 4)

    def test_sdk_stream_bounds(self):
        translator = ucode2hlsl.Translator("vs", [], 0)
        self.assertEqual(translator.stream_slot(95, 255), 0)
        self.assertEqual(translator.stream_slot(0, 0), 1)
        for fetch, stride in ((-1, 0), (96, 4), (0, -1), (0, 256)):
            with self.subTest(fetch=fetch, stride=stride):
                with self.assertRaisesRegex(ucode2hlsl.NotSupported, "SDK bounds"):
                    translator.stream_slot(fetch, stride)


class AlphaTestVariantTests(unittest.TestCase):
    """What the native pixel shader without AlphaTest (src/gpu/native/native_alpha_test_variant.h)
    relies on in the generated library and the prelude."""

    PIXEL_LISTINGS = {
        "add oC0, c0, c1": ("0", "0"),
        "add oC1, c0, c1": ("0", "0"),
        "add oC0, c0, c1\nadd oC1, c0, c1": ("0", "0"),
        "kills_eq r0.x, c0.x\nadd oC0, c0, c1": ("1", "0"),
        "kill_eq r0, c0, c1\nadd oC0, c0, c1": ("1", "0"),
        "add oC0, c0, c1\nmax oDepth, c2, c2": ("0", "1"),
    }

    @staticmethod
    def sidecar(meta):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "synthetic.meta"
            ucode2hlsl.write_sidecar(path, "0000000000000001", meta)
            return dict(line.split("=", 1) for line in path.read_text().splitlines())

    def test_pixel_shaders_end_with_one_standalone_alpha_test(self):
        for listing in self.PIXEL_LISTINGS:
            with self.subTest(listing=listing):
                hlsl, _ = ucode2hlsl.translate_ps(listing)
                self.assertEqual(hlsl.count("AlphaTest(oC0.w);"), 1)
                self.assertIn("\n  AlphaTest(oC0.w);\n", hlsl)

    def test_pixel_sidecars_state_kills_and_depth_writes(self):
        for listing, (kills, writes_depth) in self.PIXEL_LISTINGS.items():
            with self.subTest(listing=listing):
                hlsl, meta = ucode2hlsl.translate_ps(listing)
                sidecar = self.sidecar(meta)
                self.assertEqual(sidecar["kills"], kills)
                self.assertEqual(sidecar["writes_depth"], writes_depth)
                # AlphaTest's discard lives in the prelude; a body discards only for a guest kill.
                self.assertEqual("discard" in hlsl, kills == "1")
                self.assertEqual("SV_Depth" in hlsl, writes_depth == "1")
                self.assertNotIn("SV_Coverage", hlsl)

    def test_vertex_sidecars_use_a_different_kill_key(self):
        _, meta = ucode2hlsl.translate_vs("add oPos, c0, c1")
        sidecar = self.sidecar(meta)
        self.assertEqual(sidecar["kill"], "0")
        self.assertNotIn("kills", sidecar)
        self.assertNotIn("writes_depth", sidecar)

    def test_prelude_alpha_test_is_the_transcribed_function(self):
        # tools/test_native_alpha_test_variant.cpp checks NativeAlphaTestCanDiscard against a copy of
        # this function; a change here must be carried there. It is the prelude's only discard, and the
        # prelude declares no writable resource.
        with open(ucode2hlsl.PRELUDE, encoding="utf-8") as f:
            prelude = f.read()
        self.assertEqual(prelude.count(
            "void AlphaTest(float a) {\n"
            "  if ((alpha_test.x & 8u) == 0u) return;\n"
            "  float ref_value = asfloat(alpha_test.y);\n"
            "  uint f = alpha_test.x & 7u;\n"
            "  // (\"pass\" is an HLSL keyword.)\n"
            "  bool keep = (f == 1u) ? (a < ref_value) : (f == 2u) ? (a == ref_value) : "
            "(f == 3u) ? (a <= ref_value) :\n"
            "              (f == 4u) ? (a > ref_value) : (f == 5u) ? (a != ref_value) : "
            "(f == 6u) ? (a >= ref_value) :\n"
            "              (f == 7u);\n"
            "  if (!keep) discard;\n"
            "}\n"), 1)
        self.assertEqual(prelude.count("discard"), 1)
        self.assertNotRegex(prelude, r"\b(RW|RasterizerOrdered)[A-Z]")
        self.assertNotIn("earlydepthstencil", prelude)


if __name__ == "__main__":
    unittest.main()
