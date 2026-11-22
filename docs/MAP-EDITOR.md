# Map Editor (Alpha 0.13)

Open **Map Editor** in the SSNE-RECOOP-Native launcher after installing your game.
The editor loads existing native CLIMAX `.ssw` maps from your installation.
Press **F1** or **Help / Tutorial** for searchable help and a guided practice map
in English, German or French. **Start practice** opens an in-memory copy.
For a list of camera controls, transform tools and logic actions, see the
[scene and logic guide](MAP-EDITOR-PROFESSIONAL.md).

1. Select a level and choose **Open original**.
2. Find an object by name or category. Player starts are green triangles;
   enemy spawners are red, weapons/ammunition blue and supplies purple.
3. Click a marker or list entry. In 3D, drag the red X, green Y or blue Z handle
   to constrain movement to that axis. Drag an icon to move in X/Z, or enter
   X, Y (height) and Z and choose **Apply position**.
4. Use **W**, **E** or **R** in the viewport for Move, Rotate or Scale.
   Available handles depend on the object. **Reset object** restores its original transform.
5. Enter an ASCII project name and choose **Save project**.
6. **Open project** restores saved changes. **Undo/Redo** and Ctrl+Z / Ctrl+Y
   undo or repeat edits; Ctrl+S saves. Leaving with unsaved edits offers
   Save, Discard or Cancel.
7. **Test in game** prepares a separate test copy in the background and starts
   the native game. Select the source level in the original game menus. Close
   the test game before saving and starting another test of the same project.

Projects are stored in `MapProjects/<name>` beside the launcher. `project.json`
records the original map hash and confirmed map changes. Structural projects also record the exported map hash and original transforms of added objects. Schema 1 transform projects remain compatible. The exported
native map is in `game_data/files/Levels/<source-level>.ssw`. Original maps,
script references and unrelated packed resources remain unchanged.

The test tree reuses immutable game assets with hard links where available,
but replaces the edited map and its private virtual-disc file table atomically.
It relocates the edited map to a non-overlapping virtual extent and updates its
size, so larger exports can be loaded by the original native disc reader.
The test game has its own executable copy, settings and copied memory card.
The installed game runtime and normal Play target are unchanged.

You can place and transform objects, add enemy waves and connect logic controllers.
The default viewport renders original visible level surfaces, material textures,
static prop instances, sky geometry, pickups/keys/custom items and enemy previews
from your own game data. Enemy spawners follow their original authored entity ID
to the enemy template; model names use the game's ObjectModels mapping. Characters
are skinned into the first original idle/stand frame with the original bone weights.
**Enemy models** toggles these preview instances. They follow their object/spawn
point during axis dragging, numeric edits, rotation, undo/redo and project reopen.
The inspector identifies the original model and pose. Script-only points without
a visual resource retain their icons. Missing assets are reported instead of
being replaced with a guessed model. Two Dragon references in Clevel8_4
have no matching model in the installed PAL assets; these remain marked.
The cutaway affects level surfaces without cutting large enemy previews in half.
Hold the right mouse button for free mouse look and WASD flight. Q/E move down/up,
Shift multiplies speed by four. The **Fly speed** slider sets metres per second.
Movement is time-based and diagonal movement is normalized. The cursor is captured
while flying and released on right-button release or focus loss. Text entry does
not move the camera. Alt-left-drag orbits; middle-drag pans; the wheel zooms and
F focuses the selection.
Ctrl-drag an icon to place it on a level surface; Shift snaps to a 0.25 m grid.
Escape cancels a drag. Releasing commits one undoable edit. Wireframe, a horizontal
cutaway (6 m above the camera target) and X-ray icons help inspect interiors.
Turn off **3D view** for the original top-down overview. **Fit view** frames the
level; **Focus view** frames the selected object.

**Textures + models** switches between the original asset view and the solid
collision surface view. The editor decodes original GX texture formats including
CMPR cutouts and RGBA alpha, uses the authored UV channels, material layers and
wrapping, and generates mipmaps for distant surfaces. Model transforms follow
object edits and drag previews without modifying source asset data.

The viewport shows a static preview of the level. Use **Test in game** to see
enemy AI, animation, dynamic effects and script-driven visibility in action.
Spawns and script-only objects keep their selectable icons. Terrain geometry is read-only.

Transform-only exports preserve the original compressed relocation block. Structural exports rebuild the relocation table, including each new pointer and excluding null links. Object IDs are unique signed 16-bit identifiers. Props retain their own instance, frame, mutable colors, bounds and optional destruction configuration while sharing immutable geometry/materials. Render instances append at the end to preserve existing scene indices; removed instances remain inert index slots. A no-op export
is byte-identical. Changed exports use a standard zlib stored-block stream,
so they can be larger than the originals. A project requires the same original
map hash when reopened. Cyclic/out-of-range object pointers, damaged compression,
non-finite transforms and unsupported transformations are rejected. Global scene records
without a transform are preserved but never offered as movable objects.

**Save project** writes your work to your computer. To play a shared level,
add it to **Custom Level** and select it when hosting a multiplayer session.

Material preview follows the native base-color mode and explicit material blend
option. Terrain mask alpha is kept separate from surface opacity; alpha testing
is enabled only for authored cutout materials. Transparent/additive materials
retain their own blending and depth ordering. This fixes the ground gaps and
incorrect wall colors seen in Clevel5_4.

## Add, duplicate and delete

**Add object...** opens a searchable palette grouped into Enemies, Weapons / pickups,
Props and Player starts. Use **Source map** to select the current map or any other installed original level.
Foreign assets are imported into the project when placed. Select a template, then left-click a visible
level surface in the 3D view. A yellow marker shows the placement point; Shift
snaps X/Z to 0.25 m. Esc or **Cancel placement** cancels. In top view placement
uses the current camera target height; adjust Y in the inspector afterwards.
Hold RMB and use WASD to fly while finding a position.

**Duplicate (Ctrl+D)** makes an independent copy 2 m along X. A spawner template
creates one active enemy/item at the chosen location; it does not duplicate its
story wave controller. New enemies are active instead of preload-only templates.
Pickups clear original trigger/attachment references. **Delete** or the Delete key
removes the selected supported object. Ctrl+Z / Ctrl+Y reverses/repeats creation
and deletion too, including its visible model. New-object origins survive save
and reopen, so Reset object returns an added object to its initial placement.

The last player start, enemy templates referenced by existing spawners, doors,
triggers and unsupported story/control types cannot be deleted or duplicated.
The editor places objects in an existing level and can edit supported native
logic connections. Adding or deleting gameplay objects can change the
encounter balance; test your project in the native game. Creation has bounded
map/ID/history limits and respects the original model-holder capacity.

## Objects and enemies from other maps (0.7)

Open the target level, choose **Add object...**, then use **Source map** to choose
another installed original level. Filter/search its templates by name or model,
select an enemy, pickup, prop or player start and click the target level surface.
Position, rotation, duplicate, delete and undo/redo work as for local templates.
Saving and reopening preserve imported data; **Test in game** loads the export
through the normal native importer. No manual asset-folder copying is required.

The importer follows original relocated asset pointers, keeps aliases and complete
vertex/animation/collision buffers, copies model/texture dependencies and connects
their directory/source records. It omits donor terrain and gameplay-script roots.
Every placed actor gets a fresh ID and private mutable configuration, frames,
colors and bounds. Imported source dependencies are cached once per source-map
hash and reused by subsequent placements, also after reopening. Deleting a placed
object keeps shared cached assets for other copies and future placements.
Existing resource names prefer the target map's resource to protect its current
models. Original maps and common game assets remain read-only.

This imports object archetypes and assets; it does not transplant a foreign
level's boss scripting, triggers, routes or campaign logic. Test special scripted
enemies in-game. Missing original assets remain reported by the preview. The
project retains bounded format/ID/model-holder limits, and very large mixtures
remain subject to the native game's memory budget.

## Area triggers, enemy waves and project Survival (0.8)

Place enemies with **Add object...**, then choose **Add trigger**. Move the area
with the usual position fields or 3D handles. In its properties set **Radius (m)**
and **Delay (s)**, choose **Link enemy**, and set the count for each spawn point.
Only newly placed enemies can be linked; each linked enemy becomes a preload-only
template and appears through its spawner after a player enters the sphere.
Move that enemy to change its spawn point. Press **Apply trigger**, then save.

Default triggers fire once. **Repeat on re-entry** enables further activations
when a player leaves and enters again. **Max activations** bounds the number of
activations (2–4096). Each entry consumes one activation, including entries by
different players. Re-entry while a wave is pending queues another wave, with
the configured delay for each activation. Up to ten distinct enemy spawn points
and 1–64 enemies per point are supported. Undo/redo and project reopening preserve
the native area, relay and spawner configuration. Original story scripts remain
protected; deleting a trigger restores unreferenced templates as active enemies.

**Play Survival** saves the project and starts its edited base map as a native
Survival host. Choose an original game profile if requested. Automatic Survival
waves and authored trigger waves can coexist. This uses a private exported map,
disc table, resource alias and test memory card; original maps and saves stay
unchanged. For a shared session, add the project to **Custom Level** and choose
the package as host. Test very large waves against the game's memory budget.

## Camera orientation correction (local 0.8.1)

The 3D camera now follows the original game: local +Z forward, +X right.
Rendering, mouse picking, WASD flight, panning and object handles use the same
basis. Mouse-look yaw follows the corrected right direction. Map vertices,
object transforms and saved project coordinates are unchanged.

## Native model facing (local 0.8.2)

Enemy and pickup previews use the original actor-factory placement rules instead
of applying the authored helper basis to their meshes. Enemy templates start at
zero yaw; enemy spawner previews use the original derived spawn yaw. Pickups show
their initial upright pose; they turn during gameplay. AI can change enemy facing
after spawning. Original scene props retain their full authored transforms.
Changing a preview never rewrites map positions, rotations, models or textures.

## Visible activation graph and real logic tests (local 0.9)

Enable **Connections** to see directional activation links in the 3D and top
views. Click the middle of a link to inspect its target. **Logic / Test** lists
triggers, wave spawn points, counters, switches and doors; search by name, role
or native ID. **Selected chain** limits the overlay to four upstream/downstream
steps. **Focus node** centers its location. Incoming links are possible event
paths, not an implied AND condition; reference links do not send activations.

**Test logic / Run logic test** saves a private map export and launches that
level as a native host, using a separate memory card and an automatically chosen
port. **Test Survival + logic** runs the same observations in Survival. Return
to the editor to inspect the game's sampled state. A fresh session token, map
identity, level and sequence prevent mixing a different or old run with the
edited map. Edits require a new test; no gameplay event is fired by the monitor.

The test panel reports verified native states: relay/counter counts and limits,
pending spawn delays, queued waves, spawned totals, key-door-controller state,
moving-brush motion and observed changes. Enemy death targets can identify live
enemies still capable of signalling a particular counter. A moving brush at its
initial position is not automatically called closed; generic path endpoints do
not prove open/closed. Unknown logic or unresolved links remain explicit.

Diagnostics are opt-in and sampled at most four times a second; they are state
observations, not a complete callback trace. A paused/stopped game becomes
**not live** after three seconds. Last matching recorded states and observations
remain readable with that label; live colors are disabled. Existing story logic,
map files, geometry and user saves are preserved. This is targeted diagnostics,
not an assertion that every custom script or full campaign condition is decoded.

## Edit activation links in the viewport (local 0.10)

Enable **Edit links**, click a source in the 3D view, then a wave spawn point or
door. Review the action in the right-hand **Connection editor** and press
**Add link**. A preview line follows the target selection. Wave helpers
have separate diamond icons; overlapping icons offer a named selection. The
searchable Source / Target lists and Focus buttons also reach hidden helpers.
Right mouse + WASD continues to fly; link picking does not move objects.

Click an existing arrow to select its exact native output. **Pick target**
changes its destination; **Action** changes Trigger / Activate / Deactivate
where supported. **Apply link** commits those changes. **Delete link**
(or Delete while in this mode) removes only that output. **New link**
chooses an empty output; full controllers require explicitly editing an existing
connection. Duplicate endpoint pairs retain distinct native slots. Template,
reference and internal area-to-relay links remain protected.

An editor trigger can target a newly placed enemy model; **Wave count** creates
its real native wave spawner. Existing waves are selected through their spawn
point. Door/control connections survive later changes to the trigger's enemy
rows, delay and repeat settings. Relay outputs share the original ten-slot
limit. A door-only trigger remains enabled without enemy rows.

Trigger requests native door motion or a wave. Activate / Deactivate works on
supported area triggers, door controllers, relays and switches. Unsupported
action/target pairs cannot be newly created. Enemy-death outputs fire per enemy.
Opening a door after an entire wave
requires an actual counter, rather than treating incoming lines as an AND gate.
Edits support Undo / Redo and **Save project**, persist in the exported SSW, and
invalidate observations from an older test until a new test starts. Original
maps and normal saves stay unchanged.

## Custom Level category, names and previews (local 0.11)

Open a map project and choose **Custom level...**. Enter its display name and a
package ID. **Choose PNG** imports a preview; **Use current 3D view** captures
the editor camera without icons or handles. The original map preview can also be
used. Select the supported modes and press **Add / update in game**. Reusing a
package ID updates that custom level; a different ID creates a separate entry,
including another variant of the same base map. Preview/name preferences remain
with the editor project and are retained when it is reopened.

Start **Play** in the normal launcher. Its **Custom Level** category appears in
the original in-game level carousel, with the selected name below the thumbnail.
The category keeps the original frames, navigation, sounds and difficulty flow.
The category button and labels follow the selected game language. Original maps
and the original Lost Levels category remain available with their normal unlock
rules. Only packages enabled for the current game mode are listed.

Local packages live in `CustomLevels/<package-ID>/` and contain `level.json`,
`map.ssw` and optionally `preview.png`. The launcher prepares a separate cached
virtual disc under `CustomLevelData/`. Each map gets a unique alias; originals
are retained and multiple copies of a base level can coexist. Completed caches
are immutable, including when host and client run on the same PC. Changes to a
package become available after restarting the game through the launcher.

For multiplayer, the host automatically transfers the selected package to each
joining player, including map bytes, display name, base map, supported modes and optional
PNG preview. The original Connecting page shows progress and Cancel in the
selected menu language. A client validates the complete package before mounting
it into the running game's virtual disc; no restart or manual package copy is
needed. Custom-map changes during a session use the same process.

Received versions live separately in `DownloadedCustomLevels/`. Authored local
packages, memory cards and original maps are preserved. The manifest is checked
on each join, while identical verified packages reuse their cache without a
payload download. Changed names or previews refresh even if the map bytes stay
the same. Invalid or interrupted transfers cannot become active maps. Use
version 1.2.8.4-beta.1 (multiplayer build 267 R15s, protocol 71) on the host and clients.
