"""Vulkan release staging and link-order regression checks using inert files."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import stage_vulkan as package


class VulkanPackageTests(unittest.TestCase):
    def test_new_stage_from_previous_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime = root / "Vulkan"
            (runtime / "licenses").mkdir(parents=True)
            for name in package.NOTICE_FILES:
                (runtime / "licenses" / name).write_bytes((package.NOTICES / name).read_bytes())
            hashes = {}
            for name in package.FILES:
                (runtime / name).write_bytes(name.encode())
                hashes[name] = package.file_digest(runtime / name)
            (runtime / "ssne.log").write_text("local only")
            kit = root / "build-kit" / "multiplayer"
            prefix = "lib/CMakeFiles/ssne-port.dir/port/src/"
            units = [prefix + x + ".cpp.obj" for x in ("main", "crosshair_renderer", "settings", "settings_menu", "imgui_sdl_gl")]
            files = []
            for name in [*units, "lib/libimgui.a"]:
                path = kit / name
                path.parent.mkdir(exist_ok=True, parents=True)
                path.write_text(name)
                files.append({"path": name, "sha256": package.file_digest(path)})
            descriptor = {"files": files, "link_args": ["file:" + f["path"] for f in files]}
            kit_file = kit / "runtime-kit.json"
            kit_file.write_text(json.dumps(descriptor))
            manifest = root / "package-files.json"
            manifest.write_text(json.dumps({"schema": 1, "files": [{"path": "README.txt", "sha256": "old"}]}))
            with patch.object(package, "FILES", hashes):
                package.register_release(root)
                first = manifest.read_bytes()
                contents = {row["path"] for row in json.loads(first)["files"]}
                self.assertIn("README.txt", contents)
                self.assertIn("Vulkan/libgallium_wgl.dll", contents)
                self.assertIn("Vulkan/licenses/Mesa-MIT.txt", contents)
                self.assertIn("build-kit/multiplayer/" + units[-1], contents)
                self.assertNotIn("Vulkan/ssne.log", contents)
                package.register_release(root)
                self.assertEqual(first, manifest.read_bytes())
                descriptor["link_args"].reverse()
                kit_file.write_text(json.dumps(descriptor))
                with self.assertRaisesRegex(ValueError, "must precede"):
                    package.register_release(root)
                self.assertEqual(first, manifest.read_bytes())
                (runtime / "opengl32.dll").write_bytes(b"wrong")
                with self.assertRaisesRegex(ValueError, "opengl32.dll"):
                    package.register_release(root)
                self.assertEqual(first, manifest.read_bytes())


if __name__ == "__main__":
    unittest.main()
