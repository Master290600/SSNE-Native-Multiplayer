"""Recompile the owned PAL disc against the installed 1.2.7 multiplayer runtime."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from datetime import datetime, timezone


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--verify-only', action='store_true')
    ap.add_argument('--work', type=Path)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--output', type=Path, help='Write a separate executable instead of installing it')
    args = ap.parse_args()
    kit = Path(__file__).resolve().parent
    project = kit.parents[1]
    descriptor = json.loads((kit / 'runtime-kit.json').read_text())
    for field,path in (
        ('patcher_sha256',project/'source-multiplayer/tools/patch_multiplayer.py'),
        ('recompiler_sha256',project/'build-kit/ssne-recomp.exe'),
        ('compiler_sha256',project/'build-kit/toolchain/bin/clang++.exe')):
        if descriptor.get(field) and sha(path)!=descriptor[field]:
            raise RuntimeError(f'Build tool differs from this runtime kit: {path}')
    dol = project / 'game_data/sys/main.dol'
    if (project / 'game_data/sys/boot.bin').read_bytes()[:6] != b'G3BP9G' or sha(dol) != descriptor['dol_sha256']:
        raise RuntimeError('This multiplayer runtime requires the supported original European G3BP9G disc.')
    for item in descriptor['files']:
        path = kit / item['path']
        if not path.is_file() or sha(path) != item['sha256']:
            raise RuntimeError(f'Runtime kit differs: {path}')
    print('PAL disc and complete multiplayer runtime kit verified.', flush=True)
    if args.verify_only:
        return
    work = (args.work or kit / 'work').resolve()
    work.mkdir(parents=True, exist_ok=True)
    compiler = project / 'build-kit/toolchain/bin/clang++.exe'
    base, generated, objects = (work / name for name in ('base', 'generated', 'objects'))
    objects.mkdir(exist_ok=True)
    subprocess.run([str(project / 'build-kit/ssne-recomp.exe'), str(dol), str(base), '--map',
                    str(project / 'build-kit/G3BP9G_r0.sym'), '--no-comments'], check=True)
    subprocess.run([sys.executable, str(project / 'source-multiplayer/tools/patch_multiplayer.py'),
                    '--input', str(base), '--output', str(generated)], check=True)
    # Include the runtime headers in each cache key: generated code and hooks must share an ABI.
    headers = hashlib.sha256()
    for path in sorted((kit / 'include').rglob('*.h')):
        headers.update(path.relative_to(kit).as_posix().encode())
        headers.update(path.read_bytes())
    common = headers.hexdigest() + sha(compiler) + repr(descriptor['compile_flags'])
    cache_path = work / 'compiled.json'
    old = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    keys = {}

    def compile_one(source):
        obj = objects / (source.stem + '.obj')
        key = hashlib.sha256((common + sha(source)).encode()).hexdigest()
        keys[source.name] = key
        if obj.exists() and old.get(source.name) == key:
            return obj
        command = [str(compiler), *descriptor['compile_flags'], '-I' + str(kit / 'include'),
                   '-I' + str(generated), '-c', str(source), '-o', str(obj)]
        done = subprocess.run(command, capture_output=True, text=True)
        (objects / (source.stem + '.log')).write_text(done.stdout + done.stderr)
        if done.returncode:
            raise RuntimeError(f'Compilation failed: {source.name}\n{done.stderr[-6000:]}')
        print('Compiled ' + source.name, flush=True)
        return obj

    with ThreadPoolExecutor(max_workers=max(1, min(args.jobs, 8))) as pool:
        compiled = list(pool.map(compile_one, sorted(generated.glob('*.cpp'))))
    cache_path.write_text(json.dumps(keys, indent=2))
    pending = work / 'SSNE-Native-Multiplayer-Game.pending.exe'
    link = []
    for token in descriptor['link_args']:
        link.extend(str(p) for p in compiled) if token == '{game_objects}' else link.append(
            str(kit / token[5:]) if token.startswith('file:') else token)
    # Response files avoid Windows' command-line length limit; paths may contain spaces.
    response = work / 'link.rsp'
    response.write_text('\n'.join('"' + x.replace('\\', '/') + '"' for x in link), encoding='utf-8')
    subprocess.run([str(compiler), '@' + str(response), '-o', str(pending)], check=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(pending, args.output)
        print('Rebuilt: ' + str(args.output), flush=True)
        return
    package = project / 'Multiplayer'
    manifest_path = package / 'release-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    destination = package / 'SSNE-Native-Multiplayer-Game.exe'
    previous = destination.with_name('SSNE-Native-Multiplayer-Game.before-rebuild.exe')
    if destination.exists() and not previous.exists():
        shutil.copy2(destination, previous)
    # Stage both native entry points and roll back the first replacement if
    # Windows refuses an executable which is still running.
    targets=[destination]
    if manifest.get('entrypoint')=='native':targets.append(package/'SSNE-Native-Multiplayer.exe')
    staged=[];backups=[];replaced=[]
    try:
        for target in targets:
            new=target.with_suffix('.new.exe');old=target.with_suffix('.rollback.exe')
            shutil.copy2(pending,new)
            if target.exists():shutil.copy2(target,old)
            staged.append(new);backups.append(old)
        for target,new in zip(targets,staged):os.replace(new,target);replaced.append(target)
    except Exception:
        for target,old in zip(targets,backups):
            if target in replaced and old.exists():os.replace(old,target)
        raise
    finally:
        for item in staged+backups:
            if item.exists():item.unlink()
    manifest.update(product_sha256=sha(destination).upper(), entrypoint_sha256=sha(targets[-1]).upper(), rebuilt=datetime.now(timezone.utc).isoformat(),
                    acceptance='Locally rebuilt; repeat gameplay verification before accepting this executable.')
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print('Installed: ' + str(destination), flush=True)


if __name__ == '__main__':
    main()
