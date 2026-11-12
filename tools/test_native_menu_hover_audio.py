"""Actual native menu-hover sound acceptance in a private hidden game process.

Pointer commands use the same SDL/guest adapter as physical motion. Original
controller navigation is an independent baseline. The observer records the
original GUI wrapper's real sound/voice handle; bounded existing AX voice capture
proves decoded mono and main L/R contribution before intentional master mute.
This is menu audio acceptance, not a full campaign or physical speaker test.
"""
from pathlib import Path
from array import array
import argparse
import json
import sys
import traceback

from test_native_mouse import MouseMenus, Pointer, COMMAND_FILE, ident, public_menu
from test_coop_campaign import sha

SOUND_ID = 0x1C7
EVENT_BASE = 0x500000000
EVENT_FILE = 'native-menu-audio-events.jsonl'


class AudioMenus(MouseMenus):
    def audio_events(self):
        path = self.folder / 'host' / EVENT_FILE
        if not path.exists():
            return []
        lines = path.read_text(encoding='utf8').splitlines()
        rows = []
        for index, line in enumerate(lines):
            try:
                event = json.loads(line)
            except ValueError:
                if index == len(lines) - 1:
                    break  # Writer may be finishing the last line.
                raise
            for key in ('sequence', 'frame', 'sound_id', 'handle', 'record', 'parameter_block'):
                assert isinstance(event.get(key), int), ('Missing numeric audio observation', key, event)
            assert event['source'] in ('mouse', 'native'), event
            assert event['sequence'] > EVENT_BASE, event
            if rows:
                assert event['sequence'] > rows[-1]['sequence'], ('Duplicate/reordered audio event', event)
            rows.append(event)
        return rows

    def sound_count(self):
        value = self.mouse().get('focus_sounds')
        assert isinstance(value, int), ('Candidate lacks actual focus-sound diagnostics', self.mouse())
        return value


def mark(menus):
    events = menus.audio_events()
    return menus.sound_count(), events[-1]['sequence'] if events else EVENT_BASE


def mouse_navigation_since(menus, sequence):
    return [event for event in menus.audio_events()
            if event['sequence'] > sequence and event['source'] == 'mouse' and event['sound_id'] == SOUND_ID]


def record(report, action, **values):
    report['steps'].append({'action': action, **values})
    print(action, flush=True)


def positive_hover(pointer, wanted, root, report):
    m = pointer.m
    assert m.state().get('native_focus') != wanted, ('Need a different original focus', wanted, m.state())
    before, sequence = mark(m)
    widget = pointer.hover(wanted, root=root)
    m.wait(lambda: m.sound_count() >= before + 1, 8)
    m.delay(.20)
    assert m.sound_count() == before + 1, ('Repeated sound during one hover', before, m.sound_count())
    events = mouse_navigation_since(m, sequence)
    assert len(events) == 1, ('Expected one original navigation sound', events)
    event = events[0]
    assert event['handle'] >= 0 and event['record'] and event['parameter_block'], ('Original sound has no real AX voice', event)
    assert m.state().get('native_focus') == wanted, ('Sound without actual original focus', wanted, m.state())
    record(report, 'focus_change_plays_one_original_sound', root=root, id=wanted, event=event)
    return widget


def quiet(pointer, action, report, label):
    before, sequence = mark(pointer.m)
    focus_before = pointer.m.state().get('native_focus')
    action()
    pointer.m.delay(.20)
    assert pointer.m.sound_count() == before, (label, before, pointer.m.sound_count())
    assert not mouse_navigation_since(pointer.m, sequence), (label, pointer.m.audio_events())
    assert pointer.m.state().get('native_focus') == focus_before, (label, focus_before, pointer.m.state())
    record(report, 'no_repeated_navigation_sound', label=label, count=before, focus=focus_before)


def quiet_motion(pointer, widget, report, label):
    a, b, c, d = map(float, widget['bounds'])
    center = pointer.center(widget)
    dx, dy = min((c-a)*.08, .005), min((d-b)*.08, .003)
    def jitter():
        for direction in (-1, 1, -1, 1, 0):
            pointer.move([center[0] + dx*direction, center[1] + dy*direction])
        pointer.m.delay(.4)
    quiet(pointer, jitter, report, label + '_same_row_jitter')
    quiet(pointer, lambda: (pointer.move([-.05, .5]), pointer.move(center)), report, label + '_outside_then_same_focus')
    quiet(pointer, lambda: (pointer.cancel(), pointer.move(center)), report, label + '_cancel_then_same_focus')


def quiet_disabled(pointer, report, root):
    widgets = [w for surface in pointer.observe().get('surfaces', []) if ident(surface['root']) == root
               for w in surface.get('widgets', [])]
    active = [w for w in widgets if w.get('selectable') and w.get('allowed')]
    observed = 0
    for widget in widgets:
        if widget.get('selectable') and widget.get('allowed'):
            continue
        try:
            center = pointer.center(widget)
        except (AssertionError, KeyError, TypeError):
            continue
        if any(w['bounds'][0] <= center[0] < w['bounds'][2] and w['bounds'][1] <= center[1] < w['bounds'][3] for w in active):
            continue  # A disabled label must not overlap another legitimate hit.
        quiet(pointer, lambda: pointer.move(center), report, 'disabled_native_control_' + str(widget.get('id')))
        observed += 1
        if observed == 2:
            break
    record(report, 'disabled_hover_observation', root=root, observed=observed,
           scope='Only disjoint, live disabled controls count; CPU tests cover denied/stale controls separately.')


def two_rows(pointer, root, ids, report, label):
    changed = 0
    last = None
    for wanted in list(ids) * 2:
        if pointer.m.state().get('native_focus') != wanted:
            last = positive_hover(pointer, wanted, root, report)
            changed += 1
            if changed == 2:
                break
    assert changed == 2, (root, ids, pointer.m.state())
    quiet_motion(pointer, last, report, label)
    quiet_disabled(pointer, report, root)
    return last


def pcm_summary(path):
    raw = path.read_bytes()
    assert raw and len(raw) % 2 == 0, ('Missing/truncated decoded PCM', str(path), len(raw))
    values = array('h'); values.frombytes(raw)
    if sys.byteorder != 'little':
        values.byteswap()
    nonzero = sum(value != 0 for value in values)
    assert nonzero > 0, ('Original sound decoded silence', str(path))
    return {'path': str(path), 'bytes': len(raw), 'samples': len(values), 'nonzero_samples': nonzero,
            'peak': max(abs(value) for value in values), 'sha256': sha(path)}


def verify_pcm(menus, report):
    navigation = [event for event in menus.audio_events()
                  if event['sequence'] > report['audio_observation_start'] and event['sound_id'] == SOUND_ID]
    assert navigation, 'No actual original navigation sounds observed'
    sources = {event['source'] for event in navigation}
    assert {'mouse', 'native'} <= sources, ('Missing real controller/mouse baseline', sources)
    captures = []
    for event in navigation:
        assert event['handle'] >= 0 and event['record'] and event['parameter_block'], event
        prefix = menus.folder / 'host' / ('audio-weapon-shot-' + str(event['sequence']))
        mono = pcm_summary(Path(str(prefix) + '.pcm'))
        stereo = pcm_summary(Path(str(prefix) + '.stereo.pcm'))
        assert stereo['samples'] == mono['samples'] * 2, ('Unexpected AX stereo frame count', event, mono, stereo)
        captures.append({'event': event, 'mono': mono, 'main_stereo_contribution': stereo})
    report['actual_original_navigation_pcm'] = captures
    report['controller_mouse_comparison'] = {'sound_id': SOUND_ID, 'sources': sorted(sources),
        'all_real_handles_and_decoded_nonzero': True,
        'scope': 'Same original navigation sound455 (sole asset sample516, GUI flags0). Capture alignment and length are timing-sensitive, so PCM hashes need not match. Actual AX mono and main L/R contributions are nonzero before intentional master mute.'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for key in ('exe', 'data', 'card', 'output'):
        ap.add_argument('--' + key, type=Path, required=True)
    ap.add_argument('--language', type=int, choices=(0, 1, 2), default=1)
    ap.add_argument('--fps', type=int, default=240)
    ap.add_argument('--port', type=int, default=28470)
    a = ap.parse_args()
    for key in ('exe', 'data', 'card', 'output'):
        setattr(a, key, getattr(a, key).resolve())
    assert all(str(getattr(a, key)).isascii() for key in ('exe', 'data', 'card', 'output')), 'Existing game runtime needs ASCII test paths'
    assert a.fps > 0 and 1024 < a.port < 65534
    a.output.mkdir(parents=True, exist_ok=False)
    a.private_settings = '[graphics]\nrenderer=gpu\nscale=1\naspect=3\nhud_edges=1\nside_fill=1\nvsync=0\nwindow_w=960\nwindow_h=540\ntop_bar=0\nfullscreen=0\n[audio]\nmuted=1\nmusic_volume=100\nsfx_volume=100\n[general]\npause_unfocused=0\n'
    a.test_env = {'SSNE_TEST_DEVICE_INPUT': '1', 'SSNE_NATIVE_MOUSE_COMMAND': COMMAND_FILE,
                  'SSNE_TEST_MENU_HOVER_AUDIO': '1', 'SSNE_COOP_TRACE_LIFETIMES': '1',
                  'SSNE_PAD_SCRIPT_FILE': 'input.txt', 'SSNE_NATIVE_MENU_COMMAND': 'command.txt',
                  'SSNE_TEST_SCREENSHOT_DIR': 'steps', 'SSNE_PERF_LOG': 'performance.jsonl'}
    assert 'SSNE_COOP_TEST_CAPTURE_MUSIC' not in a.test_env
    original = {'card': sha(a.card), 'exe': sha(a.exe), 'dol': sha(a.data / 'sys/main.dol')}
    report = {'passed': False, 'exe_sha256': original['exe'], 'language': a.language, 'fps': a.fps,
              'steps': [], 'scope': __doc__}
    m = AudioMenus(a, '', a.output)
    pointer = Pointer(m, report)
    try:
        m.start_role('host', a.language)
        m.wait(lambda: m.mouse().get('surfaces') and m.mouse_ack().get('image_rect'), 20)
        pointer.expect_root(0x3D); pointer.cancel(); m.delay(.4)
        assert 'focus_sounds' in m.mouse(), 'Candidate did not expose new focus sound diagnostics'
        before, sequence = mark(m); initial_focus = m.state().get('native_focus')
        report['audio_observation_start'] = sequence
        m.press('host', 'DOWN')  # Independent original menu/controller baseline.
        m.wait(lambda: m.state().get('native_focus') != initial_focus and any(
            e['sequence'] > sequence and e['source'] == 'native' and e['sound_id'] == SOUND_ID for e in m.audio_events()), 10)
        assert m.sound_count() == before, 'Controller navigation was falsely attributed to mouse'
        record(report, 'original_controller_navigation_baseline', before=initial_focus, after=m.state().get('native_focus'))
        two_rows(pointer, 0x3D, (0x40, 0x3E), report, 'main')
        # Direct Down changes focus once, and the matching Up still activates once.
        if m.state().get('native_focus') == 0x41:
            positive_hover(pointer, 0x40, 0x3D, report)
        target = pointer.widget(0x41, root=0x3D); point = pointer.center(target)
        before, sequence = mark(m); activations = pointer.observe().get('activations', 0)
        pointer.button(True, point); m.wait(lambda: m.sound_count() >= before + 1, 8)
        assert m.sound_count() == before + 1 and len(mouse_navigation_since(m, sequence)) == 1
        pointer.button(False, point); pointer.expect_root(0xF0); m.delay(.3)
        assert m.sound_count() == before + 1, 'Release duplicated the focus sound'
        assert pointer.observe().get('activations', 0) == activations + 1, 'Direct click no longer activates once'
        record(report, 'direct_down_focus_one_sound_up_one_activation', event=mouse_navigation_since(m, sequence)[0])
        two_rows(pointer, 0xF0, (0xF8, 0xF5), report, 'options')
        pointer.back(); pointer.expect_root(0x3D)
        pointer.click(0x3F, root=0x3D); pointer.expect_root(0x148)
        m.wait(lambda: m.state().get('page') == 1, 12)
        two_rows(pointer, 0x148, (0x14B, 0x14A), report, 'multiplayer_home')
        pointer.back(); pointer.expect_root(0x3D)
        m.press('host', 'UISHOT')
        report['final_native_state'] = public_menu(m.state())
        m.close()  # Flush all real short voice captures before reading their bytes.
        verify_pcm(m, report)
        report['passed'] = True
    except Exception:
        report['error'] = traceback.format_exc(); print(report['error'], flush=True)
        for role, process in m.processes.items():
            if process.poll() is None:
                try:
                    m.press(role, 'UISHOT')
                except Exception:
                    pass
    finally:
        m.close()
        unchanged = {'card': sha(a.card) == original['card'], 'exe': sha(a.exe) == original['exe'],
                     'dol': sha(a.data / 'sys/main.dol') == original['dol']}
        report.update(original_files_unchanged=unchanged, visible=m.visible, focused=m.focused,
                      exits={role: process.returncode for role, process in m.processes.items()},
                      observed_audio_events=m.audio_events())
        report['passed'] &= bool(m.processes) and all(unchanged.values()) and not m.visible and not m.focused and all(
            process.returncode == 0 for process in m.processes.values())
        (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
        print(json.dumps({key: value for key, value in report.items() if key not in ('steps', 'actual_original_navigation_pcm', 'observed_audio_events')}, indent=2), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
