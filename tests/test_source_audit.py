import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('audit',Path(__file__).resolve().parents[1]/'tools/audit_source.py')
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)

class SourceAuditTests(unittest.TestCase):
    def test_allows_normal_cpp(self):
        self.assertFalse(audit.inspect('runtime/src/example.cpp',b'void tick() {}'))
    def test_rejects_private_data_even_if_renamed(self):
        self.assertTrue(audit.inspect('private/disc.cpp',b'void tick() {}'))
    def test_rejects_disc_binary(self):
        self.assertTrue(audit.inspect('data/disc.iso',b'payload'))
    def test_rejects_generated_game_body(self):
        self.assertTrue(audit.inspect('runtime/src/accident.cpp',b'void f_80003100(PPCContext& ctx) {\n}'))
    def test_rejects_machine_path(self):
        self.assertTrue(audit.inspect('tools/example.py',b'root="C:/' + b'Users/example/Desktop/game"'))
    def test_rejects_token(self):
        self.assertTrue(audit.inspect('tools/example.py',('token="ghp_'+'x'*36+'"').encode()))
    def test_rejects_unreviewed_binary(self):
        self.assertTrue(audit.inspect('launcher/font.ttf',b'bytes'))
    def test_allows_known_licensed_font(self):
        self.assertFalse(audit.inspect('launcher/fonts/Outfit-Regular.ttf',b'bytes'))

if __name__=='__main__':unittest.main()
