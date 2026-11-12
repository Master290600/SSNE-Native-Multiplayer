"""Bounded local renderer profiling with isolated saves and explicit backend selection."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time
from itertools import product

STATIONARY_SCRIPT = 'B@14:0.25,B@18:0.25'
MOTION_SCRIPT = (STATIONARY_SCRIPT + ',CRIGHT@22:0.2,SHOT@22.5:0.1,SUP@23:0.7,'
                 'SHOT@24:0.1,FIRE@25:0.7,SHOT@25.3:0.1,X@27:0.1,SHOT@27.5:0.1,'
                 'FIRE@28:0.7,SHOT@28.3:0.1,CLEFT@30:0.2,SHOT@30.5:0.1')


def read_samples(path):
    return [v for line in path.read_text(encoding='utf-8').splitlines()
            if (v := json.loads(line)).get('type') == 'sample']


def summarize(samples, start, end, level, scale, fps, required_players=0):
    rows = [v for v in samples if start <= v['elapsed_s'] <= end
            and v.get('scene') == 4 and v.get('level') == level
            and not v.get('paused') and not v.get('background_mode')
            and not v.get('vsync') and v.get('scale') == scale
            and v.get('fps_limit') == fps
            and (not required_players or (v.get('local_ready')
                 and v.get('ready_players', 0) == required_players))]
    if len(rows) < 8:
        raise ValueError(f'Only {len(rows)} valid gameplay windows; need at least 8')
    measured_seconds = sum(r['window_s'] for r in rows)
    result = {'measured_seconds': measured_seconds, 'windows': len(rows), 'first_s': rows[0]['elapsed_s'],
              'last_s': rows[-1]['elapsed_s'], 'level': level, 'scale': scale,
              'required_ready_players': required_players,
              'fps_limit': fps, 'stage_spans_overlap': True,
              'gpu_time_scope': 'Mean per timed execute; not total GPU time per displayed frame'}
    result['detail_sampling'] = sorted({r.get('detail', {}).get('sampling', 'legacy-fixed') for r in rows})
    for key in ('native_fps', 'render_fps', 'updates_per_s', 'entities', 'ping_ms',
                'last_applied_motion_age_ms'):
        values = [r[key] for r in rows if r.get(key, -1) >= 0]
        result[key] = {'median': statistics.median(values), 'min': min(values),
                       'max': max(values)} if values else None
    gpu = [r['gpu_ms_async'] for r in rows if r.get('gpu_timer_valid')]
    result['gpu_ms_async'] = {'valid_windows': len(gpu),
                              'median': statistics.median(gpu) if gpu else None}
    result['stages'] = {}
    for key in sorted({key for r in rows for key in r.get('stages', {})}):
        spans = [r['stages'][key] for r in rows if key in r.get('stages', {})]
        count = sum(s['calls'] for s in spans)
        result['stages'][key] = {
            'calls': count, 'calls_per_s': count/measured_seconds, 'avg_ms': sum(s['avg_ms']*s['calls'] for s in spans)/count,
            'max_ms': max(s['max_ms'] for s in spans),
            'over33ms': sum(s['over33ms'] for s in spans)}
    result['detail'] = {}
    for name in sorted({name for r in rows for name in r.get('detail', {}).get('roots', {})}):
        roots = [r['detail']['roots'][name] for r in rows
                 if name in r.get('detail', {}).get('roots', {})]
        n = sum(r['samples'] for r in roots)
        cpu_n = sum(r['cpu_samples'] for r in roots)
        if not n:
            continue
        spans = {}
        for key in sorted({key for r in roots for key in r['spans']}):
            spans[key] = sum(r['spans'].get(key, {}).get('self_ms', 0) for r in roots)/n
        result['detail'][name] = {
            'samples': n, 'wall_ms_per_sample': sum(r['wall_ms'] for r in roots)/n,
            'unattributed_self_ms_per_sample': sum(r.get('unattributed_self_ms', 0) for r in roots)/n,
            'thread_cpu_ms_per_sample': sum(r['thread_cpu_ms'] for r in roots)/cpu_n if cpu_n else None,
            'work_per_sample': {key: sum(r.get('work', {}).get(key, 0) for r in roots)/n
                                for key in sorted({k for r in roots for k in r.get('work', {})})},
            'self_ms_per_sample': dict(sorted(spans.items(), key=lambda pair: -pair[1]))}
    return result


def run_case(args, directory, pair, scale, interpolation='on', predicted_upload='default', reprojection='default', position_storage='default', frame_batch='default'):
    processes, streams, roles = [], [], ['host', 'client'] if pair else ['host']
    settings = (f'[graphics]\nrenderer = {args.renderer}\nantialiasing = off\nanisotropy = {args.anisotropy}\n'
                f'scale = {scale}\naspect = 1\nfps_limit = {args.fps}\n'
                'vsync = 0\nsync_to_display = 0\nwindow_w = 960\nwindow_h = 540\n'
                'window_maximized = 0\nfullscreen = 0\n[audio]\nmuted = 1\n'
                '[general]\npause_unfocused = 0\n')
    metadata = {'pair': pair, 'scale': scale, 'renderer': args.renderer, 'anisotropy': args.anisotropy, 'commands': {}, 'exits': {},
                'timeout': False, 'duration_seconds': args.duration, 'interpolation': interpolation,
                'motion_exercise': args.exercise_motion or args.exercise_native_motion, 'motion_trace': args.exercise_motion, 'predicted_upload': predicted_upload,
                'reprojection': reprojection, 'position_storage': position_storage, 'frame_batch': frame_batch,
                'verify_reprojection': args.verify_reprojection, 'profile_ui_gl': args.profile_ui_gl,
                'capture_frames': args.capture_frames,
                'input_script': args.input_script or (MOTION_SCRIPT if (args.exercise_motion or args.exercise_native_motion) else STATIONARY_SCRIPT)}
    try:
        for role in roles:
            folder = directory / role
            folder.mkdir(parents=True)
            executable = folder / args.exe.name
            # An immutable local hard link avoids six copies of the executable.
            # Fall back to copying for another volume. The source is never edited.
            try:
                os.link(args.exe, executable)
            except OSError:
                shutil.copy2(args.exe, executable)
            shutil.copy2(args.card, folder / 'test-card.raw')
            (folder / 'settings-netplay.ini').write_text(settings, encoding='utf-8')
            if args.shader_cache:
                shutil.copytree(args.shader_cache, folder / ('shadercache-dx11' if args.renderer == 'dx11' else 'shadercache'))
            env = {k: v for k, v in os.environ.items() if not k.startswith('SSNE_')}
            env.update(SSNE_TEST_HIDDEN='1', SSNE_SCREENSHOT=str(folder/'game.bmp'),
                       SSNE_SCREENSHOT_MS=str((args.duration+(3 if role == 'host' and pair else 0))*1000),
                       SSNE_PERF_LOG=str(folder/'performance.jsonl'), SSNE_PERF_DETAIL_EVERY='16')
            env['SSNE_PAD_SCRIPT'] = metadata['input_script']
            if args.profile_ui_gl:
                env['SSNE_PROFILE_UI_GL'] = '1'
            if args.exercise_motion or args.exercise_native_motion:
                env['SSNE_TEST_SCREENSHOT_DIR'] = str(folder/'steps')
            if args.exercise_motion:
                env['SSNE_DEV_SMOOTHNESS'] = '1'
            if args.capture_frames and role == 'client':
                captures = folder / 'gpu-captures'
                captures.mkdir()
                first, count = args.capture_frames
                env['SSNE_GPU_CAPTURE'] = f'{captures}:{first}:{count}'
            if interpolation == 'off':
                env['SSNE_PROFILE_NATIVE_ONLY'] = '1'
            if predicted_upload != 'default':
                env['SSNE_PROFILE_COMPACT_UPLOAD'] = '1' if predicted_upload == 'compact' else '0'
            if reprojection != 'default':
                env['SSNE_PROFILE_BATCH_REPROJECTION'] = '1' if reprojection == 'four-vertex' else '0'
            if position_storage != 'default':
                env['SSNE_PROFILE_PACKED_POSITIONS'] = '1' if position_storage == 'packed' else '0'
            if frame_batch != 'default':
                env['SSNE_PROFILE_SNAPSHOT_BATCH'] = '1' if frame_batch == 'snapshot' else '0'
            if args.verify_reprojection and role == 'client':
                env['SSNE_VERIFY_BATCH_REPROJECTION'] = '1'
            # Keep the camera alive and stationary. Explicit fixture, not campaign acceptance.
            if role == 'host':
                env['SSNE_NETPLAY_MOD_FLAGS'] = '1'  # god mode only
            cmd = [str(executable), '--data', str(args.data), '--memcard', str(folder/'test-card.raw'),
                   '--lan-port', str(args.port), '--lan-pin', '123456', '--fps', str(args.fps),
                   '--scale', str(scale), '--mute']
            cmd += ['--coop-host', '--coop-capacity', '8', '--coop-level', args.level] if role == 'host' else ['--coop-join', '127.0.0.1']
            metadata['commands'][role] = cmd
            out, err = open(folder/'stdout.log', 'wb'), open(folder/'stderr.log', 'wb')
            streams.extend([out, err])
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            p = subprocess.Popen(cmd, cwd=folder, env=env, stdout=out, stderr=err,
                                 startupinfo=startup, creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            processes.append((role, p))
            if pair and role == 'host':
                time.sleep(2)
        deadline = time.monotonic()+args.duration+20
        while any(p.poll() is None for _, p in processes):
            if (args.output/'STOP').exists():
                raise RuntimeError('Stopped through the run directory STOP marker')
            if time.monotonic() > deadline:
                metadata['timeout'] = True
                raise TimeoutError('Timed out; only processes started by this run will be stopped')
            time.sleep(.2)
    finally:
        for role, p in processes:
            if p.poll() is None:
                p.terminate()
            p.wait(timeout=10)
            metadata['exits'][role] = p.returncode
        for stream in streams:
            stream.close()
        (directory/'run.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    if any(code != 0 for code in metadata['exits'].values()):
        raise RuntimeError(f'Game exited unsuccessfully: {metadata["exits"]}')
    reports = []
    for role in roles:
        log = (directory/role/'stdout.log').read_text(encoding='utf-8', errors='replace')
        if args.profile_ui_gl and '[renderer-profile] UI GL call sampling enabled' not in log:
            raise RuntimeError(f'{role}: UI GL sampling was not enabled')
        disabled = '[renderer-profile] interpolation disabled; native frames only' in log
        if disabled != (interpolation == 'off'):
            raise RuntimeError(f'{role}: executable did not confirm the requested interpolation mode')
        if predicted_upload != 'default' and f'[gpu] predicted upload: {predicted_upload}' not in log:
            raise RuntimeError(f'{role}: executable did not confirm the requested upload mode')
        if reprojection != 'default' and f'[gpu] clip reprojection: {reprojection}' not in log:
            raise RuntimeError(f'{role}: executable did not confirm the requested reprojection mode')
        if position_storage != 'default' and f'[gpu] interpolation storage: {position_storage}' not in log:
            raise RuntimeError(f'{role}: executable did not confirm interpolation storage')
        if frame_batch != 'default' and f'[gpu] frame batch: {frame_batch}' not in log:
            raise RuntimeError(f'{role}: executable did not confirm frame batching')
        verified = log.count('[reprojection-verified]')
        if args.verify_reprojection and role == 'client' and ('[gpu] clip reprojection: four-vertex' not in log or not verified or '[reprojection-verify-failed]' in log):
            raise RuntimeError(f'{role}: no successful batched/scalar verification')
        stats = summarize(read_samples(directory/role/'performance.jsonl'), 22,
                          args.duration-2, args.level, scale, args.fps, len(roles))
        if args.capture_frames and role == 'client':
            captures = list((directory/role/'gpu-captures').glob('frame_*.bin'))
            if len(captures) != args.capture_frames[1]:
                raise RuntimeError('Requested capture sequence was not fully recorded')
            stats['captures'] = [dict(name=p.name, bytes=p.stat().st_size,
                                      sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                                 for p in sorted(captures)]
        if position_storage == 'packed' and role == 'client' and predicted_upload != 'full':
            direct_uploads = sum(root.get('work_per_sample', {}).get('packed_prediction_uploads', 0)
                                 for root in stats.get('detail', {}).values())
            if not direct_uploads:
                raise RuntimeError('Packed mode did not exercise direct client position uploads')
        reports.append({'case': directory.name, 'role': role, 'pair': pair,
                        'interpolation': interpolation, 'predicted_upload': predicted_upload,
                        'reprojection': reprojection, 'position_storage': position_storage, 'frame_batch': frame_batch, 'verified_reprojections': verified, **stats})
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--card', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--level', default='Rlevel1_2')
    parser.add_argument('--port', type=int, default=27451)
    parser.add_argument('--fps', type=int, default=120)
    parser.add_argument('--anisotropy', type=int, choices=[1, 2, 4, 8, 16], default=1)
    parser.add_argument('--renderer', choices=['gpu', 'dx11'], default='gpu')
    parser.add_argument('--shader-cache', type=Path, help='Seed an isolated warm-cache run from this directory')
    parser.add_argument('--input-script', help='Override the isolated scripted input fixture')
    parser.add_argument('--duration', type=int, default=40)
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--profile-ui-gl', action='store_true', help='Sample ImGui GL driver calls; diagnostic overhead')
    parser.add_argument('--exercise-motion', action='store_true',
                        help='Script turning, walking, shooting and weapon input; capture intermediate images. Not a stationary benchmark.')
    parser.add_argument('--exercise-native-motion', action='store_true',
                        help='Script motion/fire and screenshots without disabling packed decode through smoothness tracing')
    parser.add_argument('--verify-reprojection', action='store_true',
                        help='Compare sampled real frame payloads against the scalar reference; diagnostic, not a benchmark')
    parser.add_argument('--interpolation', nargs='+', choices=['on', 'off'], default=['on'],
                        help='Diagnostic A/B only; off removes immediate camera reprojection too')
    parser.add_argument('--predicted-upload', nargs='+', choices=['default', 'full', 'compact'], default=['default'],
                        help='Compare full vertex/index uploads with compact position uploads')
    parser.add_argument('--reprojection', nargs='+', choices=['default', 'scalar', 'four-vertex'], default=['default'],
                        help='Compare scalar with four-vertex clip reprojection; same camera and interpolation')
    parser.add_argument('--frame-batch', nargs='+', choices=['default', 'drain', 'snapshot'], default=['default'],
                        help='Compare consuming arrivals during rendering with a bounded initial queue snapshot')
    parser.add_argument('--position-storage', nargs='+', choices=['default', 'full', 'packed'], default=['default'],
                        help='Compare interleaved prediction buffers with direct packed positions')
    parser.add_argument('--cases', nargs='+', choices=['solo-1x', 'pair-1x', 'pair-3x', 'pair-8x'],
                        default=['solo-1x', 'pair-1x', 'pair-3x'])
    parser.add_argument('--capture-frames', nargs=2, type=int, metavar=('FIRST', 'COUNT'),
                        help='Capture 2..8 consecutive client frames for CPU replay; adds I/O, not an FPS baseline')
    args = parser.parse_args()
    if args.capture_frames and (args.capture_frames[0] < 0 or not 2 <= args.capture_frames[1] <= 8 or
                               len(args.cases) != 1 or args.cases[0] == 'solo-1x' or args.repeats != 1):
        parser.error('Capture needs FIRST >= 0, COUNT 2..8, one paired case and one repetition')
    if args.exercise_motion and args.exercise_native_motion:
        parser.error('Choose one motion exercise mode')
    if 'packed' in args.position_storage and 'off' in args.interpolation:
        parser.error('Packed position comparisons require interpolation enabled')
    if args.verify_reprojection and ('scalar' in args.reprojection or 'off' in args.interpolation):
        parser.error('Reprojection verification requires default/four-vertex mode with interpolation enabled')
    if args.verify_reprojection and 'solo-1x' in args.cases:
        parser.error('Reprojection verification requires a paired client case')
    if os.name != 'nt':
        parser.error('The current multiplayer executable and harness require Windows')
    if not 35 <= args.duration <= 60 or not 1 <= args.repeats <= 3:
        parser.error('Use 35-60 seconds and 1-3 repetitions')
    if (args.fps != 0 and not 30 <= args.fps <= 120) or not 1024 <= args.port <= 65535:
        parser.error('Use unlimited (0) or a frame cap of 30-120 and an unprivileged port')
    for key in ('exe', 'data', 'card'):
        setattr(args, key, getattr(args, key).resolve(strict=True))
    if (args.data/'sys/boot.bin').read_bytes()[:4] != b'G3BP':
        parser.error('Requires PAL GameCube data')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'executable_sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest(),
              'source_card_sha256': hashlib.sha256(args.card.read_bytes()).hexdigest(),
              'motion_exercise': args.exercise_motion or args.exercise_native_motion, 'motion_trace': args.exercise_motion,
              'verify_reprojection': args.verify_reprojection, 'profile_ui_gl': args.profile_ui_gl,
              'capture_frames': args.capture_frames,
              'renderer': 'Direct3D 11' if args.renderer == 'dx11' else 'OpenGL', 'reports': [], 'complete': False,
              'limits': [f'Only {args.renderer} measured in this invocation; no simultaneous backend comparison', 'Host and client share one PC',
                         ('Scripted motion/fire with extra tracing; not a stationary benchmark' if args.exercise_motion else
                          'Scripted native motion/fire without smoothness tracing; not a stationary benchmark' if args.exercise_native_motion else
                          'Stationary camera, god mode, muted audio, two-player fixture only'),
                         'No deterministic replay; compare entity counts and repeated runs',
                         'Stage times overlap; never sum nested totals',
                         'GPU time asynchronous, per timed execute, excludes UI; detail CPU sampled 1 in 16',
                         'Network stages measure main-thread capture/publish/apply, not all worker CPU',
                         'No claim of multiplayer campaign correctness']}
    if args.verify_reprojection:
        result['limits'].append('Additional scalar reference work enabled; not a performance baseline')
    if args.capture_frames:
        result['limits'].append('Synchronous diagnostic capture I/O enabled; not a performance baseline')
    try:
        for repeat in range(1, args.repeats+1):
            cases = [('solo-1x', False, 1), ('pair-1x', True, 1), ('pair-3x', True, 3), ('pair-8x', True, 8)]
            cases = [case for case in cases if case[0] in args.cases]
            if repeat % 2 == 0:
                cases.reverse()
            for name, pair, scale in cases:
                modes = args.interpolation if repeat % 2 else list(reversed(args.interpolation))
                for mode in modes:
                    uploads = args.predicted_upload if repeat % 2 else list(reversed(args.predicted_upload))
                    projections = args.reprojection if repeat % 2 else list(reversed(args.reprojection))
                    storage_modes = args.position_storage if repeat % 2 else list(reversed(args.position_storage))
                    batch_modes = args.frame_batch if repeat % 2 else list(reversed(args.frame_batch))
                    for upload, reprojection, position_storage, frame_batch in product(uploads, projections, storage_modes, batch_modes):
                        suffix = '' if upload == 'default' else f'-{upload}'
                        if reprojection != 'default':
                            suffix += f'-{reprojection}'
                        if position_storage != 'default':
                            suffix += f'-storage-{position_storage}'
                        if frame_batch != 'default':
                            suffix += f'-batch-{frame_batch}'
                        folder = args.output/f'{name}-{mode}{suffix}-r{repeat}'
                        print(f'Running {folder.name} ({args.duration}s)', flush=True)
                        reports = run_case(args, folder, pair, scale, mode, upload, reprojection, position_storage, frame_batch)
                        result['reports'].extend(reports)
                        for r in reports:
                            print(f'  {r["role"]}: native {r["native_fps"]["median"]:.1f}; presented {r["render_fps"]["median"]:.1f} fps', flush=True)
                        (args.output/'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        result['complete'] = True
    finally:
        (args.output/'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
