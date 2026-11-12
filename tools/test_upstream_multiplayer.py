"""Run two isolated hidden peers; never send desktop input or touch user cards."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    ap.add_argument('--card', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--level', default='Rlevel1_1')
    ap.add_argument('--duration', type=int, default=55)
    ap.add_argument('--scale', type=int, default=1)
    ap.add_argument('--fps', type=int, default=60)
    ap.add_argument('--port', type=int, default=27681)
    ap.add_argument('--input-script')
    ap.add_argument('--skip-delay-ms', type=int, default=1500)
    ap.add_argument('--mod-flags', type=int, default=1)
    ap.add_argument('--pickup-type', type=int)
    ap.add_argument('--players', type=int, choices=range(2,9), default=2)
    ap.add_argument('--hud-edges', type=int, choices=(0,1,2), default=1, help='2: host on, clients off')
    ap.add_argument('--fov', type=int, default=63)
    ap.add_argument('--require-save-load', action='store_true')
    args = ap.parse_args()
    for name in ("exe", "data", "card", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    processes, streams = [], []
    roles=['host','client']+[f'client{n}' for n in range(2,args.players)]
    result = dict(level=args.level, scale=args.scale, fps=args.fps,
                  exe_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                  cards_before=hashlib.sha256(args.card.read_bytes()).hexdigest(),
                  exits={}, commands={}, visible_game_windows=[], foreground_game_windows=[])
    user32 = ctypes.windll.user32
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    user32.GetWindowThreadProcessId.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong))
    user32.IsWindowVisible.argtypes = (ctypes.c_void_p,)
    user32.EnumWindows.argtypes = (callback_type, ctypes.c_void_p)
    try:
        for role in roles:
            folder = args.output / role
            folder.mkdir()
            exe = folder / args.exe.name
            shutil.copy2(args.exe, exe)
            shutil.copy2(args.card, folder / 'test-card.raw')
            (folder / 'settings-netplay.ini').write_text(
                f'[graphics]\nrenderer = gpu\nscale = {args.scale}\naspect = 1\n'
                f'fps_limit = {args.fps}\nvsync = 0\nsync_to_display = 0\n'
                f'gx_thread = 1\ngpu_vertex = 1\nhud_edges = {int(args.hud_edges==1 or (args.hud_edges==2 and role=="host"))}\nwindow_w = 960\nwindow_h = 540\n'
                'window_maximized = 0\nfullscreen = 0\n[audio]\nmuted = 1\n'
                '[general]\npause_unfocused = 0\n', encoding='utf-8')
            env = {k: v for k, v in os.environ.items() if not k.startswith('SSNE_')}
            env.update(SSNE_TEST_HIDDEN='1', SSNE_SCREENSHOT=str(folder / 'game.bmp'),
                       SSNE_SCREENSHOT_MS=str((args.duration + (4 if role == 'host' else 0)) * 1000),
                       SSNE_PERF_LOG=str(folder / 'performance.jsonl'),
                       SSNE_TEST_SCREENSHOT_DIR=str(folder / 'steps'))
            env['SSNE_COOP_TEST_SKIP_DELAY_MS'] = str(args.skip_delay_ms)
            env['SSNE_TEST_SKIP_FIRST_VOTE'] = '1'
            env['SSNE_PAD_SCRIPT'] = (
                'B@14:0.25,B@18:0.25,SHOT@23:0.2,CRIGHT@26:0.2,'
                'SUP@28:0.7,FIRE@30:0.8,SHOT@32:0.2,X@34:0.1,'
                'FIRE@37:0.7,SHOT@40:0.2,CLEFT@43:0.2')
            if args.input_script:
                env['SSNE_PAD_SCRIPT'] = args.input_script
            if role == 'host':
                env['SSNE_NETPLAY_MOD_FLAGS'] = str(args.mod_flags)
                if args.pickup_type is not None:
                    env.update(SSNE_COOP_TEST_PICKUP=str(args.pickup_type),SSNE_COOP_TEST_PICKUP_AT_PLAYERS='1',SSNE_COOP_TEST_PICKUP_MASK='3')
            cmd = [str(exe), '--data', str(args.data), '--memcard', str(folder / 'test-card.raw'),
                   '--lan-port', str(args.port), '--lan-pin', '123456', '--player-name', role,
                   '--scale', str(args.scale), '--fps', str(args.fps), '--fov',str(args.fov),'--mute']
            cmd += ['--coop-host', '--coop-capacity', '8', '--coop-level', args.level] if role == 'host' else ['--coop-join', '127.0.0.1']
            result['commands'][role] = cmd
            out, err = open(folder / 'stdout.log', 'wb'), open(folder / 'stderr.log', 'wb')
            streams.extend((out, err))
            si = subprocess.STARTUPINFO()
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            si.wShowWindow = 0
            p = subprocess.Popen(cmd, cwd=folder, env=env, stdout=out, stderr=err,
                                 startupinfo=si, creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            processes.append((role, p))
            print(f'{role} pid={p.pid}', flush=True)
            if role == 'host':
                time.sleep(2)
        deadline = time.monotonic() + args.duration + 25
        while any(p.poll() is None for _, p in processes):
            peers = {p.pid for _, p in processes}
            foreground = user32.GetForegroundWindow()

            @callback_type
            def inspect(hwnd, unused):
                pid = ctypes.c_ulong()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value in peers:
                    if user32.IsWindowVisible(hwnd):
                        result['visible_game_windows'].append(int(hwnd))
                    if hwnd == foreground:
                        result['foreground_game_windows'].append(int(hwnd))
                return True

            user32.EnumWindows(inspect, 0)
            if time.monotonic() > deadline:
                raise TimeoutError('Bounded peer test timed out')
            time.sleep(.2)
    finally:
        for role, p in processes:
            if p.poll() is None:
                p.terminate()
            p.wait(timeout=15)
            result['exits'][role] = p.returncode
        for stream in streams:
            stream.close()
        result['cards_after'] = hashlib.sha256(args.card.read_bytes()).hexdigest()
        for role, _ in processes:
            path = args.output / role / 'performance.jsonl'
            samples = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
            valid = [r for r in samples if r.get('type') == 'sample' and r.get('scene') == 4
                     and (r.get('level') == args.level or args.level == 'lobby')
                     and r.get('local_ready') and r.get('ready_players') == args.players
                     and 0 < r.get('native_fps', 0) <= args.fps * 1.5]
            result[role] = dict(gameplay_samples=len(valid),
                                max_entities=max((r.get('entities', 0) for r in valid), default=0),
                                native_fps=[r.get('native_fps') for r in valid],
                                screenshot=(args.output / role / 'game.bmp').exists())
            log=(args.output/role/('ssne-coop-host.log' if role=='host' else 'ssne-coop-client.log')).read_text(encoding='utf-8',errors='replace')
            result[role]['fatal_messages']=[line for line in log.splitlines() if '[port] Stopped:' in line or 'Fatal renderer' in line]
            result[role]['save_load_ok']=('[coop-save-result] save=1 slot=1 ok=1' in log and '[coop-save-result] save=0 slot=1 ok=1' in log) if role=='host' else True
        result['passed'] = (result['exits'] == {role:0 for role in roles}
                            and result['cards_before'] == result['cards_after']
                            and not result['visible_game_windows'] and not result['foreground_game_windows']
                            and all(result.get(role, {}).get('gameplay_samples', 0) >= 8 and not result[role]['fatal_messages'] for role in roles)
                            and (not args.require_save_load or result['host']['save_load_ok']))
        (args.output / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(json.dumps(result), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
