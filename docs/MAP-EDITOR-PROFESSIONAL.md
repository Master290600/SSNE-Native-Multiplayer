# Scene and logic editing

Open **Map editor** in the normal launcher. The editor works on project copies;
opening an original level does not replace the original game data. Press **F1**
or **Help / Tutorial** for the interactive lessons and the searchable reference.
The help follows the selected game language (English, German or French).

## Viewport controls

| Action | Control |
| --- | --- |
| Select a model or logic symbol | Left click |
| Fly / look | Hold right mouse; WASD, Q/E down/up, Shift faster |
| Orbit the current pivot | Alt + left drag |
| Pan | Middle drag |
| Zoom toward/away from the pivot | Mouse wheel |
| Focus the selected object | F / Focus selection |
| Reset the view | Home / Reset view |
| Move / Rotate / Scale | W / E / R, or the tool buttons |
| Cancel the current transformation | Escape |
| Temporarily snap a transformation | Shift while dragging |

Tool shortcuts apply while the viewport has focus and text entry and flying
are inactive. A plain click selects without changing the camera position or
orbit pivot. Flight retains the viewing direction; orbit uses the displayed
camera pivot. Changing panels retains the view.

The gizmo uses red X, green Y and blue Z. A hovered handle turns yellow. Move
also has plane handles; Scale has a uniform centre handle. Local axes are
available for Move/Rotate; Scale uses the object's local axes. Grid distance,
angle and scale increments are configurable. A completed drag is one Undo step;
Escape restores the unchanged document.

Native actor capabilities differ. Scene models and Arrow Markers support full
TRS; enemy spawners support position and heading; point controllers support
position. Camera paths are translated together with their keys. Original moving
brushes/doors retain their native motion paths and cannot be moved by changing
only their origin. Their event inputs remain available.

## Creating logic

Use **Add logic** to create an Area, Relay, Counter, Timer, Gate, Key branch,
Button, Toggle, Light or Explosion. Place it in the viewport, then edit its
properties in the inspector. Symbols distinguish the native roles; private
implementation helpers are hidden from the normal outliner and viewport.

- An **Area** sends events when a player enters its volume.
- A **Relay** fans one signal out to several receivers.
- A **Counter** counts incoming signals before sending outputs.
- A **Timer** delays signals or schedules repeated ticks.
- A **Gate** forwards triggers only while enabled.
- A **Key branch** has separate accepted/denied outputs. It uses the original
  game key rules, including consuming an accepted key.
- A visible **Button** is activated with the game's Use action. It can be
  reusable or limited and can send several actions.
- A **Toggle** has separate switched-on and switched-off output pins.
- A **Light** is a native point light with colour and radius. Enable/Disable
  turns it on/off. There can be up to eight authored point lights per level.
- An **Explosion** has effect style/size, radius, damage and delay. Trigger
  fires it; Enable/Disable arms/disarms it. Damage uses native radial falloff.

New Buttons/Toggles reuse the original game's interactive button geometry;
the editor creates a separate visible model and native interaction controller.
Selecting its visible model selects the logical button. Both share the same
transform. Imported action templates retain their resources while foreign map
script callbacks and outputs are cleared.

## Connecting events

Open **Logic Graph**. Drag an output pin onto a compatible input, or click an
output and choose a target by name. The wire action is **Trigger**, **Enable**
or **Disable**. Each receiver exposes only the actions it actually supports.
Use separate outputs for fanout. Toggle and Key branch have named branch pins.

Click a wire to change its action or delete it. Undo/Redo also applies to links.
Drag node headers to arrange the canvas; this does not move game objects.
Middle drag pans, the wheel zooms, **Frame selection** centres a node and
**Arrange nodes** lays out the chain. Node layout is saved with the project.

Example:

1. Button output -> door Trigger (starts/advances its original motion path).
2. Another output -> Sound Trigger for the alarm.
3. Another output -> Timer Trigger, with a one-second delay.
4. Timer output -> enemy wave Trigger and Camera Trigger.

Use Enable/Disable for receivers that have an explicit state. A native door's
Trigger advances its authored motion; it is not a universal explicit Open/Close
command. Immediate synchronous cycles are rejected. A looping chain must retain
a positive delay to avoid recursively dispatching events in one game tick.

## Saving and testing

**Save project** stores the native level changes and the editor graph layout.
**Test in game** builds a private test copy and starts it through the normal
game path. Original levels remain intact. The existing live logic test view
reports observed controller state and wave progress.

Buttons and standard controllers use native actors and native event dispatch,
not an editor-only simulation. Authored lights and explosions use the original
PAL engine light/effect/damage functions through the matching runtime extension.
In multiplayer the host owns damage and authoritative events; clients receive
the session state/effect stream. All peers must use the matching runtime build.

Saving a project and publishing a Custom Level are separate actions. A Custom
Level may also have its own display name and preview image using the existing
Custom Level settings. No upload is required for local editing or testing.
