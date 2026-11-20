# SSNE-RECOOP-Native

Unofficial Windows native port of **Serious Sam: Next Encounter** with integrated
multiplayer. Based on Silly ReEncounter and SSNE-Native. The launcher builds the
game locally from your own supported GameCube disc image.

## Download and play on Windows

1. Download the Windows x64 launcher ZIP under **Assets** on the [1.2.8.3 release page](https://github.com/Master290600/SSNE-Native-Multiplayer/releases/tag/v1.2.8.3).
2. Extract the complete ZIP into a writable folder with a short ASCII path, such as `C:/Games/SSNE`.
3. Run `SSNE-RECOOP-Native.exe`, select your own European GameCube disc image (G3BP9G, revision 0, English/French/German), and choose **Install Game** or **Build Game**. The package includes the compiler and recompiler.
4. Choose **Play** or open `SSNE-Native-Game.exe` in the installation folder. Choose **Online** in the main menu to host or join. All participants should use version 1.2.8.3 (multiplayer build 252, protocol 71).

**Known issues:** Bots and the Survival game mode may still have bugs.

Build Game also installs the 15 official PS2 multiplayer maps, their previews, six player skins and the Online menu icon from the included content pack. Your European GameCube disc is the only disc you need.

The launcher includes **Map Editor (Alpha 0.13)**. Open **Map Editor** after building the game to edit a level, place enemies or create triggers. Press **F1** in the editor for a guided exercise in your game language. [Map Editor guide](docs/MAP-EDITOR.md)

From version 1.2.8.1 or 1.2.8.2, accept the GitHub update in the launcher. The launcher restarts and rebuilds an installed game automatically.

For a manual update, close the game and launcher, back up your saves and settings, and extract the complete 1.2.8.3 Windows ZIP into your installation folder. Open `SSNE-RECOOP-Native.exe` and choose **Update Game** or **Build Game**. Version 1.2.8.3 uses multiplayer protocol 71.

Open **F12 > Game and menu language** for English, German or French. F12 applies the choice immediately. Restart the game to apply the language to the original PAL menus.

[Build from source](docs/BUILDING.md) · [Credits](docs/CREDITS.md)

## About the native port


Play **Serious Sam: Next Encounter** (Nintendo GameCube, 2004) as a native program on **Linux** and **Windows**:
no emulator. The game's own code is translated to C++ and compiled on your PC from **your own disc image**, then
runs with a PC port that handles graphics (OpenGL), sound, keyboard, mouse and gamepads.

SSNE-Native is a fork of [Silly ReEncounter](https://github.com/takoshero96/Silly-ReEncounter) by takoshero96,
which made all of this possible. It adds a Linux version, fixes and PC-style controls (see
[What SSNE-Native adds](#what-ssne-native-adds)). Its full name is **Serious Sam: Next Encounter Native**; the programs are
`SSNE-RECOOP-Native` (the launcher) and `SSNE-Native-Game` (until 1.2 they kept the original names `SillyReEncounter` and
`SSNE-Native-Game`).

**This repository contains no game code and no game data.** You need your own copy of the game.

## Normal Windows launcher with native multiplayer

The launcher builds the game in the installation folder. Multiplayer settings and the host memory card are kept in `Multiplayer/`. Use **Online** for network play and **Multiplayer** for local split-screen.

Hosts can copy an invitation code from the lobby. Friends can join with the code or find a local host in the LAN server browser. The lobby shows the map, players, ready status and session rules. In a match, press **T** to chat, **Enter** to send and **Esc** to cancel. F12 includes multiplayer key bindings and the team display.

### Custom crosshair PNGs

In the native PAL game, open **F12 > Graphics > Crosshair**. Copy your PNGs into
`crosshairs/` beside the game executable; the integrated multiplayer game uses
`Multiplayer/crosshairs/`. Press **Refresh**, choose a texture and enable
**Use custom PNG**. The preview, size and opacity controls apply locally and
are saved with your settings. **Use original color** is enabled by default: the PNG
receives the original health/target color. Disable it to keep the PNG colors; a
white PNG shows the original tint most directly. Disable the custom PNG checkbox
or choose **Use original crosshair** to return to the original.

Use PNGs up to **1024 pixels per dimension** and **4 MiB per file**, preferably
with a transparent background and at least one visible pixel. The visible alpha
area is centered without changing the source file. A missing, invalid or fully
transparent PNG keeps the original crosshair. Refresh also reloads an edited
file with the same name. Size **32** preserves the original weapon/player-view
size; **14** means 14/32 of that size, rather than 14 fixed screen pixels.

## What you need

- **Your own disc image** of *Serious Sam: Next Encounter* for GameCube, dumped from a disc you own:
  USA (`G3BE9G`, Rev 1) or Europe/PAL (`G3BP9G`; English, French or German).
  Formats: `.iso`, `.gcm`, or Dolphin's `.rvz`, `.wia`, `.gcz`, `.ciso` (no conversion needed).
  Do not ask for, share or download disc images.
- **Linux** (64-bit x86, X11 or Wayland) or **Windows** 7 SP1 / 8.1 / 10 / 11 (64-bit).
- A GPU with **OpenGL 3.3** and an up-to-date driver, and about **3 GB** of free disk space.
- **Linux:** a C++ compiler, which builds the game on your PC. `clang` is recommended (faster), `g++` works too:
  `sudo pacman -S clang` (Arch), `sudo apt install clang` (Debian/Ubuntu), `sudo dnf install clang` (Fedora).
- **Windows:** the 1.2.8.3 launcher package includes the LLVM-MinGW compiler for the local game build.

## How to play

1. **Unpack the launcher package** for your system into a folder you can write to: the game data, saves and settings are
   kept next to it. The package contains the launcher and the build kit, but no game code. To build it yourself,
   see [docs/BUILDING.md](docs/BUILDING.md).
2. **Start the launcher**: `SSNE-RECOOP-Native` (Linux) or `SSNE-RECOOP-Native.exe` (Windows).
3. **Select Disc**: pick your disc image, or drag it onto the launcher window. The launcher checks that it is the
   right game:
   - *Verified*: a known good dump.
   - *Recognized (unverified dump)*: the right game, but a different or modified dump (e.g. a scrubbed `.gcz` /
     `.ciso`). It still installs and plays.
   - *Wrong game / Not a valid disc*: rejected.
4. **Install Game**: copies the game's files out of the disc image into `game_data` (about 1.3 GB), then translates
   the game's program and compiles it on your PC. This takes about a minute with clang, a few minutes otherwise.
   It creates `SSNE-Native-Game`.
5. **Play**. From then on you can also start the game directly from its folder:

   ```bash
   ./SSNE-Native-Game --data game_data
   ```

   (Windows: `SSNE-Native-Game.exe --data game_data`.)

After updating to a newer launcher package, choose **Update Game** in the launcher once: the game is rebuilt with
the new engine (your game data, saves and settings stay).

Everything made from your disc (`game_data`, `SSNE-Native-Game`) is only for you: don't share it.

## Controls

**Keyboard and mouse** (change them in F12 › Keyboard & Mouse, up to three keys or buttons per control):

| Key | Action | Key | Action |
|---|---|---|---|
| W A S D / arrows | move (in vehicles: W / S drive, A / D steer) | Mouse | aim, steer vehicles (click the window to capture the mouse) |
| Left mouse / Ctrl | fire | Space | jump |
| E | use, enter / exit vehicles | R | reload, switch ammo type |
| Right mouse | sniper rifle zoom (hold) | Q | next weapon; **hold** for the weapon wheel |
| Mouse wheel | next / previous weapon | F | previous weapon |
| 1 – 9 | weapons (see below) | Tab | NETRICSA |
| Esc / Enter | pause | | |

- **Weapon wheel:** hold Q, move the mouse towards a weapon, release Q to take it.
- **Number keys:** 1 chainsaw, 2 Desert Hawk, 3 double shotgun, 4 Uzis / minigun, 5 rocket / grenade launcher,
  6 Sirian power gun / flamethrower, 7 sniper rifle, 8 cannon, 9 Serious Bomb. Press a key again for the other
  weapon of its pair; weapons you don't have yet are skipped.
- **Menus, pause screen, cutscenes:** the mouse wheel moves the selection, left click or Space selects (and skips
  cutscenes), right click or Esc goes back. Esc opens and closes the pause menu.
- The game's on-screen button hints show your keys and mouse buttons, or your controller's buttons when you play
  with a gamepad.

**Gamepad** (Xbox layout by default; PlayStation, Switch Pro and most others work): A jump (also selects in menus),
X use, B back, RT fire, LB / RB switch weapons, Y = the GameCube's Z, Start pause, Back = settings menu.
F12 › Gamepad has a "GameCube layout" button, rumble settings and the on-screen button style.

**Hotkeys** (also listed in F12 › Hotkeys):

| Key | Action |
|---|---|
| F12 | settings menu (graphics, audio, controls, save states, exit) |
| F5 / F9 | save state / load state (Shift+F5: next slot) |
| F11 or Alt+Enter | fullscreen |
| F10 | frame rate limit: 60 / 120 / 144 / unlimited |
| F8 | aspect ratio: 16:9 / 21:9 / automatic / 4:3 |
| F6 / F7 | internal resolution down / up |
| F1 | log and statistics window (Shift+F1: info bar at the top) |
| F2 | release the mouse |
| F3, F4 / Shift+F4 | mute, volume down / up |
| Print Screen or Shift+F12 | screenshot (PNG in the `Screenshots` folder) |

## Good to know

- **Where things are kept** (next to the game): `settings.ini`, memory card saves in `saves/`, save states in
  `states/`, screenshots in `Screenshots/`, and compiled shaders in `shadercache/` (safe to delete).
- **First start:** the game's shaders are compiled in the background, shown by a "Preparing shaders" box in the
  bottom-right corner (F12 › Graphics can hide it). The frame rate can dip until it's done; later starts load them
  from `shadercache/`.
- **Frame rate:** use F10 to change the gameplay frame-rate limit. NETRICSA's blue background animation follows elapsed game time.
- **Slow on your PC?** F12 › Graphics › Presets › Low. Automatic resolution (on by default) already lowers the
  internal resolution when the GPU can't keep up.
- **Updates install themselves** (1.2): when it starts, the launcher checks for a newer release on GitHub and offers
  to install it, then restarts and rebuilds the game if the engine changed. `check_updates=0` in `launcher.ini`
  turns the check off.
- **Launcher lettering**: uses Boink LET (the font of the game's menus) if it's installed on your PC.
- **Widescreen:** choose 16:9, 21:9 or Automatic (F8). NETRICSA retains the selected aspect ratio when switching between a mission briefing and a weapon entry. The level score screen also supports widescreen. F12 has additional HUD and side-fill options.
- **Cheats** (single player or multiplayer host, F12 › Cheats; experimental, they can break the game): invulnerable, infinite ammo, all
  weapons with full ammo, faster movement (up to 300%), higher jumps (up to 500%), flying, more weapon damage (up to
  1000%) and unlock everything (the 10 lost levels, the gallery and the game's treasure cheats, without
  collecting the treasure; nothing is written to the memory card). They work at once, also in a running level.
  Hosts control shared invulnerability, ammo, weapons and movement. Flight and weapon damage affect the host;
  clients can view these settings. Jump raises the flying player; fire lowers them.
- **Local multiplayer** (split screen for 2-4 players): plug in a controller per player and choose MultiPlayer;
  every player presses A to join. Co-operative is for 2 players, Deathmatch, Hold the Flag and Pass the Bomb for up
  to 4. F12 › Players sets who plays which player (keyboard & mouse, controllers 1-4) and how two players share the
  screen (side by side or top and bottom). Each view keeps its HUD at its own edges in widescreen, and rumble goes to
  the controller of the player who is hit. Works over the internet with Parsec or Steam Remote Play Together: the
  remote players' controllers show up like your own.
- **Sharpest image:** F12 › Graphics › Internal resolution › Native renders at exactly your screen or window
  resolution (e.g. 1920x1080 in widescreen gameplay on a 1080p screen); Automatic picks a multiple of 640x448.
- **Pause in the background** (on by default): switching to another window pauses and silences the game.
- **Graphics thread and GPU vertex processing:** the game's graphics commands are processed on their own CPU thread,
  in parallel with the game (like the console's GPU), and texture coordinates and lighting are computed on the GPU.
  If something looks wrong that didn't before, set `gx_thread = 0` or `gpu_vertex = 0` in `settings.ini` and restart,
  and please report it.
- **Linux:** Print Screen is usually taken by the desktop; use Shift+F12. If Select Disc doesn't open a file
  dialog, install `zenity` or `kdialog`, or drag the disc image onto the window.
- **Build problems:** the launcher shows the error, and the full logs are in `build-kit/work/logs/`.

## What SSNE-Native adds

On top of Silly ReEncounter 1.01 (details in [data/release/CHANGELOG.txt](data/release/CHANGELOG.txt)):

- **Linux version**: native launcher and game, built with the system's compiler.
- **In-engine cutscenes** show the cutscene instead of a frozen gameplay frame.
- **The jeep** keeps driving at high frame rate limits (vehicles run at 60 fps); Space no longer exits it. Native co-op hosts retain vehicle gameplay input; fire and jump do not become the use/exit action.
- **Menu animations** no longer speed up above 60 fps.
- **Faster** (1.1): graphics processing on its own thread, texture coordinates and lighting on the GPU, a faster
  vertex path and a profile-optimized engine: about two and a half times the frame rate where the CPU is the limit.
- **PC-style controls**: classic shooter keys, WASD driving, mouse in menus, weapon wheel with the mouse, number keys for
  weapons, keyboard & mouse button hints, three bindings per control.
- Slightly lower CPU use on the game thread.

## Building

See [docs/BUILDING.md](docs/BUILDING.md).

## For developers

| Part | Path | What it does |
|---|---|---|
| Launcher (`SSNE-RECOOP-Native`) | `launcher/` | Disc selection and verification, install, builds the game with the build kit, starts it |
| Disc library | `libgc/` | ISO/GCM and Dolphin formats, file system, DOL, SHA-1, verification |
| Recompiler (`build-kit/SSNE-RECOOP-Native`) | `recomp/` | Translates the game's PowerPC `main.dol` to C++ (all 489,232 instructions, 7,919 functions) |
| Runtime | `runtime/` | CPU state, memory, hardware (GX graphics on OpenGL, AX audio, controllers, memory card), threads |
| PC port (`SSNE-Native-Game`) | `port/` | SDL2 window, audio, input, F12 settings menu, F1 statistics |
| Tools | `tools/` | `gctool` (disc images), headless runner, GPU frame replay, release scripts |
| Tests | `tests/` | Synthetic discs and a recompiled-CPU test (no game data needed) |

Technical notes: [docs/FINDINGS.md](docs/FINDINGS.md) (how the game works and how things were found),
[docs/ROADMAP.md](docs/ROADMAP.md), [docs/SYMBOLS.md](docs/SYMBOLS.md).

## Legal

SSNE-Native, like Silly ReEncounter, is an unofficial, non-commercial fan project. It is not affiliated with,
endorsed by or sponsored by Croteam, Devolver Digital, Climax Group, Global Star Software, Take-Two Interactive or
Nintendo. "Serious Sam" and "Serious Sam: Next Encounter" are trademarks of their respective owners and are used
here only to identify the game this software works with. Nintendo GameCube is a trademark of Nintendo.

The project's own code is released into the public domain ([The Unlicense](LICENSE.txt)). Third-party components
keep their licenses (SDL2: zlib, Dear ImGui: MIT, Erica One / Outfit fonts: SIL OFL 1.1).

This project contains no copyrighted game code or assets. Files generated from your disc (`game_data/`,
`recompiled/`, the built game) are derived from the game: **do not commit or distribute them**. `.gitignore`
excludes them.
