# Public test build 201 — version 1.2.7.201

## Player setup

Windows x64; European/PAL GameCube Serious Sam: Next Encounter, disc ID G3BP9G
(English/French/German). Extract the launcher ZIP, open SSNE-Native.exe, select
an owned ISO/GCM/RVZ, then Install/Build Game and Play. The compiler is fetched
from the pinned official release. Python and a separate multiplayer CMD are not
required for players. Use an ASCII installation directory.

## Included behavior

- The normal launcher installs/builds and starts the native multiplayer game.
- Original game menus offer hosting, joining and native game-mode selection.
- Mouse navigation, localized multiplayer entries and original navigation sounds.
- Host-authoritative cooperative world replication and configurable bots/AFK control.
- Optional PNG crosshairs under F12: preview, size, opacity, original health/target
  tint, PNG-only colors and original-reticle fallback.
- Vehicle input fix: a co-op host remains in gameplay input mode while driving,
  including periods without on-foot look updates. Fire and Space no longer turn
  into the vehicle's use/exit action. E remains the default intentional exit.

## Validation boundary

Focused private PAL tests reproduced the previous host Jeep defect and checked
real keyboard/mouse conversion on host and client after the fix. Both fired
original projectiles, stayed seated on Space and exited through E. Component
checks cover the vehicle state cache, controller mapping, native GUI, crosshair
lifetimes and launcher package/update validation. The integrated launcher was
also exercised through owned-PAL translation, compilation and actual game startup.

These checks use isolated data/card copies and hidden windows. They are not a
complete campaign playthrough, a cross-PC/internet network certification or a
hardware-controller/display certification. Multiplayer remains experimental.
Both peers need runtime build 201 / protocol 62. Older runtime builds are not an
accepted replacement merely because they share the same protocol number.

## Distribution

The downloadable ZIP contains the launcher, recompiler/disc tool, runtime kit,
sparse patch recipe, source and license notices. Disc images, extracted game_data,
full generated game translations, built game executables, personal settings and
memory cards are created locally. Keep those files out of reports and uploads.
