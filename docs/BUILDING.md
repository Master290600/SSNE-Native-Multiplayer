# Building

Requirements: CMake ≥ 3.20, a C++20 compiler, Python 3 (tests only), Git (CMake fetches SDL2 2.30.9 and Dear ImGui 1.91.5).

## Windows — Visual Studio 2022
```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
Binaries land in `build\bin\`.

## Windows binaries from Linux (llvm-mingw)
```sh
cmake -B build-win -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DLLVM_MINGW=/opt/llvm-mingw \
      -DSSNE_BUILD_TESTS=OFF
ninja -C build-win
```
The output is statically linked and needs only standard Windows DLLs (msvcrt, OpenGL32). Releases use the
**MSVCRT** llvm-mingw variant (`llvm-mingw-20241119-msvcrt-*`), the same one `data/kit/kit.ini` downloads on the
player's PC, so everything runs on Windows 7 SP1 / 8.1 / 10 / 11 without the UCRT update; check a build with
`llvm-readobj --coff-imports` (no `api-ms-win-crt-*` DLLs).

### Windows release (launcher + build kit)
```sh
tools/make_public_release.sh <out_dir> /opt/llvm-mingw
```

## Linux
Everything builds natively (launcher, port, tools, tests). SDL2 is fetched and linked statically (2.32.10 on Linux,
since 2.30.9 no longer builds against current PipeWire headers); it loads X11/Wayland/PipeWire/PulseAudio at run time.
```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build && (cd build && ctest --output-on-failure)
```
Tools and tests only: add `-DSSNE_BUILD_LAUNCHER=OFF -DSSNE_BUILD_PORT=OFF`.

### Linux release (launcher + build kit)
```sh
tools/make_linux_release.sh <out_dir>
```
Same layout as the Windows release, but `build-kit/kit.ini` (from `data/kit/kit-linux.ini.in`) names the system's own
C++ compiler instead of a download: `compiler = clang++` (or `g++`) is looked up in `PATH`. The script uses clang++
if installed, else g++; set `CXX`/`CC` to choose. The launcher's Install Game then works exactly as on Windows.

### Profile-guided optimization of the runtime (release packages)
Both release scripts build the prebuilt runtime (`libssne-runtime.a`, the emulation side: graphics command and
vertex processing, audio, timing) with profile-guided optimization when the compiler is clang: about 7% less CPU time
on the game thread. The profile is `data/pgo/runtime.proftext` (only the runtime's function names and execution
counts). The release scripts convert it for their compiler with `tools/pgo_build_profile.sh`: clang versions and
standard libraries name and hash some functions differently, so `tools/pgo_target_names.sh` asks the compiler for its
names and `tools/pgo_retarget.py` maps the profile onto them (~98% of the profile's counts match llvm-mingw 19 and
Ubuntu 22.04's clang 14). Without clang, `llvm-profdata` of the compiler's version, or `python3`, the scripts say so
and build without it. The Ubuntu container for Linux packages needs the `llvm` and `python3-minimal` packages.

Re-train after larger runtime changes (functions changed since training just aren't optimized with profile data),
with your recompiled game, its `game_data` and a few save states (gameplay; F5 in the game):
```sh
tools/make_pgo_profile.sh <recompiled dir> <game_data dir> states/G3BE9G/slot1.state ...
```
It builds the headless runner with an instrumented runtime (host clang), plays boot + menus and each state for about
30 s in parallel on the GPU renderer (surfaceless EGL), and rewrites `data/pgo/runtime.proftext`. CMake options for
manual builds: `-DSSNE_PGO_GENERATE=ON` (instrument) and `-DSSNE_PGO_PROFILE=<file.profdata>` (optimize).

## Recompiling the game
1. Install your disc with the launcher, or run `gctool extract game.iso game_data`.
2. Translate the executable:
   ```sh
   ssne-recomp game_data/sys/main.dol recompiled --map data/symbols/G3BE9G_r1.sym
   ```
   Expected output for USA Rev 1: `functions: 7919, jump tables: 23`, `translated 489232 instructions, 23 unimplemented`.
   The 23 "unimplemented" entries are zero words in padding and are never executed.
   Add `--no-comments` for smaller output (~25 MB instead of ~60 MB).
3. Reconfigure with `-DSSNE_RECOMPILED_DIR=<abs path>/recompiled` and build. This produces:
   * `SSNE-Native-Game` (target `ssne-port`): the game window
   * `ssne-headless`: runs the game code without a window (`--data game_data --seconds 10`); `--press A@8,START@30` scripts
     controller input, `--dump-frames DIR` saves frames, `--wav out.wav` records the audio output,
     `--gpu --scale N` renders with the GPU renderer through surfaceless EGL (Linux/Mesa) for testing

Compiling the generated code takes a minute or two (32 translation units). Use `/bigobj` on MSVC; the CMake file already sets it.

## Tests
`tests/make_test_iso.py` builds synthetic discs, including a hand-assembled PowerPC program. CTest then checks that:
the verifier accepts/rejects the right images, extraction works, and the **recompiled** test program computes correct
results (bl/blr, bdnz, rlwinm incl. wrap masks, CR logic, floats, bctrl, switch jump tables, paired singles).

## Integrated Windows native-multiplayer package

Build `ssne-launcher` with CMake as above. Its native patch engine uses
`launcher/src/translation_patch.cpp`; player installs do not invoke Python.
The reviewed runtime descriptor/payload and `launcher-kit.json` bind the sparse
`translation-recipe.json`, PAL symbol map, tools and original disc hashes.
`tools/make_multiplayer_launcher_recipe.py` is a developer-only recipe generator
from private recompiler/reference outputs; keep those generated bodies private.

After producing/reviewing the runtime kit and recipe, assemble a separate clean
launcher package with `tools/package_native_launcher.py --base <existing-clean-kit>
--launcher <build/bin/SSNE-Native.exe> --recipe <translation-recipe.json>
--sidecar <launcher-kit.json> --output <new-folder>`. The package includes source
and runtime-only objects, never extracted data, translated game objects, built
game executables, settings, memory cards or compiler caches. The standard
`make_public_release.sh` builds the original standard package; it does not
assemble the additional reviewed multiplayer kit.

## Public fork release identity

The public Windows launcher uses version 1.2.7.201 and the release feed for
Master290600/SSNE-Native-Multiplayer. A developer fork can set
`-DSSNE_UPDATE_REPOSITORY=owner/repository` at configuration time. The value must
be an owner/repository pair. Public CI builds synthetic/component tests without
a disc; complete local game builds still require the player-owned PAL image.
