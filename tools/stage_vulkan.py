"""Stage the pinned Windows x64 Zink driver in a portable SSNE installation."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


RELEASE = "https://github.com/pal1000/mesa-dist-win/releases/tag/26.3.0"
ARCHIVE = "mesa3d-26.3.0-release-msvc.7z"
SHA256 = "c505fcc77e8f63a09d7ddac086591d2f542c0698d37fc9e3094685a34d0d9991"
FILES = {
    "opengl32.dll": "cf26e2e580fbcc421668c9e9de985275f240e793f882357fc200cfdcc9813137",
    "libgallium_wgl.dll": "304bc54cb5ad8e99c6cce002d95274b77cd17701908da8363d0b7b27f3d1e21c",
}
NOTICES = Path(__file__).resolve().parents[1] / "licenses" / "vulkan"
NOTICE_FILES = (
    "README.txt", "mesa-dist-win-MIT.txt", "LLVM-LICENSE.txt", "Mesa-notices.rst",
    "Mesa-Apache-2.0.txt", "Mesa-BSL-1.0.txt", "Mesa-GPL-1.0-or-later.txt",
    "Mesa-GPL-2.0-only.txt", "Mesa-Linux-Syscall-Note.txt", "Mesa-MIT.txt", "Mesa-SGI-B-2.0.txt",
)


def file_digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def release_files(install_root):
    """Select only pinned DLLs and matching source notices, even from a live install."""
    expected = {f"Vulkan/{name}": digest for name, digest in FILES.items()}
    for name in NOTICE_FILES:
        expected[f"Vulkan/licenses/{name}"] = file_digest(NOTICES / name)
    selected = {}
    for name, digest in expected.items():
        candidate = install_root / name
        if not candidate.is_file() or candidate.is_symlink() or file_digest(candidate) != digest:
            raise ValueError(f"Missing or changed Vulkan release file: {name}")
        selected[name] = candidate
    return selected


def register_release(install_root):
    """Register the new payload in the staged manifest used to assemble a Windows update."""
    manifest = install_root / "package-files.json"
    value = json.loads(manifest.read_bytes())
    if value.get("schema") != 1:
        raise ValueError("Unsupported Windows package manifest")
    new_files = release_files(install_root)
    # A previous release manifest predates Vulkan. Rebuild the complete Vulkan
    # subset, rather than inheriting unreviewed or obsolete entries from it.
    for name, path in renderer_kit_files(install_root).items():
        new_files[name] = path
    rows = [row for row in value["files"] if not row["path"].casefold().startswith("vulkan/")
            and row["path"] not in new_files]
    rows.extend({"path": name, "bytes": path.stat().st_size, "sha256": file_digest(path)}
                for name, path in new_files.items())
    value["files"] = sorted(rows, key=lambda row: row["path"])
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=install_root, delete=False) as out:
            temporary = Path(out.name)
            out.write((json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode())
        temporary.replace(manifest)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def renderer_kit_files(install_root):
    """Verify that the integrated kit links the objects needed by its F12 renderer."""
    kit = install_root / "build-kit" / "multiplayer"
    descriptor = json.loads((kit / "runtime-kit.json").read_bytes())
    checksums = {row["path"]: row["sha256"] for row in descriptor["files"]}
    link = descriptor["link_args"]
    prefix = "lib/CMakeFiles/ssne-port.dir/port/src/"
    required = [prefix + name + ".cpp.obj" for name in ("main", "crosshair_renderer", "settings", "settings_menu")]
    backend = prefix + "imgui_sdl_gl.cpp.obj"
    library = "lib/libimgui.a"
    required.append(library)
    if backend in checksums:
        if link.index("file:" + backend) >= link.index("file:" + library):
            raise ValueError("The SDL-resolved ImGui backend must precede libimgui.a")
        required.append(backend)
    elif b"imgui_sdl_gl.cpp.obj" not in (kit / library).read_bytes():
        raise ValueError("Rebuilt ImGui Vulkan backend is missing")
    selected = {}
    for name in required:
        path = kit / name
        if "file:" + name not in link or file_digest(path) != checksums.get(name):
            raise ValueError("Invalid renderer kit input: " + name)
        selected["build-kit/multiplayer/" + name] = path
    return selected


def stage(archive, install_root):
    with archive.open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != SHA256:
            raise ValueError(f"Expected the published {ARCHIVE} from {RELEASE}")
    install_root.mkdir(parents=True, exist_ok=True)
    runtime = install_root / "Vulkan"
    # Only publish the optional directory once the archive and notices are ready.
    with tempfile.TemporaryDirectory(dir=install_root) as temp:
        staged = Path(temp) / "Vulkan"
        staged.mkdir()
        subprocess.run([shutil.which("bsdtar") or "tar", "-xf", str(archive.resolve()), "-C", str(staged),
                        "--strip-components", "1", *("x64/" + name for name in FILES)], check=True)
        (staged / "licenses").mkdir()
        for name in NOTICE_FILES:
            shutil.copy2(NOTICES / name, staged / "licenses" / name)
        if runtime.exists():
            # The package is pinned. Allow installation to be retried without
            # replacing DLLs in use or partially combining different versions.
            for path in staged.rglob("*"):
                if path.is_file():
                    existing = runtime / path.relative_to(staged)
                    if not existing.is_file() or not files_match(path, existing):
                        raise ValueError(f"Existing Vulkan runtime differs at {existing}")
        else:
            staged.rename(runtime)
    return runtime


def files_match(first, second):
    with first.open("rb") as a, second.open("rb") as b:
        return hashlib.file_digest(a, "sha256").digest() == hashlib.file_digest(b, "sha256").digest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help=f"{ARCHIVE} from {RELEASE}")
    parser.add_argument("install_root", type=Path)
    parser.add_argument("--release-manifest", action="store_true",
                        help="Register the Vulkan payload in an existing Windows distribution staging manifest")
    args = parser.parse_args()
    print(stage(args.archive, args.install_root))
    if args.release_manifest:
        register_release(args.install_root)
