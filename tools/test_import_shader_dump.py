"""Synthetic merge safety checks; contains no game-derived data."""

from pathlib import Path
import tempfile
import unittest

from import_shader_dump import merge_dump


class MergeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.destination = self.root / "merged"
        self.source.mkdir()

    def test_merge_deduplicates_and_excludes_host_blobs(self):
        listing = "shader_0123456789ABCDEF.ucode.frag"
        binary = "shader_0123456789ABCDEF.ucode.bin.frag"
        (self.source / listing).write_bytes(b"synthetic listing\n")
        (self.source / binary).write_bytes(b"\x00\x01\x02\x03")
        (self.source / "shader_0123456789ABCDEF_001.d3d12.bin.frag").write_bytes(b"host")
        first = merge_dump(self.source, self.destination)
        self.assertEqual(first["added_listings"], 1)
        self.assertEqual(first["added_binaries"], 1)
        self.assertEqual(first["ignored_files"], 1)
        self.assertEqual(sorted(p.name for p in self.destination.iterdir()), [binary, listing])
        second = merge_dump(self.source, self.destination)
        self.assertEqual(second["identical_files"], 2)
        self.assertEqual(second["added_listings"], 0)

    def test_conflict_preflight_prevents_partial_import(self):
        self.destination.mkdir()
        new = "shader_0000000000000001.ucode.vert"
        conflict = "shader_FFFFFFFFFFFFFFFF.ucode.frag"
        (self.source / new).write_bytes(b"new")
        (self.source / conflict).write_bytes(b"different")
        (self.destination / conflict).write_bytes(b"retained")
        with self.assertRaisesRegex(ValueError, "collision"):
            merge_dump(self.source, self.destination)
        self.assertFalse((self.destination / new).exists())
        self.assertEqual((self.destination / conflict).read_bytes(), b"retained")

    def test_dry_run_and_stage_identity(self):
        for suffix in ("vert", "frag"):
            (self.source / f"shader_abcdef0123456789.ucode.{suffix}").write_bytes(b"listing")
        report = merge_dump(self.source, self.destination, dry_run=True)
        self.assertEqual(report["new_stages"], ["ps_ABCDEF0123456789", "vs_ABCDEF0123456789"])
        self.assertFalse(self.destination.exists())

    def test_empty_source_does_not_create_destination(self):
        with self.assertRaisesRegex(ValueError, "No SDK"):
            merge_dump(self.source, self.destination)
        self.assertFalse(self.destination.exists())


if __name__ == "__main__":
    unittest.main()
