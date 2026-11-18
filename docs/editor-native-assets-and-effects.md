# Editor helper assets and native effects

The editor draws its own vector helper symbols. These are source code, not
textures extracted from the game. The catalog distinguishes native area triggers,
switches, relays, counters, timers, conditions, spawners, doors, cameras, sound,
music, lights, explosions, paths and other supported actor classes. Semantic roles
come from decoded configuration; an object's editable display name is not a type.
Selected and linked helpers remain visible when nearby unselected helpers are
culled from the same screen cell.

Visible model surfaces are also selectable. Appearance loading caches local
bounds for each distinct vertex span; only a selection click transforms the ray
and tests candidate triangles, ordered nearest first. Ray distance remains in
world units under nonuniform scaling. The picker honors cutaway, model/category
visibility, terrain depth limits and binary material alpha, and uses the same
posed geometry and transform preview as rendering. A paired button visual selects
its proven logical Switch owner. A 20,001-batch CPU hit/miss fixture tests one
triangle for the nearest hit and none for the miss (3 ms combined on the test
machine); no triangle walk is added to rendered frames.

The visual convention of a small camera-facing helper is consistent with
[Unreal's sprite/billboard viewport helpers](https://dev.epicgames.com/documentation/en-us/unreal-engine/viewport-show-flags-in-unreal-engine).
The symbols themselves are original vector drawings in `map_logic_icons.cpp`.

## Original resource audit

The local PAL resource audit examined `Data.ssg`, `FrontEnd.ssg`, all 49 level
archives and `base.cfg`. The configuration retains developer marker flags for
Lights, Instances, GameItems and Cameras. No separate named Logic editor icon
pack was identified in those resources. Nothing from those archives is added to
the source distribution.

The native Arrow actor is type **4260**. Type 4150 is an enemy path marker.
`LevelModels.cfg` names `yellowarrow` as ObjectModel152. The original PAL Arrow
constructor (80076BBC) chooses `yellowarrow` for configuration kind 1 and
`greenarrow` for kind 2. The inspected maps contain 156 arrows, all kind 1;
`yellowarrow` is embedded in 45 of the maps. The appearance loader now loads this
real original model when available. Its full authored matrix is retained, rather
than using the pickup factory's orientation rules. Camera-dependent bobbing and
native moving-parent animation are intentionally not simulated by the static
editor view. A missing optional resource remains a helper symbol, not invented
replacement geometry.

Interactive native buttons use a Switch actor (4180) linked to a MovingBrush
(4070). For example, the original Alevel11_2 button arena1 brush references its
switch through configuration +0x24. Authoring converts the original visual brush
surface into a portable local ModelHolder instance and pairs it with the native
Switch; source geometry stays inside the user's private map package.

## Authored Light and Explosion controllers

`gc/editor_effect.h` defines a versioned, bounded ASCII description stored in the
native Trigger's existing relocated action field. Save, map package transfer and
map reload therefore carry the effect inside the SSW, without external files.
The description is parsed strictly and independently of the process locale.
At most eight Light controllers and 256 total effect controllers are accepted.

Native Trigger initialization (800846D0) and actual dispatch (80084C30) hide this
reserved description from the original ActionNames resolver. All normal Trigger
counter, delay, activation limit and output dispatch behavior stays native.
Descriptions with an unknown reserved schema are rejected instead of reaching
the original resolver. Native maps with ordinary actions follow the original
code unchanged.

Light controllers support Enable and Disable inputs. Their native enabled flag
at actor +0x8C creates or releases an original point light using the original
RenderEngine API (801431A8, 80122760, 801227CC, 8012280C, 80122844, 801219B0).
There is no replacement shader. Setters run only when necessary; an exhausted
native pool retries at a bounded interval. Removing the actor or leaving its map
releases its light before the original pools reset.

Explosion controllers use the original visual factory (800D653C), with one
original radial damage event (8000E330 / 8000DF58). The owner is explicitly null,
which the original environment damage constructor supports. Native Damager
(4110) is not a substitute: some Damager modes dereference the event activator
and cannot safely be used with an ownerless timer.

Only the authoritative host or offline game dispatches an authored explosion.
The existing native effect journal captures its original visual factory call;
clients render that journal and receive damage through authoritative world
state. They do not dispatch a second explosion or damage event. Light On/Off
already uses the Mission definition1 `trigger_enabled` snapshot property and its
validated client write to actor +0x8C; no parallel network message is needed.
The helper frame must run after client session application.

Runtime checkpoint section EDF1 stores host-side ownership of native Light
handles along with the RAM image. Inspection makes no native calls. Old states
without the optional section remain readable; after their RAM is restored, old
host ownership is discarded without releasing pointers against the new image.

## Validation

- Semantic helper drawings and density layout: 38 symbols, 24,703 CPU checks.
- Model surface picking: 50 checks, including actual original prop, enemy, Arrow
  and paired-button geometry, transformed previews, alpha holes, occlusion and
  dense nearest-first pruning.
- Full original appearance/material scan: 49 maps, 156 Arrow actors, 8,528,895
  checks; the two pre-existing unavailable Dragon resources remain reported.
- Effect schema: 86 checks, including locale-independent round trips and
  malformed, truncated, nonfinite and out-of-range fields.
- Fake-native lifecycle: 194 checks covering lights, native delay/dispatch
  delegation, damage/FX authority, cleanup and checkpoint rollback. This test
  uses no ROM, GPU or actual game renderer.
- `map_logic_native_fixture.cpp` creates a private real-game fixture combining
  interactive buttons, a two-enemy wave, blue light On/Off, and one delayed
  native explosion. It must also be exercised in the actual PAL game with host
  and client before calling visual/replication integration verified.
