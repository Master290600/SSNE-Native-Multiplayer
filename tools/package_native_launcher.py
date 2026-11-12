"""Assemble the normal Windows launcher plus the reviewed native multiplayer kit.
Developer packaging only. Never copies discs, extracted data, generated game
sources/objects/executables, user settings, saves or private build caches.
"""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('base','launcher','recipe','sidecar','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--integration-build',type=int,default=198)
    args=parser.parse_args()
    source=Path(__file__).resolve().parents[1]
    base=args.base.resolve(strict=True);kit=base/'build-kit/multiplayer'
    descriptor_path=kit/'runtime-kit.json';descriptor=json.loads(descriptor_path.read_bytes())
    side=json.loads(args.sidecar.read_bytes());recipe=json.loads(args.recipe.read_bytes())
    if side['runtime_kit_sha256']!=sha(descriptor_path) or recipe['runtime_kit_sha256']!=sha(descriptor_path):
        raise ValueError('Recipe/sidecar and reviewed runtime descriptor differ')
    if side['patch_recipe']!='build-kit/multiplayer/translation-recipe.json' or side['patch_recipe_sha256']!=sha(args.recipe):
        raise ValueError('Unexpected recipe path or checksum')
    if side['symbols']!='build-kit/G3BP9G_r0.sym' or side['symbols_sha256']!=sha(base/side['symbols']):
        raise ValueError('PAL symbols differ')
    if sha(base/'build-kit/ssne-recomp.exe')!=descriptor['recompiler_sha256']:
        raise ValueError('Pinned recompiler differs')
    if sha(source/'tools/patch_multiplayer.py')!=descriptor['patcher_sha256']:
        raise ValueError('Source patcher differs from reviewed runtime')
    files=descriptor['files']
    if not 58<=len(files)<=128 or descriptor['protocol']!=63 or descriptor['build']<197:
        raise ValueError('Unexpected native runtime scope')
    seen=set()
    for item in files:
        relative=Path(item['path'])
        if relative.is_absolute() or ':' in str(relative) or '..' in relative.parts or relative.parts[0] not in ('include','lib'):
            raise ValueError('Unsafe runtime payload path')
        name=relative.as_posix().lower()
        if name in seen:raise ValueError('Duplicate runtime payload path')
        seen.add(name)
        if sha(kit/relative)!=item['sha256']:raise ValueError('Runtime payload differs: '+name)
    output=args.output.resolve()
    if output.exists():raise FileExistsError('Use a new package directory: '+str(output))
    output.mkdir(parents=True)
    def copy(origin,relative):
        target=output/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(origin,target)
    copy(args.launcher,'SSNE-Native.exe')
    for name in ('gctool.exe','known_dumps.txt','LICENSE.txt','THIRD-PARTY-NOTICES.txt'):
        copy(base/name,name)
    for name in ('kit.ini','ssne-recomp.exe','G3BE9G_r1.sym','G3BP9G_r0.sym'):
        copy(base/'build-kit'/name,Path('build-kit')/name)
    for name in ('include','lib'):
        shutil.copytree(base/'build-kit'/name,output/'build-kit'/name)
    copy(descriptor_path,'build-kit/multiplayer/runtime-kit.json')
    for item in files:copy(kit/item['path'],Path('build-kit/multiplayer')/item['path'])
    copy(args.recipe,'build-kit/multiplayer/translation-recipe.json')
    copy(args.sidecar,'build-kit/multiplayer/launcher-kit.json')
    shutil.copytree(base/'licenses',output/'licenses')
    paths=subprocess.check_output(['git','-c','core.quotepath=false','ls-files','--cached','--others','--exclude-standard','-z'],cwd=source).decode('utf8').split('\0')
    for name in sorted(set(paths)-{''}):
        path=source/name
        if path.is_symlink() or not path.is_file():raise ValueError('Missing/nonregular source: '+name)
        if any(part in ('game_data','generated','generated-multiplayer','work','build') for part in Path(name).parts):
            raise ValueError('Private generated data in source list: '+name)
        copy(path,Path('source')/name)
    copy(source/'data/release/NATIVE-MULTIPLAYER.txt','README.txt')
    marker={'schema':1,'target':'native-multiplayer','integration_build':args.integration_build,
        'runtime_build':descriptor['build'],'protocol':descriptor['protocol'],
        'launcher_sha256':sha(output/'SSNE-Native.exe'),
        'launcher_kit_sha256':sha(output/'build-kit/multiplayer/launcher-kit.json'),
        'runtime_descriptor_sha256':sha(output/'build-kit/multiplayer/runtime-kit.json')}
    (output/'launcher-integration.json').write_text(json.dumps(marker,indent=2)+'\n',encoding='utf8')
    if (output/'game_data').exists() or (output/'Multiplayer').exists() or (output/'build-kit/toolchain').exists():
        raise ValueError('Unexpected local game/build output in public package')
    payload=[{'path':p.relative_to(output).as_posix(),'sha256':sha(p)} for p in sorted(output.rglob('*')) if p.is_file()]
    (output/'package-files.json').write_text(json.dumps({'schema':1,'files':payload},indent=2)+'\n',encoding='utf8')
    print(json.dumps({'output':str(output),'files':len(payload)+1,'launcher_sha256':marker['launcher_sha256'],'contains_game_data':False,'contains_built_game':False}))

if __name__=='__main__':main()
