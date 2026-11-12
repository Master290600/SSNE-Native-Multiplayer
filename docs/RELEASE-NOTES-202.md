# Public test build 202 — version 1.2.7.202

Build 202 adds Session settings to the original multiplayer menus, with mouse,
keyboard/controller navigation and English/German/French labels. The same entry
is available through Connection during a session.

- Set a personal name before hosting or joining (24 native-font characters).
  The selected game profile remains the default when the name is empty.
- Host enemy-count factors ×1, ×2, ×3, ×4 and ×5. These multiply future regular
  enemy factory calls, not spawn frequency. Bosses, scripted story actors and
  resource preloads remain singular. Clones keep native lifetimes and do not
  acquire the original scripted actor's completion reference.
- Host ammo-carry factors ×1 through ×5, or Unlimited. Unlimited removes the
  weapon-specific carry cap up to the native signed limit of 32,767 per type;
  firing still consumes ammunition.
- Host ammo-pickup factors ×1 through ×5. Health, armor, pickup ownership and
  script grants are not multiplied. Grants saturate at the selected carry cap.
- Clients receive the host's rule values and replicated inventories. Shared
  controls and active-session name changes are read-only on clients. A host
  can change rules during a session; they affect future spawns/pickups and
  current carry caps. Leaving restores the offline rules.
- Personal menu preferences are stored beside settings.ini in
  settings.ini.multiplayer and are excluded from source/release packages.

Protocol 63 intentionally rejects older peers whose validators do not support
these rules. Both players need build 202. Prior custom-crosshair, vehicle-input,
original-menu, AFK and campaign-sync changes remain included.

Focused checks cover the native enemy factory at all factors, boss/preload/
replica exclusions, caller/stack preservation, both ammo banks, normal collector
scoping, signed saturation, template restoration, network validation, names and
localized strings. Private live tests use owned PAL data and isolated cards.
They do not establish a complete campaign playthrough or cross-PC/internet
compatibility. Multiplayer remains experimental.

Download the Windows x64 launcher ZIP from Releases, extract it to an ASCII
path, select an owned PAL G3BP9G ISO/GCM/RVZ in SSNE-Native.exe, build and Play.
No original disc data, full translated game source, built game executable,
personal preferences or saves are included. See README and CREDITS for setup,
upstream authors and license notices.
