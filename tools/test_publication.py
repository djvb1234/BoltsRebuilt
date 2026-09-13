import unittest
from check_publication import issues

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

if __name__ == '__main__': unittest.main()
