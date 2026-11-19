"""PAL Via Aurelia host/client regression for contested entry and independent Jeep cameras.

Use private cards and two hidden local processes. Placement sets up short approaches.
Both peers then press E through their keyboard adapters. Read both native camera
matrices on the authoritative peer and save the rendered views from both processes.
"""
import argparse
import json
import math
import shutil
import struct
from pathlib import Path

from test_coop_campaign import sha
from test_vehicle_input import AttachedMenus, VehiclePeers


ROLES = ('host', 'client')
ORIGINAL, EXTRA = 166, 32767
USE, FORWARD = 8, 26


def sample(peers, actors):
    peers.action('dump')
    ram = (peers.folder / 'host/campaign-capture.ram').read_bytes()

    def word(address):
        return struct.unpack_from('>I', ram, address - 0x80000000)[0]

    def position(address):
        return struct.unpack_from('>fff', ram, address - 0x80000000)

    table = word(0x802DB7C4)
    vehicles = {}
    for i in range(word(0x802DB7C8)):
        ref, _, wrapper = struct.unpack_from('>HHI', ram, table - 0x80000000 + 8 * i)
        if ref not in (ORIGINAL, EXTRA):
            continue
        assert word(wrapper + 0x48) == 0x80209BB8
        car = word(wrapper + 0x60)
        vehicles[ref] = dict(car=car, wrapper=wrapper, position=position(car + 0x20),
                             driver=word(car + 0x160), visible=word(wrapper + 0x7C))
    assert set(vehicles) == {ORIGINAL, EXTRA} and all(v['visible'] for v in vehicles.values()), vehicles
    players = {}
    for role, actor in actors.items():
        camera = word(actor + 0x368)
        matrix = struct.unpack_from('>12f', ram, camera + 0x24 - 0x80000000)
        players[role] = dict(ref=next((r for r, v in vehicles.items() if v['car'] == word(actor + 0x6C4)), None),
                             actor=actor, movement=word(actor + 0x340), position=position(actor + 0x20),
                             target=word(actor + 0x6BC), camera_mode=word(camera + 0xAC),
                             camera=[matrix[i] for i in (3, 7, 11)], yaw=position(actor + 0x290)[0])
    return dict(vehicles=vehicles, players=players)


def press_use(peers, roles):
    for role in roles:
        peers.raw(role, [USE])
    peers.delay(.24)
    for role in roles:
        peers.raw(role, [])
    peers.delay(1.2)  # Give the original use cooldown time to expire.


def place(peers, slot, position, offset_x=0, yaw=0):
    x, y, z = position
    x += offset_x - 4.8 * math.sin(yaw)
    z -= 4.8 * math.cos(yaw)
    peers.action(f'player_place {slot} {x:.4f} {y + 2:.4f} {z:.4f}')


def check_drivers(state, expected):
    for role, ref in expected.items():
        player = state['players'][role]
        assert player['ref'] == ref and (player['movement'] == 3) == (ref is not None), (role, ref, player)
    for ref, vehicle in state['vehicles'].items():
        occupants = [state['players'][r]['actor'] for r, claimed in expected.items() if ref == claimed]
        assert len(occupants) <= 1 and vehicle['driver'] == next(iter(occupants), 0), (ref, occupants, vehicle)


def check_views(state):
    errors = []
    for role, player in state['players'].items():
        if player['ref'] is None:
            continue
        own = state['vehicles'][player['ref']]['position']
        camera = player['camera']
        other = next(v['position'] for ref, v in state['vehicles'].items() if ref != player['ref'])
        # The native chase camera pulls back when driving (about 14 m here).
        if not (player['camera_mode'] == 2 and 0 < camera[1] - own[1] < 10 and math.dist(own, camera) < 22):
            errors.append(f'{role} camera {camera} outside own Jeep {own}')
        if math.dist(own, other) > 12 and math.dist(camera, own) >= math.dist(camera, other):
            errors.append(f'{role} camera closer to other Jeep')
    if all(p['ref'] is not None for p in state['players'].values()):
        if math.dist(*(p['camera'] for p in state['players'].values())) < 1:
            errors.append('Drivers have the same camera position')
    return errors


def run(args):
    peers = VehiclePeers(args)
    report = dict(passed=False, exe_sha256=sha(args.exe), phases={}, camera_errors=[])
    args.output.mkdir(parents=True, exist_ok=False)

    def write():
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')

    def record(label):
        state = sample(peers, actors)
        report['phases'][label] = state
        print(label, {r: p['ref'] for r, p in state['players'].items()}, flush=True)
        write()
        return state

    def views(label, state):
        report['camera_errors'].extend(f'{label}: {e}' for e in check_views(state))
        old = {r: set((peers.folder / r / 'steps').glob('ui-*.png')) for r in ROLES}
        peers.pulse('UISHOT', ROLES, .24)
        for r in ROLES:
            latest = set((peers.folder / r / 'steps').glob('ui-*.png')) - old[r]
            assert len(latest) == 1, (label, r, latest)
            shutil.copyfile(latest.pop(), args.output / f'{label}-{r}.png')
        write()

    def enter_parked(role, ref):
        state = sample(peers, actors)
        place(peers, ROLES.index(role), state['vehicles'][ref]['position'], yaw=state['players'][role]['yaw'])
        for _ in range(8):
            peers.delay(.2)
            approach = sample(peers, actors)
            if approach['players'][role]['target'] == approach['vehicles'][ref]['wrapper']:
                break
        assert approach['players'][role]['target'] == approach['vehicles'][ref]['wrapper'], approach
        for _ in range(2):
            press_use(peers, (role,))
            state = sample(peers, actors)
            player = state['players'][role]
            if player['ref'] == ref:
                return
            assert player['ref'] is None and player['target'] == state['vehicles'][ref]['wrapper'], state
        raise AssertionError(f'{role} still on foot after using Jeep {ref}')

    try:
        peers.start()
        peers.stage(args.level)
        menus = AttachedMenus(peers)
        peers.wait(lambda: all(menus.state(role).get('scene') == 4 and
                               menus.mouse_ack(role) for role in ROLES))
        for role in ROLES:
            if menus.state(role).get('info'):
                peers.raw(role, [43])  # Tab closes the automatic NETRICSA page.
                peers.delay(.24)
                peers.raw(role, [])
        peers.wait(lambda: all(not menus.state(role).get('info') and
                               peers.input_state(role).get('player_control') for role in ROLES))
        initial = peers.state()
        actors = {ROLES[o['slot']]: o['address'] for o in initial['objects']
                  if o.get('vtable') == 0x802121E8 and o.get('slot') in (0, 1)}
        assert set(actors) == set(ROLES)
        initial = record('initial')
        for slot, offset in enumerate((-.8, .8)):
            place(peers, slot, initial['vehicles'][EXTRA]['position'], offset)
        peers.delay(.8)
        approach = record('both-facing-extra')
        assert all(p['target'] == approach['vehicles'][EXTRA]['wrapper'] for p in approach['players'].values()), approach
        press_use(peers, ROLES)
        contested = record('contested')
        winners = [r for r, p in contested['players'].items() if p['ref'] == EXTRA]
        assert len(winners) == 1, 'Concurrent use must produce exactly one driver'
        winner = winners[0]
        loser = next(r for r in ROLES if r != winner)
        check_drivers(contested, {winner: EXTRA, loser: None})
        enter_parked(loser, ORIGINAL)
        occupied = {winner: EXTRA, loser: ORIGINAL}
        both = record('both-driving')
        check_drivers(both, occupied)
        views('both-driving', both)
        press_use(peers, ROLES)
        check_drivers(record('both-exited'), {r: None for r in ROLES})
        # Swap at the original parking area while the cars are at rest.
        occupied = {winner: ORIGINAL, loser: EXTRA}
        for role, ref in occupied.items():
            enter_parked(role, ref)
        peers.delay(.8)
        swapped = record('swapped')
        check_drivers(swapped, occupied)
        views('swapped', swapped)
        for role in ROLES:
            ref = occupied[role]
            previous = sample(peers, actors)['vehicles'][ref]['position']
            peers.hold(role, keys=[FORWARD])
            peers.delay(2.4)
            peers.hold(role)
            peers.delay(.6)
            moved = record(role + '-moved')
            check_drivers(moved, occupied)
            assert math.dist(previous, moved['vehicles'][ref]['position']) > 3
            views(role + '-moved', moved)
        checks = [line for line in peers.log('client').splitlines() if '[campaign-verify]' in line]
        assert checks and all('differences=0' in line for line in checks), 'Client world verification differed'
        assert not report['camera_errors'], report['camera_errors']
        report['passed'] = True
    finally:
        peers.close()
        write()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('exe', 'card', 'data', 'output'):
        parser.add_argument('--' + flag, type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument('--port', type=int, default=32916)
    args = parser.parse_args()
    args.level, args.fps = 'Rlevel1_3', 60
    run(args)
