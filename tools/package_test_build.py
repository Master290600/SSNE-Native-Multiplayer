"""Prepare a separate local test package. Never bundle or upload disc data."""
import argparse
import hashlib
import json
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--project', type=Path, required=True,
                        help='Local directory containing game_data from your own PAL disc')
    parser.add_argument('--skins', type=Path, help='Optional private skin preview asset')
    parser.add_argument('--build', type=int, required=True)
    parser.add_argument('--protocol', type=int, required=True)
    parser.add_argument('--renderer', choices=['gpu', 'dx11'], default='gpu',
                        help='Default renderer in the new isolated package')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    project = args.project.resolve(strict=True)
    data = project / 'game_data'
    if (data / 'sys/boot.bin').read_bytes()[:4] != b'G3BP':
        parser.error('Requires European GameCube game data')
    if not (data / 'sys/main.dol').is_file():
        parser.error('main.dol is missing')
    if args.build < 116 or args.protocol < 1:
        parser.error('Invalid build/protocol')
    files = [args.bin_dir / 'SSNE-RECOOP-Game.exe',
             args.bin_dir / 'SSNE-RECOOP-Multiplayer.exe',
             root / 'launcher/multiplayer/Start-Multiplayer-GUI.ps1']
    if args.skins:
        files.append(args.skins)
    for source in files:
        if not source.is_file():
            parser.error(f'Missing file: {source}')
    # Require a new folder: no saves, settings or previous package get overwritten.
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    for source in files:
        shutil.copy2(source, output / ('skins.bin' if source == args.skins else source.name))
    shutil.copy2(root / 'THIRD-PARTY-NOTICES.txt', output / 'THIRD-PARTY-NOTICES.txt')
    shutil.copytree(root / 'licenses', output / 'licenses')
    (output / 'settings-netplay.ini').write_text(
        f'[graphics]\nrenderer = {args.renderer}\nscale = 1\naspect = 1\nfps_limit = 60\n'
        'vsync = 0\nwindow_w = 960\nwindow_h = 540\nwindow_maximized = 0\nfullscreen = 0\n', encoding='utf-8')
    sha = hashlib.sha256((output / files[0].name).read_bytes()).hexdigest().upper()
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True).strip())
    manifest = dict(build=args.build, protocol=args.protocol, product_sha256=sha,
                    destination=files[0].name, source=str(project / 'local-test-build'),
                    source_revision=revision, source_dirty=dirty,
                    created=datetime.now(timezone.utc).isoformat(), experimental=True,
                    renderer=args.renderer,
                    acceptance='Component tests only; multiplayer campaign acceptance pending')
    (output / 'release-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    (output / 'multiplayer-launcher.ini').write_text(
        f'player_name=Sam\nplayer_skin=0\naddress=127.0.0.1\nproject={project}\n'
        'port=27341\nfullscreen=0\nremember=1\n', encoding='utf-8')
    (output / 'Start-Multiplayer.cmd').write_bytes(
        b'@echo off\r\ncd /d "%~dp0"\r\nstart "" "%~dp0SSNE-RECOOP-Multiplayer.exe"\r\n')
    (output / 'TEST-HINWEISE.txt').write_text(
        'SSNE-RECOOP: experimenteller lokaler Testbuild\n\n'
        'Start-Multiplayer.cmd oeffnet den sichtbaren Launcher. Host oder Beitreten waehlen.\n'
        'Fuer den Selbsttest den Launcher erneut starten und 127.0.0.1 sowie die Host-PIN verwenden.\n'
        'Auf beiden PCs genau diesen Build benutzen. Unter Spieldaten jeweils den eigenen\n'
        'Spielordner auswaehlen. Das Paket enthaelt keine Spieldaten oder ISO.\n'
        'Spielstaende und Einstellungen werden getrennt im Testordner angelegt.\n\n'
        f'Renderer: {"Direct3D 11" if args.renderer == "dx11" else "OpenGL"}. Unter F12 > Graphics > Renderer umschaltbar; danach neu starten.\n'
        'Beim ersten Auftreten eines Materials werden Shader kompiliert und lokal zwischengespeichert.\n'
        'Renderer-Tests sind keine Abnahme der gesamten Multiplayer-Kampagne.\n'
        'Der bisherige Multiplayer-Build bleibt erhalten.\n', encoding='utf-8')
    print(json.dumps(dict(output=str(output), sha256=sha, **manifest), indent=2))


if __name__ == '__main__':
    main()
