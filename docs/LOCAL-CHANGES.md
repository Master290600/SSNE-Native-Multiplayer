# Local changes collected after build204

Local build226/protocol69 adds automatic host-to-client Custom Level packages,
including display name and preview. Bounded TCP chunks share the existing
session connection with world/control traffic. Complete packages are validated
before an append-only virtual-disc mount; previous DVD entries retain their
indices and extents. File I/O and package hashing run outside the guest frame
and authority locks. The original Connecting GUI provides localized progress
and cancellation. Downloaded versions use a separate immutable cache and do
not replace authored CustomLevels. This change remains local and unpublished.

This working tree contains an unpublished local multiplayer chat and CPU optimizations (build208,
protocol65). No GitHub release or distribution ZIP was created for this change.

T opens the chat at the upper left of the actual game image. Enter sends and
Escape cancels. Typing suppresses gameplay input and gameplay hotkeys, including
held jump/fire/movement actions. Closing chat restores the earlier mouse capture
when the game is still controllable. Existing names identify authenticated senders.

Messages are delivered over the session TCP connection, retained over level
changes and cleared when the session ends. The chat has no background or permanent idle hint. Recent messages disappear
after5seconds, fading during the final second;
opening chat shows the session history (up to64messages). The editor and hints
follow the selected English, German or French menu language.

Host and clients need the same local build208/protocol65. Older public build204
still uses protocol63. The launcher continues to accept reviewed kit protocols63,64 and65
so existing builds can be read, but a game session requires matching
wire versions. Settings, save cards and custom crosshair PNGs are preserved.


## Local renderer and presentation performance work

The OpenGL renderer now calculates the used texture-unit mask once per program
and resolves/binds only the texture units referenced by that program. Its TEV
shader sources, geometry and cache format are unchanged. The internal reference
path is available through `SSNE_GPU_TEXTURE_BINDINGS=legacy`.

Client render sampling reuses the already owned pose-vector storage instead of
allocating and copying a second bone-matrix vector. Float operation order and
model/layout/lifetime checks are retained. `SSNE_PRESENTATION_VECTOR_REUSE=0`
selects the old path for a comparison. Bit-value and complete-frame tests cover
both paths, immutable source snapshots, aliases, layout changes and retirements.

Private same-context ABBA replay of three immutable combat captures measured
900 pairs at each of 1x and 3x, with the client local-camera flag restored (the
capture format does not store it). Median CPU submission was 1.458 -> 1.417 ms
and 1.451 -> 1.400 ms respectively. The median paired deltas were -0.044 and
-0.050 ms. Every compared RGBA pixel and PNG byte matched. These are render
submission timings, not full-game FPS; no GPU-execution or worst-frame gain is
claimed. Separate shader/effect/menu replays also produced identical pixels.

The isolated pose-copy benchmark reduced allocations from two to one per pose
with identical output checksums. Its small synthetic loop is not an FPS result.

`tools/profile_enemy_wave.py` provides the local two-process combat fixture.
It uses private copies of the card, settings and shader cache, ordinary input to
close introductory screens, a 240-FPS cap, native enemy factories and verified
living population. First-wave distance and row spacing are explicit; this is
a stress fixture rather than a campaign completion test. Both peers must show
visible gameplay before measurement. Capture I/O runs are excluded from FPS
comparisons. `SSNE_PERF_RAW_FRAMES=1` enables bounded per-operation durations
for actual new-image presentation, updates and renderer submission when logging
is enabled. Ordinary original-menu sessions leave performance logging off when
`SSNE_PERF_LOG` is absent. GPU submission also includes empty polling
calls, so its raw median must not be mistaken for actual frame rendering cost.

These changes remain local build206/protocol65. No ZIP, commit or publication
is created by this work. Personal settings, cards and crosshair assets remain
outside the optimization.

The earlier instrumented renderer-only portable gameplay check used four runs with both old paths enabled
or both optimized defaults, in forward and reverse order. Each run kept 84
living enemies at 84 different X/Z positions in eight rows 7..17.5 m ahead.
Host median FPS ranged 43.98..44.89 and client 43.95..44.93 at a 240-FPS cap.
The small overall differences reversed between pairs; no reliable total-FPS
or worst-frame improvement is claimed. All peers exited cleanly, the selected
shader count stayed constant during timing, and original files stayed equal.
The source CLI subsequently gained a 0..5 m row-spacing argument guard; the
exact driver used for these runs is preserved with the private test evidence.

## Local multiplayer CPU and synchronization work

World capture and transmission run independently of render FPS. Native gameplay,
input, scripts and bosses continue at their existing cadence. Input acknowledgements
are published with their corresponding captured poses; repeated network-worker
iterations reuse the immutable snapshot until its revision changes.

Only players with a current connection identity are captured for publication.
Unready connected players remain included so late join can bind their characters.
Unconnected reservations still exist in native RAM, full captures and checkpoints.
Disconnect detaches the client logical binding without destroying the reserved actor,
HUD or models; the same slot can reconnect with a new generation. Lifecycle scalar
reconciliation borrows unchanged immutable entity fields instead of deep copying the
whole world and its skeletons, while retaining fresh resource and value validation.

Client presentation reuses interpolation plans, vector storage and contiguous write
plans. The indirect guest-call dispatcher caches stable hook slots per thread and
still reads the current installed hook at each call. Reference switches are private
process options rather than saved settings or host rules.

The verified PAL authoritative render transaction captures draw classification at
FIFO command boundaries. Five extra view-context waits can therefore be omitted;
original SDK completion points and all guest-memory, root, layout and pose-lease
restoration fences remain. Single-player and other unmarked rendering keep live
classification. Save states with only live/unknown contexts retain GX01; required
known pending contexts use GX02. The new runtime reads both. For diagnostic
SSNE_GX_CONTEXT_VERIFY=1, async view scopes default off; explicitly combining
verification with async scopes is rejected. SSNE_GX_SCOPED_CONTEXT=0 and
SSNE_GX_ASYNC_VIEW_SCOPES=0 retain the reference paths. A GX02 state containing
known contexts requires scoped decoding when loading.

The session protocol is65 because older clients cannot detach reserved player
bindings safely. Host and clients must use the matching updated game. The normal
launcher and packaging tools accept reviewed kit protocols63,64 and65. Ordinary
memory cards, preferences and custom crosshairs are preserved. Native cooperative
checkpoint files retain their existing strict executable/data/configuration identity
checks; they are not portable between different game executable hashes.

Local validation includes the complete automated suite and original-menu host/client
late join, disconnect, same-process same-slot reconnect with a new generation,
results continuation, level/epoch change and return to single-player. An owned-PAL
RAM fixture additionally checks publication parity, unready inclusion, native
reservation survival and atomic rejection/restore for invalid data or active render
leases. These checks establish correctness; FPS is measured separately without
extra comparison captures. No equal-to-single-player or fixed240FPS guarantee is
implied, and these changes have not been published.

## Further local worker, render-lease and host-publication changes

The network worker now uses a WorldReadView of the immutable shared world body.
Each peer owns its decorated mission/event overlay while entity and bone arrays
remain owned by the shared body. Baseline, in-flight and retained sent snapshots
keep that ownership through acknowledgement, force-full retransmission and epoch
changes. Validation and encoding still examine the actual snapshot; mutable guest
RAM or entity pointers are not cached. SSNE_NET_WORLD_VIEW=0 retains the deep-copy
reference path for private comparisons.

Client render leases use contiguous bone spans when all bone ranges are disjoint
from other bones and scalar/mesh-stamp writes. Fresh native bindings, counts,
resource layouts and every float are checked for each lease. Owned raw backups
supply both exact restoration bytes and the original pose vectors; all packing
and backup allocations finish before guest writes. Any overlap falls back for the
whole lease to the original ordered word plan. SSNE_RENDER_BONE_SPANS=0 selects
that word path. The real 84-enemy fixture tested 84 fresh skeletons and 23,184 bone
words in four reference/span and plan-order combinations: complete RAM during
rendering and after restoration matched exactly, and the original native bones
hook replayed both visual and restored registry poses. Aliases, late NaN, the
post-write 10001 registry rejection, temporary source maps and epoch/lease guards
also passed. These are correctness and work-count results, not an FPS measurement.

A stable cooperative host without any TCP connection now publishes complete
world snapshots at a nominal 10 Hz. This includes the absence of accepted sockets
that have not finished joining. Native simulation, input, rendering and AFK/bot
decisions retain their existing cadence. Fresh player binding/health checks,
ammunition HUD repair and confirmed own-hit feedback keep their 16 ms budget.
Connected-but-unready participants, native transitions, save/results/cutscene/movie
states and pending join placement bypass this reduction; accepting a TCP socket
wakes the normal 16 ms publication budget before peer readiness. Bone ESP and an
aimbot using a bone/head target also keep the normal budget so native skeleton
reads stay fresh. SSNE_HOST_IDLE_PUBLICATION=0 retains the old publication path.

The primary local measurement now uses an existing fresh-game-image counter
with performance logging and sampled detail profiling disabled. An uncapped,
same-PC ABBA comparison changed only SSNE_HOST_IDLE_PUBLICATION; the other
optimization defaults and the 4-enemy fixture stayed identical. Two 18-second
mean-rate measurements per condition summarized Host-alone performance as
179.2661 -> 193.3250 FPS (+7.84%). Single-player runs before and after the ABBA
sequence summarized 298.000 FPS. Both conditions showed substantial time drift;
two repetitions do not establish a robust confidence interval or single-player
parity. The counter advances for new game images before UI/Swap, giving at most
one in-flight-image uncertainty at the endpoints. Sparse counter reads supply
mean rates only: no P99, 1%-low, scanout or general campaign result is inferred.

The earlier ABBA figures, 147.90 -> 174.96 FPS with 4 enemies (+18.3%) and
53.28 -> 60.86 FPS with 84 enemies (+14.23%), were instrumented diagnostics with
performance logging/detail work active. They are not normal-play improvement
claims and are superseded for the Host-alone comparison above by the Perf-OFF
counter measurement. The 84-enemy fixture used 4 authored enemies plus 80 native
factory-spawned enemies in fixed rows. These measurements do not isolate a
WorldReadView or bone-span gain and do not establish client performance,
single-player equality or a fixed FPS target.

A further local camera candidate omits the original per-frame world-camera
update only for verified disconnected reserved PAL cooperative-host slots.
Connected-but-unready players retain their updates. A fresh session gate and
registry/actor/camera/FOV-getter checks preserve the complete original path during
save/load, level/epoch/join or spawn uncertainty, menus/results/cutscenes/movies,
camera shake, other camera modes, unknown identities or aliased storage. Other
camera groups, native simulation and animation updates retain their behavior.
SSNE_CONNECTED_CAMERA_UPDATES=0 selects the reference path.

The earlier 62-case automated suite passed, including the final restored core.
Owned-PAL raw tests passed 47 guard cases per mode and reduced original camera
callbacks from 8 to 2 with two connected players. Complete RAM matched between
reference and filtered callbacks, including a test that really changed the active
camera matrix; disconnected cameras and actor bytes stayed unchanged. Three
original-menu lifecycle QA runs and 61 screenshot checks also passed. These are
correctness and work-count results, not full-campaign completion evidence.

The same-binary camera A/B summarized Host rates as 172.01 -> 174.24 FPS (+1.30%),
with substantial time drift and mixed forward/reverse directions. This does not
establish a reliable camera FPS improvement. The supported result is fewer
unneeded callbacks while preserving the checked behavior. These changes remain
local and unpublished.

An earlier build205 check of the restored executable used 84 verified living enemies and a
requested 240-FPS cap, with performance logging/detail profiling off. Each
configuration ran once for 18 seconds; these are mean fresh-game-image rates.

| Configuration | Role | Mean FPS |
| --- | --- | ---: |
| Single-player | Local player | 128.4202 |
| Cooperative host alone | Host | 63.6406 |
| Host and client on the same PC | Host | 54.3432 |
| Host and client on the same PC | Client | 50.0134 |

All three configuration checks passed, but none attained the requested cap.
The paired processes shared the same PC's CPU/GPU resources. One run per
configuration is a limited stress snapshot, with no improvement comparison,
robust confidence interval, frame-time quantiles, single-player parity or
full-campaign claim. The separate 4-enemy publication A/B above must not be
extrapolated to this population.


## Local build206: remove reserved-player and diagnostic work

The verified PAL scene-manager auxiliary weapon/aim pass queried all eight
reserved actors even when only one player was connected. In the earlier
84-enemy diagnostic it cost about5.14ms per Host frame versus0.35ms in SP.
The new predicate filter changes only that exact callsite. It preserves the
original hook chain for occupied actors, unrelated callers, other modes and
unknown layouts. Client filtering additionally requires a ready canonical local
actor, matching ready/connected masks, clear UI/transition state and fresh
registry identity. Deferred disconnected spawn reservations remain valid.
Connected or unknown pending spawns use the original path.

Stable, verified cooperative Host frames can defer the complete frame observer
until after the original SDKFlip and an explicit GX drain. The same CPU context
is temporarily restored to its original observer-entry state, then restored to
the SDK-returned state. Fresh ownership/layout gates cancel unsafe tickets;
menus, save/load, transitions, probes and Client pose restoration retain their
required fences. Mouse diagnostics no longer serialize complete menu JSON during
ordinary gameplay. Client full-world comparison captures and retained enemy-bone
copies are explicit diagnostics rather than normal-play work. Reconciliation,
fresh native binding checks, HUD refresh and error handling are unchanged.

Reference switches: SSNE_HOST_RENDER_PIPELINE=0;
SSNE_MOUSE_DIAGNOSTICS_EAGER=1; SSNE_SECONDARY_PLAYER_FILTER=0 (all filtering);
SSNE_SECONDARY_PLAYER_CLIENT_FILTER=0 (Client filtering only);
SSNE_NATIVE_DIAGNOSTICS_EAGER=1 (old Client comparison/copy path). Existing
campaign verification and lifetime tracing still opt into those diagnostics.

The final normal image passed66 automated tests and an original-menu Host/Client
flow covering movement/fire, pause, adding/removing a bot, private F5 save, real
LAN late join, leave, same-process rejoin with a new generation, both Main menus
and real SP after the session. Bounded native traces confirm Host1forward/7skip
and Client2forward/6skip. Required diagnostic readback markers remain absent in
normal play. Twelve final functional screenshots were directly reviewed. This
is first-level continuity coverage, not a campaign/boss/level-transition result.

Owned-PAL constituent tests execute the real getter and complete auxiliary pass:
28 getter checks and17 guards per reference/default process; actual calls7->1
(the second connected actor is driving and already fails the original predicate).
Eleven active actor/camera/HUD/selected-weapon prefixes,8,524 bytes total, match
exactly. Reference full RAM matches; default intentionally stops inactive-state
advancement and differs in150 other bytes. This does not claim full Client RAM,
held-fire cleanup, vehicle-disconnect or complete weapon-allocation parity.

An earlier same-image six-case Host comparison with84 stable living NPCs measured
64.3000->127.2543 mean FPS; both directions improved (+100.35%/+95.54%). SP
bookends averaged133.9669 with2.91% drift. That constituent Host helper remains
byte-for-byte unchanged. Final-image regression checks and Client ABBA use the
existing native fresh-image counter with profiling disabled, scale1/FOV63:

| Final206b case | Role | Mean FPS | Window/repetitions |
| --- | --- | ---: | --- |
| SP,84 living NPCs | Local | 133.8925 | 18s,N1 |
| Host alone,84 living NPCs | Host | 129.6255 | 18s,N1 |
| Pair,4 NPCs,old Client work | Host / Client | 218.6322 /136.1090 | 12s,N2 |
| Pair,4 NPCs,normal defaults | Host / Client | 218.8985 /151.2472 | 12s,N2 |
| Pair,84 NPCs,requested240 cap | Host / Client | 94.7021 /61.0131 | 8s,N1 |

Client mean gain is11.12%; paired directions are+11.65%/+10.60%. Host changes
are mixed and small (-0.53%/+0.78%); no Client-cleanup Host improvement is claimed.
The two Client changes are combined, not individually isolated. All24 final
benchmark screenshots were directly reviewed; each role keeps exactly the same
live enemy IDs/count through the post-measurement screenshots. Distinct native
frame IDs, not350ms repeated polls, count fixture observations.

A prior18s Pair84 run was rejected: all sampled states were84 live, but post-window
screenshots/state changed to Host74/Client66 with visible collapsed bodies. The
attacker/death reason was not recorded; the fixture leaves native AI/damage/
lifetimes running. Its FPS is excluded. The subsequent8s case retains the full
population/ID/post-screenshot guards and passes; it does not establish long-run
wave stability. Pair processes share one PC and have different player views.
240 is a requested limit, not attained in this stress pair. No confidence
interval, P99/1%-low, scanout, global SP parity or full-campaign claim is made.

Everything stays local build206/protocol65. No ZIP, Git commit/stage, upload or
release was created. Normal launcher readiness is checked separately after the
local installation; saves, personal settings and crosshairs are preserved.


## Local Build207: bounded client presentation storage

The client now reuses the capacity of its owned render-lease scratch buffers.
Object bindings, addresses, bone matrices and restoration backups are refreshed
for every lease; no pose or model-layout authority is carried between frames.
Active prefixes and an explicit lease flag keep empty/shrinking frames separate
from retained capacity. Reset releases the buffers. Retention is bounded to
8 MiB and 512 pose slots; larger prepared leases use transient owned storage.
`SSNE_PRESENTATION_SCRATCH_REUSE=0` selects the allocation reference path.

Unused first-person proxy collection and an unused player readback in the client
render preparation were removed. The original render, authoritative simulation,
registration, rollback and GX/RAM completion fences remain in place.

All 66 existing CTests pass. Eight owned-PAL render cases cross scratch reuse,
bone spans and visual-plan normalization, including a full 84-enemy frame.
Complete before/during/after RAM digests agree across all modes. Fresh input and
backup data, aliases, retargeting, layout/epoch changes, empty/shrink/grow frames,
preflight failure, registration rollback, reset and retention fallback are checked.
Four additional owned-PAL cases cross scratch and registry reuse and verify
restoration/ownership. These captures are private and are never distributed.

With the default span/plan paths warmed, the 84-enemy test makes 3 allocations
per three leases instead of 1,068, retaining 395,648 bytes for 84 pose slots.
This is an allocation result, not a full-game FPS or 240-FPS guarantee. Required
renderer completion waits remain a separate cost. The patch stays local and
unpublished, with the existing protocol65 and launcher entry point.

Normal-game PerfOFF pair4 ABBA (12 s/run, two runs/setting, same frozen207
image and warm cache) measured Client 142.91 -> 150.86 FPS
(+5.56%; paired directions +2.59% and +8.67%). Host
comparison directions were mixed (+3.96% / -1.11%), so no added Host
gain is claimed. A separate eight-second84-enemy regression passed with stable
per-role live IDs through postphotos, measuring Host 88.99 / Client
60.66 FPS at a requested240cap; the cap was not attained. All five runs
had performance logging off and clean exits. These are finite first-level tests
on one PC, not global parity, latency quantiles or a complete campaign proof.
Native GUI QA also passed movement/fire, bot add/remove, private save, LAN
latejoin, leave/same-PID rejoin, original-main and actual SP fallback.


## Local Build208: original native auto-aim preferences

A new client connection starts the original AutoAimType option at Off after its
native gameplay profile is ready. The player can select On in the original menu;
that later choice survives actor recreation and level changes. AFK temporarily
enables the original option on the authoritative actor and the client's own menu,
then restores the previous human choice when control returns.

While AFK overrides the local option, input packets carry the saved human
preference. This prevents the first returning packet from replacing the restored
Off value with temporary On. AFK firing uses the original native getter when its
acquired direction is available and finite; other cases retain the camera-forward
fallback. Native camera/vehicle cone and marker policy continues to determine
availability.

Owned leases validate the actor, registry slot, controller, option variable and
profile, with player-generation and epoch checks in the session. Removal, level
exit and session cleanup release live overrides before their bindings disappear;
stale bindings receive no writes. NSC2 session checkpoints store the explicit
previous option mode and binding metadata. NSC1 session data derives that mode
from restored RAM, since its old flag represented marker state. The outer .coop
file's existing executable-identity check still applies.

All 66 automated tests and 21 private owned-PAL cases passed. The PAL cases cover
both player slots, prior modes, repeated AFK acquisition, unrelated preferences,
stale bindings and native-direction/manual fallback behavior. Final original-menu GUI validation passed for client defaults, AFK On/Off,
manual On, Host AFK menu reopening/return and same-process client rejoin. The Build207 client-cache and measured performance evidence
above remain historical results; no new FPS result is claimed for Build208.

The AFK host now receives a separate sanitized local UI snapshot inside the
same existing take_controls lock, before pressed edges are drained. Human
Start opens Pause; local menus receive human input with zero AI look. Gameplay
and remote AFK controls retain their prior suppression. Regression assertions
cover pressed-only Start, release, stale input and AFK toggles. The final suite
still has66 tests, with these assertions added to the existing session test.
Checkpoint load/return and manual-On AFK restoration were also verified in
separate earlier208 GUI segments; those complete runs stopped at the old host
menu route. No whole-run pass is attributed to the excluded attempts.


## Local Build210: Survival, spectators and LAN browser

Survival adapts the continued-attack/time-survived format to the six original
multiplayer maps. It uses the original cooperative rules, lobby and difficulty
menus with the existing multiplayer map carousel. The host owns seeded wave
quotas, spawn cadence and the one-life round roster. Death and late join become
forced spectators until a new round; defeat restarts after15 seconds. Original
enemy factories, map collision, pickups and native font rendering are retained.
The original enemies/assets come from each player's own disc data.

Voluntary watching is available from the original in-game Connection menu.
The authority separates manual preference from Survival locks and rejects
stale or forged requests. Watchers have neutral gameplay controls and do not
participate in collision, damage, targeting or skip votes. The following camera
uses a bounded render lease; it restores the local camera/body masks after
render completion. Original native Pause remains available for the watching
host without pausing other players. Leaving a vehicle and dropping mode
objectives uses original native routines before a body is deactivated.

LAN discovery retains one-click join and deduplicated stable room identity.
Bounded optional metadata adds map, mode, player/bot/spectator counts, rules,
running state and nonce-matched RTT. The original menu offers refresh, sorting,
mode filter and available-room filtering. Older/unknown optional extensions
remain safely readable, while gameplay compatibility moves to protocol66.

This is a local, uncommitted development build. No ZIP, upload or release is
authorized. Finite test evidence is recorded separately; no full-campaign,
all-hardware or240-FPS result is claimed for these features.
