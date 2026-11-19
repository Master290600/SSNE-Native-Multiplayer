"""CPU-only packaging regression tests; fixture tools are inert text, never executed."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT=Path(__file__).resolve().parents[1]
REFERENCE=Path(os.environ.get('SSNE_DISTRIBUTION_TEST_SOURCE',ROOT))
sys.path.insert(0,str(ROOT/'tools'))
from audit_source import vendor_source

def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/f'{name}.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
package=load('package_native_launcher')
checker=load('check_release_package')

def digest(data):return hashlib.sha256(data).hexdigest()

class ReleasePackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='ssne-package-test-')
        self.root=Path(self.tmp.name);self.base=self.root/'base';self.source=self.root/'source'
        self.output=self.root/'package';self.git_files=[]
    def tearDown(self):self.tmp.cleanup()
    def put(self,root,name,data):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True)
        p.write_bytes(data.encode() if isinstance(data,str) else data);return p
    def fixture(self,protocol=70):
        for name in ('gctool.exe','known_dumps.txt','LICENSE.txt','THIRD-PARTY-NOTICES.txt',
                     'build-kit/SSNE-RECOOP-Native.exe','build-kit/kit.ini','build-kit/G3BE9G_r1.sym','build-kit/G3BP9G_r0.sym',
                     'build-kit/include/base.h','build-kit/lib/base.a','licenses/existing-LICENSE.txt'):
            self.put(self.base,name,'INERT FIXTURE '+name)
        self.launcher=self.put(self.root,'launcher-placeholder.txt','INERT LAUNCHER FIXTURE')
        for name in ('.gitignore','tools/audit_source.py','tools/package_native_launcher.py','tools/check_release_package.py',
                     'data/release/THIRD-PARTY-NOTICES.txt','data/release/NATIVE-MULTIPLAYER.txt',
                     'data/release/NATIVE-INVITATIONS.txt','data/release/SSNE-INTERNET.example',
                     'licenses/libjuice-MPL-2.0.txt','licenses/KCP-MIT.txt','docs/INVITATION-DEPENDENCIES.json'):
            origin=ROOT/name
            if not origin.is_file():origin=REFERENCE/name
            self.put(self.source,name,origin.read_bytes());self.git_files.append(name)
        self.put(self.source,'tools/patch_multiplayer.py','pass # inert fixture patcher')
        self.git_files.append('tools/patch_multiplayer.py')
        # Vendor inputs deliberately do not appear in mocked Git enumeration.
        # The packager must collect all reviewed dependencies independently.
        for directory in ('vendor/libjuice','vendor/kcp'):
            for path in (REFERENCE/directory).rglob('*'):
                name=path.relative_to(REFERENCE).as_posix()
                if path.is_file() and vendor_source(name):self.put(self.source,name,path.read_bytes())
        for name in ('vendor/kcp/benchmark.png','vendor/libjuice/fuzzer/input/message1.raw'):
            self.put(self.source,name,b'EXCLUDED UPSTREAM FIXTURE');self.git_files.append(name)
        files=[]
        for i in range(58):
            name=f'include/reviewed{i}.h' if i<57 else 'lib/ssne-coop.a'
            payload=('INERT RUNTIME FIXTURE '+str(i)).encode()
            self.put(self.base,'build-kit/multiplayer/'+name,payload)
            files.append({'path':name,'sha256':digest(payload)})
        descriptor={'schema':1,'build':229,'protocol':protocol,'files':files,
            'recompiler_sha256':digest((self.base/'build-kit/SSNE-RECOOP-Native.exe').read_bytes()),
            'patcher_sha256':digest((self.source/'tools/patch_multiplayer.py').read_bytes())}
        descriptor_data=(json.dumps(descriptor)+'\n').encode()
        self.put(self.base,'build-kit/multiplayer/runtime-kit.json',descriptor_data)
        self.recipe=self.put(self.root,'recipe.json',json.dumps({'format':67,'runtime_kit_sha256':digest(descriptor_data)}))
        self.side=self.put(self.root,'sidecar.json',json.dumps({'runtime_kit_sha256':digest(descriptor_data),
            'patch_recipe':'build-kit/multiplayer/translation-recipe.json','patch_recipe_sha256':digest(self.recipe.read_bytes()),
            'symbols':'build-kit/G3BP9G_r0.sym','symbols_sha256':digest((self.base/'build-kit/G3BP9G_r0.sym').read_bytes())}))
    def assemble(self):
        argv=['package_native_launcher.py','--base',str(self.base),'--launcher',str(self.launcher),
              '--recipe',str(self.recipe),'--sidecar',str(self.side),'--output',str(self.output)]
        enumeration=('\0'.join(self.git_files)+'\0').encode()
        with mock.patch.object(package,'__file__',str(self.source/'tools/package_native_launcher.py')),mock.patch.object(sys,'argv',argv),mock.patch.object(package.subprocess,'check_output',return_value=enumeration),contextlib.redirect_stdout(io.StringIO()):
            package.main()
    def archive(self,mutate=None):
        contents={p.relative_to(self.output).as_posix():p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        if mutate:mutate(contents)
        # Recompute hashes to exercise policy checks beyond checksum failures.
        contents['package-files.json']=json.dumps({'schema':1,'files':[{'path':n,'sha256':digest(d)} for n,d in sorted(contents.items()) if n!='package-files.json']}).encode()
        path=self.root/'fixture.zip'
        with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED) as z:
            for name,data in contents.items():z.writestr(name,data)
        return path
    def test_current_protocol_70_recipe_67_and_licenses(self):
        self.fixture();self.assemble();report=checker.audit(self.archive())
        self.assertTrue(report['passed']);self.assertEqual(report['protocol'],70)
        marker=json.loads((self.output/'launcher-integration.json').read_bytes())
        self.assertEqual(marker['integration_build'],229)
        self.assertEqual(json.loads((self.output/'build-kit/multiplayer/translation-recipe.json').read_bytes())['format'],67)
        self.assertTrue((self.output/'licenses/existing-LICENSE.txt').is_file())
        self.assertEqual((self.output/'THIRD-PARTY-NOTICES.txt').read_bytes(),(self.source/'data/release/THIRD-PARTY-NOTICES.txt').read_bytes())
        self.assertTrue((self.output/'source/vendor/libjuice/src/juice.c').is_file())
        self.assertTrue((self.output/'source/vendor/kcp/ikcp.c').is_file())
        self.assertFalse((self.output/'source/vendor/kcp/benchmark.png').exists())
        self.assertFalse((self.output/'source/vendor/libjuice/fuzzer/input/message1.raw').exists())
        self.assertFalse((self.output/'game_data').exists())
        self.assertFalse((self.output/'Multiplayer').exists())
    def test_campaign_vehicle_protocol(self):
        self.fixture(protocol=71);self.assemble()
        self.assertEqual(checker.audit(self.archive())["protocol"],71)
    def test_checker_requires_source_license_match(self):
        self.fixture();self.assemble()
        for name in ('licenses/libjuice-MPL-2.0.txt','source/vendor/libjuice/LICENSE','licenses/KCP-MIT.txt','source/vendor/kcp/ikcp.c','NATIVE-INVITATIONS.txt','SSNE-INTERNET.example'):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):checker.audit(self.archive(lambda c:c.pop(name)))
    def test_checker_rejects_stale_notices(self):
        self.fixture();self.assemble()
        with self.assertRaises(ValueError):checker.audit(self.archive(lambda c:c.__setitem__('THIRD-PARTY-NOTICES.txt',b'outdated')))
    def test_checker_rejects_injected_vendor_binary(self):
        self.fixture();self.assemble()
        with self.assertRaises(ValueError):checker.audit(self.archive(lambda c:c.__setitem__('source/vendor/kcp/benchmark.png',b'bytes')))
    def test_builder_rejects_game_artifact_in_source(self):
        self.fixture();name='port/src/accidental-game.exe';self.put(self.source,name,b'INERT FORBIDDEN FIXTURE');self.git_files.append(name)
        with self.assertRaises(ValueError):self.assemble()
    def test_rejects_unknown_protocol(self):
        self.fixture(protocol=72)
        with self.assertRaises(ValueError):self.assemble()

if __name__=='__main__':unittest.main()
