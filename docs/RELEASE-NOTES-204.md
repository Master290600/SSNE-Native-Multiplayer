# Public test build 204 — version 1.2.7.204

F12 gameplay cheats can now be changed by the multiplayer host. Invulnerability,
infinite ammunition and all weapons use the same authoritative party settings as
the Insert mod menu, so neither menu overwrites the other. Clients see the shared
values and cannot change them. Existing protocol 63 carries these switches.

The host can enable flight from F12. The runtime resolves the actual host actor
from the extended party registry, keeps the jump/fire flight mapping stable over
frame flips, and restores the original collision and flight configuration when
switched off. Jump raises the host; fire lowers it. Flight is suspended inside a
vehicle. Movement speed and jump height use the shared native movement settings;
weapon damage and flight apply to the host actor. Local unlock settings remain
local to the host. Joining and leaving a session update cheat authority explicitly.

The single-player actor lookup also accepts the extended guest-memory range.
The previous F12 language selection and personal settings/save/crosshair files are
retained. See RELEASE-NOTES-203.md for language setup and RELEASE-NOTES-202.md for
the broader experimental multiplayer scope. This is not full-campaign certification.

Validation: 39 Windows component checks passed. Owned PAL native routines were
executed for all18 ammunition types/both banks, actual ownership checks and
flight ascent/descent/neutral/restoration/authority gates. Two private hidden
processes passed F12 pointer control, host/client ammunition on/off firing,
client read-only controls, held Space/fire flight and all-off restoration with
KBM player2. Client world, inventory and player-pose checks reported no differences.
The two-process fixture used declared CLI/automatic-intro setup; it is not a new
LAN browser or original-menu host-creation acceptance run.
