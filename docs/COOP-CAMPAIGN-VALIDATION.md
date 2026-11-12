# PAL campaign integration checks

These checks run two native processes with private cards/settings and hidden
windows. They never capture desktop input. They require the owner's extracted
PAL G3BP9G game data and a built multiplayer executable; no game data or RAM
captures belong in the repository.

`tools/test_coop_campaign_chain.py --exe GAME --data GAME_DATA --card CARD
--output NEW_DIRECTORY` loads all 42 campaign assets, follows original WorldLink
events and results, and checks client native readback after reconciliation.
The same peers continue across successive ordinary exits. Original terminal
exits return to the menu or credits according to the original campaign profile.
`--roots LEVEL ... --stop-outside-roots` runs a bounded regression subset.

`tools/test_coop_campaign_bosses.py` accepts the same arguments. It activates
the authored Diablotaur, Hydra and Dark Lord spawners, dispatches original damage
receivers and reconnects the client during each encounter. Dark Lord hits obey
native vulnerability phases and revisit remaining regions. The report preserves
the actual native hit results and all client field comparison samples.

`tools/test_coop_campaign_vehicles.py` checks the initially hidden Clevel5_1
Jeep, Alevel10_4 Combine and Clevel6_1 vehicle: show/hide/show, host entry/exit,
client rejoin and client entry/input/exit. It uses original native lifecycle and driving
actions, then requires successful client reconciliation after each action.

Both drivers explicitly shortcut exploration, triggers and aiming. A passing
integration matrix does **not** establish a complete normal campaign playthrough,
all possible script orders, gameplay balance or Internet latency tolerance.
Screenshots and logs complement native readback; matching scalar fields alone
do not establish visual correctness.

The hidden integration command channel is enabled only when both
`SSNE_TEST_HIDDEN` and `SSNE_COOP_CAMPAIGN_COMMAND` are set on the host. It is
local to that child process, has no network command route, and is disabled
during ordinary play. Client readback additionally requires
`SSNE_COOP_CAMPAIGN_VERIFY`.

Campaign fixes use protocol 61:

- Accept a valid empty native geometry-model catalog in Alevel12_4.
- Preserve authored water/air spawn heights while retaining native collision
  and player separation checks.
- Transfer the Dark Lord's sixteen attached hit regions by index and scalar
  state; ownership and allocations remain local.
- Support all three original campaign vehicle layouts, including hidden vehicles,
  original mesh show/hide lifetimes and Combine sound loops.
- Accept native boss Netricsa parameters beyond 16 bits.
- Bind the authored unindexed destructible in Clevel6_2 by its checked native
  geometry resource and spawn position, retaining ambiguity rejection. The
  unindexed trigger in Alevel12_2 similarly binds by class and spawn, while
  outgoing script target references still require normal resolution.
- Use moving-brush level indices to distinguish coincident doors. Coincident
  unindexed pickups pair in original creation order and retain lifetime bindings;
  other ambiguous bindings remain rejected.
- Release player weapon/HUD camera reservations at native level shutdown before
  the original main-menu allocator reuses its camera pool.
- Initialize the shared results renderer, retire freed sparkle globals, reset
  the original stat-order cursor, and transfer result coefficients on clients.

`tests/coop_campaign_real_test.cpp` exercises adapters against private captured
PAL RAM/CPU fixtures: geometry bounds, vehicle roundtrip, Netricsa catalog,
collision-safe air spawns and Dark Lord region health/retirement/ownership.
It is an opt-in owned-data integration fixture, separate from ordinary CTest.
