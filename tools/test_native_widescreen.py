"""Hidden serial GPU menu aspect regression; image appearance needs manual review.

Uses private settings/cards and process-local controls through Menus. The pass
flag covers dimensions, native menu routing, frame progress and process cleanup;
it does not certify background coverage, font/model proportions or alignment.
"""
from test_native_frontend import Menus, sha
from pathlib import Path
from types import SimpleNamespace
import argparse
import json
import struct
import traceback


def png_dimensions(path):
    with path.open('rb') as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:8] != b'\x89PNG\r\n\x1a\n' or header[12:16] != b'IHDR':
        raise ValueError('Incomplete PNG: ' + str(path))
    return list(struct.unpack('>II', header[16:24]))


def public_state(state):
    # Diagnostic states also contain the generated session PIN; retain only UI
    # lifecycle fields needed by this test's report.
    fields = ('scene', 'page', 'visible', 'active', 'gui', 'native_root',
              'native_focus', 'returning', 'lobby', 'pause')
    return {key: state[key] for key in fields if key in state}


def capture(menus, label, expected_output, window):
    menus.delay(.3)
    folder = menus.folder / 'host' / 'steps'
    existing = set(folder.glob('ui-*.png'))
    menus.press('host', 'UISHOT')
    def complete():
        for path in set(folder.glob('ui-*.png')) - existing:
            try:
                png_dimensions(path)
                json.loads(path.with_suffix('.json').read_text())
                return True
            except (OSError, ValueError):
                pass
        return False
    menus.wait(complete, 15)
    candidates = sorted(set(folder.glob('ui-*.png')) - existing)
    assert len(candidates) == 1, (label, 'unexpected screenshot count', len(candidates))
    source = candidates[0]
    perf = json.loads(source.with_suffix('.json').read_text())
    assert perf.get('output') == expected_output, (label, expected_output, perf.get('output'))
    assert perf.get('scale') == 1, (label, 'expected fixed 1x resolution', perf.get('scale'))
    image_size = png_dimensions(source)
    assert image_size == window, (label, 'window dimensions', window, image_size)
    image = folder / (label + '.png')
    source.rename(image)
    source.with_suffix('.json').rename(image.with_suffix('.json'))
    return {'label': label, 'image': str(image), 'image_dimensions': image_size,
            'gpu_output': perf['output'], 'guest_frame': perf.get('display_copies'),
            'wide': perf['wide'], 'state': public_state(menus.state())}


def run_case(args, spec, edges):
    name = spec['name'] + '-edges' + str(edges)
    row = {'name': name, 'aspect': spec['aspect'], 'hud_edges': edges,
           'window': spec['window'], 'expected_gpu_output': spec['output'],
           'passed': False, 'captures': []}
    case_args = SimpleNamespace(**vars(args))
    width, height = spec['window']
    case_args.private_settings = (
        '[graphics]\nrenderer=gpu\nscale=1\naspect=' + str(spec['aspect']) +
        '\nhud_edges=' + str(edges) + '\nside_fill=1\nvsync=0\n'
        'top_bar=0\nfullscreen=0\nwindow_w=' + str(width) +
        '\nwindow_h=' + str(height) + '\n[audio]\nmuted=1\n'
        '[general]\npause_unfocused=0\n')
    menus = Menus(case_args, '', args.output / name)
    def shot(label):
        value = capture(menus, label, spec['output'], spec['window'])
        row['captures'].append(value)
        return value
    try:
        menus.start_role('host', args.language)
        pid = menus.processes['host'].pid
        menus.wait(lambda: menus.state().get('native_root') == 0x3d
                   and not menus.state().get('returning'))
        first = shot('01-main')
        menus.network('host')
        shot('02-home')
        menus.choose('host', 0)
        menus.page('host', 2)
        shot('03-host')
        menus.press('host', 'B')
        menus.page('host', 1)
        menus.choose('host', 3)
        menus.wait(lambda: menus.state().get('native_root') == 0x3d
                   and not menus.state().get('returning'))
        shot('04-main-after-modal')
        menus.network('host')
        menus.choose('host', 0)
        menus.page('host', 2)
        menus.choose('host', 0)
        menus.wait(lambda: menus.state().get('active')
                   and menus.state().get('native_root') == 0xa7)
        menus.delay(1.5)  # Let the original hint animation settle before inspecting text clipping.
        shot('05-original-mode-chooser')
        menus.press('host', 'B')
        menus.wait(lambda: not menus.state().get('active')
                   and menus.state().get('native_root') == 0x3d
                   and not menus.state().get('returning'))
        last = shot('06-main-after-host')
        assert menus.processes['host'].pid == pid, 'Host start replaced process'
        assert first['gpu_output'] == last['gpu_output'], 'Return changed GPU dimensions'
        assert isinstance(first['guest_frame'], int) and last['guest_frame'] > first['guest_frame'], 'Guest frames stopped'
        # Reopen a native submenu once more after both return paths.
        menus.network('host')
        menus.choose('host', 3)
        menus.wait(lambda: menus.state().get('native_root') == 0x3d)
        row['passed'] = True
    except Exception:
        row['error'] = traceback.format_exc()
    finally:
        row['last_state'] = public_state(menus.state())
        menus.close()
        row.update(visible=menus.visible, focused=menus.focused,
                   exits={role: process.returncode for role, process in menus.processes.items()})
        row['passed'] &= not menus.visible and not menus.focused and all(
            code == 0 for code in row['exits'].values())
    print(name + ': ' + ('PASS' if row['passed'] else 'FAIL'), flush=True)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('exe', 'data', 'card', 'output'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--fps', type=int, default=60)
    parser.add_argument('--port', type=int, default=27860)
    parser.add_argument('--language', type=int, choices=(0, 1, 2), default=1)
    parser.add_argument('--edges', type=int, choices=(0, 1), nargs='+', default=(0, 1))
    args = parser.parse_args()
    for key in ('exe', 'data', 'card', 'output'):
        setattr(args, key, getattr(args, key).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    before = sha(args.card)
    result = {'passed': False, 'exe_sha256': sha(args.exe), 'cases': [],
              'scope': 'Serial hidden GPU/native-menu dimensions and lifecycle only; no campaign or network join verification.',
              'visual_review_passed': None,
              'manual_review_required': [
                  'Auto cases: original background artwork fills the window without 4:3 black or blurred side strips.',
                  'Original fonts, logo, widgets and 3D characters keep their intended proportions.',
                  'Compare hud_edges=0 and 1: menu artwork and widget alignment stay consistent.',
                  'Main after modal and after host cancel visually matches the initial Main layout.',
                  'Fixed 4:3 stays 4:3; its configured side fill is expected.']}
    matrix = (
        {'name': 'auto-960x540', 'aspect': 3, 'window': [960, 540], 'output': [853, 448]},
        {'name': 'auto-1260x540', 'aspect': 3, 'window': [1260, 540], 'output': [1120, 448]},
        {'name': '4x3-960x540', 'aspect': 0, 'window': [960, 540], 'output': [640, 448]},
    )
    try:
        for spec in matrix:
            for edges in args.edges:
                result['cases'].append(run_case(args, spec, edges))
                (args.output / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf8')
        result['passed'] = all(row['passed'] for row in result['cases'])
    except Exception:
        result['error'] = traceback.format_exc()
    finally:
        result['original_card_unchanged'] = sha(args.card) == before
        result['passed'] &= result['original_card_unchanged']
        (args.output / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps({'passed': result['passed'], 'cases': len(result['cases']),
                      'visual_review_passed': None, 'output': str(args.output)}, indent=2), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
