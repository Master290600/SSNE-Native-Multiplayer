# Connected camera updates

The PAL cooperative host may omit the original per-frame world-camera update
for a disconnected reserved slot. Native actor simulation, animation updates,
render masks and UI-camera groups retain their existing behavior. Connected
players retain camera updates even while not ready.

The filter reads fresh registry/actor/camera/FOV-getter links and falls back to
the original expanded mask during session transitions, save/load, level/epoch
or join changes, spawn ambiguity, menus/results/cutscenes/movies, unknown classes,
nonzero camera shake, other camera modes, or overlapping storage. A missing or
failing guest-thread session gate also keeps the original path.
`SSNE_CONNECTED_CAMERA_UPDATES=0` selects that reference path.

The complete 62-case automated suite passes, including the final restored core.
Owned-PAL raw tests passed 47 guard cases per mode. With two connected players,
the original camera callbacks decreased from 8 to 2. Complete RAM matched the
reference, including a dynamic test that changed the active camera matrix;
omitted camera storage and actor bytes remained unchanged. Three original-menu
lifecycle QA runs and 61 screenshot checks also passed. These focused checks do
not establish complete campaign coverage.

A same-binary camera A/B summarized Host rates as 172.01 -> 174.24 FPS (+1.30%).
Substantial time drift and mixed comparison directions prevent a reliable FPS
gain claim. The verified result is reduced callback work with the checked native
behavior preserved. Separate host-publication performance evidence is documented
in `LOCAL-CHANGES.md`; it must not be attributed to the camera filter.
