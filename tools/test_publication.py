import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib

import check_publication
from check_publication import issues, png_issues, snapshot_issues, SCREENSHOT_MANIFEST


def chunk(kind, data=b''):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)


def png(width=1, height=1, before_data=b'', compressed=None):
    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + before_data
            + chunk(b'IDAT', zlib.compress(b'\x00\xff\x00\x00') if compressed is None else compressed)
            + chunk(b'IEND'))


def screenshot_snapshot(data=None):
    if data is None:
        data = png()
    manifest = {'images': [{'file': 'town.png', 'sha256': hashlib.sha256(data).hexdigest(),
                            'caption': 'Reviewed gameplay capture'}]}
    return {SCREENSHOT_MANIFEST: json.dumps(manifest).encode(), 'docs/screenshots/town.png': data}

class PublicationTests(unittest.TestCase):
    def test_game_data_disguised_as_source_is_blocked(self):
        self.assertTrue(issues('src/test.cpp', b'hello\x00game'))
    def test_ignored_game_path_still_blocked(self):
        self.assertTrue(issues('.local/default.xex', b'XEX2'))
    def test_generated_cpp_is_blocked(self):
        self.assertTrue(issues('generated/default/nb_recomp.1.cpp',b'int x;'))
    def test_private_key_is_blocked(self):
        self.assertTrue(issues('config/key.txt',b'-----BEGIN ' + b'PRIVATE KEY-----'))
    def test_sdk_boilerplate_is_allowed(self):
        self.assertFalse(issues('generated/rexglue.cmake',b'# build rules'))
    def test_original_hlsl_is_allowed(self):
        self.assertFalse(issues('src/gpu/native/shaders/prelude.hlsl',b'float x;'))
    def test_transcribed_shader_cannot_return(self):
        self.assertTrue(issues('src/gpu/nb_command_processor.cpp',b'const char '+b'kDofFogPsHlsl[];'))
    def test_imported_table_cannot_return(self):
        self.assertTrue(issues('config/functions.toml',b'[functions]'))


class ScreenshotTests(unittest.TestCase):
    def test_reviewed_png_is_allowed(self):
        self.assertFalse(snapshot_issues(screenshot_snapshot()))

    def test_standard_color_chunks_are_allowed(self):
        data = png(before_data=chunk(b'sRGB', b'\x00') + chunk(b'gAMA', struct.pack('>I', 45455)))
        self.assertFalse(snapshot_issues(screenshot_snapshot(data)))

    def test_reviewed_png_can_exceed_normal_text_limit(self):
        width, height = 2048, 1024
        pixels = (b'\x00' + b'\xff\x00\x00' * width) * height
        data = png(width, height, compressed=zlib.compress(pixels, level=0))
        self.assertGreater(len(data), 4 * 1024 * 1024)
        self.assertFalse(snapshot_issues(screenshot_snapshot(data)))

    def test_png_without_manifest_is_blocked(self):
        self.assertTrue(snapshot_issues({'docs/screenshots/town.png': png()}))

    def test_unlisted_png_is_blocked(self):
        snapshot = screenshot_snapshot()
        snapshot['docs/screenshots/unreviewed.png'] = png()
        self.assertTrue(snapshot_issues(snapshot))

    def test_changed_png_is_blocked(self):
        snapshot = screenshot_snapshot()
        snapshot['docs/screenshots/town.png'] = png(width=2)
        self.assertTrue(any('SHA-256' in error for error in snapshot_issues(snapshot)))

    def test_missing_listed_png_is_blocked(self):
        snapshot = screenshot_snapshot()
        del snapshot['docs/screenshots/town.png']
        self.assertTrue(any('absent' in error for error in snapshot_issues(snapshot)))

    def test_invalid_manifest_shapes_are_blocked(self):
        for value in (None, [], {}, {'images': {}}, {'images': ['town.png']}):
            with self.subTest(value=value):
                self.assertTrue(snapshot_issues({SCREENSHOT_MANIFEST: json.dumps(value).encode()}))

    def test_invalid_manifest_json_is_blocked(self):
        self.assertTrue(snapshot_issues({SCREENSHOT_MANIFEST: b'{'}))

    def test_manifest_cannot_authorize_other_paths_or_formats(self):
        for filename in ('../town.png', 'nested/town.png', 'nested\\town.png', '/town.png',
                         'town.exe', '.hidden.png', 'town.PNG'):
            with self.subTest(filename=filename):
                manifest = {'images': [{'file': filename, 'sha256': 'a' * 64}]}
                self.assertTrue(snapshot_issues({SCREENSHOT_MANIFEST: json.dumps(manifest).encode()}))

    def test_manifest_requires_lowercase_sha256(self):
        for digest in ('A' * 64, 'a' * 63, 'g' * 64, None):
            with self.subTest(digest=digest):
                manifest = {'images': [{'file': 'town.png', 'sha256': digest}]}
                self.assertTrue(snapshot_issues({SCREENSHOT_MANIFEST: json.dumps(manifest).encode()}))

    def test_duplicate_entries_are_blocked(self):
        snapshot = screenshot_snapshot()
        manifest = json.loads(snapshot[SCREENSHOT_MANIFEST])
        manifest['images'] *= 2
        snapshot[SCREENSHOT_MANIFEST] = json.dumps(manifest).encode()
        self.assertTrue(any('duplicate' in error for error in snapshot_issues(snapshot)))

    def test_other_binary_files_remain_blocked(self):
        snapshot = screenshot_snapshot()
        snapshot['docs/screenshots/raw.bin'] = b'\x00raw data'
        self.assertTrue(snapshot_issues(snapshot))

    def test_screenshot_still_receives_credential_check(self):
        data = png(compressed=b'-----BEGIN ' + b'PRIVATE KEY-----')
        self.assertTrue(any('credential-like' in error for error in snapshot_issues(screenshot_snapshot(data))))

    def test_invalid_signature_is_blocked_even_with_matching_hash(self):
        self.assertTrue(snapshot_issues(screenshot_snapshot(b'not a PNG')))

    def test_bad_crc_and_lengths_are_blocked(self):
        data = png()
        bad_crc = bytearray(data)
        bad_crc[29] ^= 1
        bad_length = data[:8] + struct.pack('>I', len(data)) + data[12:]
        for invalid in (bytes(bad_crc), bad_length, data[:-1], data[:-12]):
            with self.subTest(invalid=invalid):
                self.assertTrue(png_issues(invalid))

    def test_trailing_payload_is_blocked(self):
        self.assertTrue(png_issues(png() + b'private payload'))

    def test_embedded_metadata_is_blocked(self):
        for kind in (b'tEXt', b'zTXt', b'iTXt', b'eXIf', b'zzZZ'):
            with self.subTest(kind=kind):
                self.assertTrue(png_issues(png(before_data=chunk(kind, b'private metadata'))))

    def test_excessive_size_or_dimensions_are_blocked(self):
        self.assertTrue(png_issues(png() + b'\x00' * (24 * 1024 * 1024)))
        for width, height in ((0, 1), (16385, 1), (16384, 16384)):
            with self.subTest(width=width, height=height):
                self.assertTrue(png_issues(png(width, height)))

    def test_missing_image_data_is_blocked(self):
        self.assertTrue(png_issues(png(compressed=b'')))


class GitSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.git('init', '-q')
        self.write_snapshot(screenshot_snapshot())

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], stderr=subprocess.STDOUT)

    def write_snapshot(self, snapshot):
        for path, data in snapshot.items():
            destination = self.root / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)

    def check(self, head=False):
        output = io.StringIO()
        with patch.object(check_publication, 'ROOT', self.root), \
                patch('sys.argv', ['check_publication.py'] + (['--head'] if head else [])), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            return check_publication.main(), output.getvalue()

    def test_working_tree_manifest_cannot_approve_staged_image(self):
        valid_manifest = (self.root / SCREENSHOT_MANIFEST).read_bytes()
        self.write_snapshot({SCREENSHOT_MANIFEST: b'{"images": []}'})
        self.git('add', '.')
        self.write_snapshot({SCREENSHOT_MANIFEST: valid_manifest})
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn('not listed', output)

    def test_head_reads_committed_image_and_manifest(self):
        self.git('add', '.')
        self.git('-c', 'user.name=Publication test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'fixture')
        self.write_snapshot({'docs/screenshots/town.png': png(width=2)})
        self.git('add', '.')
        self.assertEqual(self.check(head=True)[0], 0)
        self.assertEqual(self.check()[0], 1)


if __name__ == '__main__': unittest.main()
