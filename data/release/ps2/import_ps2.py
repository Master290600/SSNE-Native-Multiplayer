"""Install the bundled multiplayer content using the local PAL GameCube data."""
from __future__ import annotations
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import zipfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "engine"))
from ps2_content import Container, MAPS, SKINS
from prepare_ps2_pack import encode, extend_database, merge_archives
from prepare_ps2_test_data import files, fst_tree, serialize
from skin_text import extend as add_descriptions
from dvd import next_extents

def require(ok, message):
    if not ok:
        raise ValueError(message)

def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream,"sha256").hexdigest()

def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))

def blob_hash(data):
    return hashlib.sha256(data).hexdigest()

def local(root, relative):
    rel=Path(relative)
    require(rel.parts and not rel.drive and not rel.is_absolute() and ".." not in rel.parts, "Invalid package path")
    dest=root
    for part in rel.parts:
        dest=dest/part
        require(not dest.is_symlink() and not dest.is_junction(), f"Linked destination not supported: {relative}")
    return dest

def replaceable(row, digest):
    return digest in (row["base"], row["sha256"]) or (digest is not None and digest == row.get("previous_sha256"))

def current(root, relative):
    path=local(root,relative)
    require(not path.exists() or path.is_file(),f"Expected a file at {relative}")
    return digest(path) if path.is_file() else None

@contextmanager
def single_import(scratch):
    scratch.mkdir(exist_ok=True)
    marker=local(scratch,"owner.txt")
    lock=local(scratch,"import.lock")
    if not marker.exists():
        require(all(p.name=="import.lock" for p in scratch.iterdir()),"Existing import folder is not owned by this helper")
    with lock.open("a+b") as handle:
        if handle.seek(0,os.SEEK_END)==0: handle.write(b"0");handle.flush()
        handle.seek(0)
        try:
            if os.name=="nt":
                import msvcrt
                msvcrt.locking(handle.fileno(),msvcrt.LK_NBLCK,1)
            else:
                import fcntl
                fcntl.flock(handle,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except OSError as e:
            raise RuntimeError("Another PS2 import is running in this installation.") from e
        if not marker.exists(): marker.write_text("SSNE-PS2-IMPORT-237\n",encoding="ascii")
        require(marker.read_text(encoding="ascii")=="SSNE-PS2-IMPORT-237\n","Foreign import marker")
        yield

def retire_manual_launcher(root, scratch):
    # Only retire the exact helper supplied with earlier builds. Keep a backup
    # outside the launcher UI and leave locally edited or linked files alone.
    old = root / "PS2-Inhalte importieren.cmd"
    known = "36b05c4081d88c11e5de163f191f104d30e5dd39c8000e4532a2589b04347f63"
    if not old.is_file() or old.is_symlink() or old.is_junction():
        return
    if digest(old) != known:
        return
    saved = local(scratch, "previous-manual-launcher.txt")
    if saved.exists() and (not saved.is_file() or digest(saved) != known):
        return
    os.replace(old, saved)

def materialize_map(package, name, donor):
    # The payload ends where the common GameCube resources begin. Rebase their
    # pointers into this world's address space using the player's own donor.
    prefix = "maps/" + name
    geometry = bytearray(package.read(prefix + ".data"))
    offset = len(geometry)
    fields = {field for field, in struct.iter_unpack(">I", package.read(prefix + ".rel"))}
    geometry.extend(donor.image)
    for field in donor.fields:
        struct.pack_into(">I", geometry, offset + field, offset + donor.u(field))
        fields.add(offset + field)
    return encode(geometry, fields)

def generate(recipe, root, package, scratch):
    rows=recipe["rows"]
    originals=local(scratch,"originals"); originals.mkdir(exist_ok=True)
    stage=local(scratch,"stage");stage.mkdir(exist_ok=True)
    for name,row in rows.items():
        if row["base"] is None:continue
        keep=local(originals,name)
        if not keep.exists():
            source=local(root,name)
            require(digest(source)==row["base"],f"Cannot recover original {name}. Restore the original GameCube data before importing.")
            keep.parent.mkdir(parents=True,exist_ok=True)
            # A stopped backup copy must not be mistaken for a complete original on retry.
            temp=keep.with_suffix(keep.suffix+".pending")
            require(not temp.is_symlink() and not temp.is_junction(),"Unsafe backup path")
            shutil.copyfile(source,temp)
            require(digest(temp)==row["base"],"Original changed during backup: "+name)
            os.replace(temp,keep)
        require(digest(keep)==row["base"],"Backup checksum differs: "+name)
    def base(name):
        return local(originals,"game_data/"+name).read_bytes()
    emitted=set()
    def emit(name,data):
        require(name in rows and name not in emitted,"Unexpected generated file "+name)
        require(blob_hash(data)==rows[name]["sha256"],f"Generated file differs: {name}. Use the included Python runtime.")
        dest=local(stage,name);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data);emitted.add(name)
    def game_file(name,data):
        emit("game_data/files/"+name,data)
    skin_names=[]; skin_data=[]
    for skin in SKINS:
        wire = package.read("skins/" + skin + ".ssg")
        name="Multiplayer/skin_"+skin+".ssg"
        game_file(name,wire);skin_names.append(name);skin_data.append(wire)
    all_name="Multiplayer/skin_all.ssg"
    game_file(all_name,merge_archives(base("files/"+all_name),skin_data));skin_names.append(all_name)
    cfg=base("files/base.cfg").decode("latin1")
    opening=cfg.index("{",cfg.index("MultiPlayerInfo"))
    depth=1;end=opening+1
    while depth:
        depth += (cfg[end]=="{")-(cfg[end]=="}");end+=1
    block="".join(f'\n\t\tstash Model{i}\n\t\t{{\n\t\t\tstring Name "{skin}"\n\t\t\tint SoundSet 0 0 1\n\t\t}}\n' for i,skin in enumerate(SKINS,11))
    game_file("base.cfg",(cfg[:end-1]+block+cfg[end-1:]).encode("latin1"))
    langs={"EnglishEUR.tdb":("6D7DC.TDB","Player Character"),"EnglishUSA.tdb":("6D7DC.TDB","Player Character"),
           "German.tdb":("GERMAN.TDB","Spieler-Figur"),"french.tdb":("FRENCH.TDB","Personnage")}
    for name,(source,label) in langs.items():
        db=package.read("strings/" + source)
        data,_,_=add_descriptions(extend_database(base("files/"+name),db),db,label)
        game_file(name,data)
    members=sorted(skin_names+list(langs)+["base.cfg"],key=str.casefold)
    skin_manifest="SSNE-PS2-SKINS-1\n"+"".join(rows["game_data/files/"+n]["sha256"]+" "+n+"\n" for n in members)
    game_file("ps2-skin-content.txt",skin_manifest.encode("ascii"))
    emit("Multiplayer/online-icon.png", package.read("online.png"))
    donor=Container(local(root,"game_data/files/Levels/RlevelDM_2.ssw").read_bytes(),">")
    renames={}
    for idx,name in enumerate(MAPS,1):
        print(f"[{idx}/{len(MAPS)}] {name}",flush=True)
        wire = materialize_map(package, name, donor)
        metadata = package.read("maps/" + name + ".json")
        title = json.loads(metadata)["display_name"]
        stem=title.translate(str.maketrans({"ä":"ae","ö":"oe","ü":"ue","Ä":"Ae","Ö":"Oe","Ü":"Ue","ß":"ss"})).replace(" ","_")
        level="Levels/"+stem+".ssw"
        renames["Levels/PS2_"+name+".ssw"]=level
        prefix="CustomLevels/ps2_"+name+"/"
        game_file(level,wire);emit(prefix+"map.ssw",wire)
        emit(prefix+"level.json",metadata)
        emit(prefix+"preview.png",package.read("maps/" + name + ".png"))
    fst,boot=base("sys/fst.bin"),base("sys/boot.bin")
    for original_sizes in recipe["old_menu_extents"]:
        fst,boot=next_extents(fst,boot,original_sizes)
    tree=fst_tree(fst);entries=dict(files(tree))
    for old,new in renames.items():entries[old]["name"]=Path(new).name
    fst=serialize(tree);header=bytearray(boot)
    struct.pack_into(">2I",header,0x428,len(fst),len(fst))
    fst,boot=next_extents(fst,header,{n:local(stage,"game_data/files/"+n).stat().st_size for n in skin_names})
    emit("game_data/sys/fst.bin",fst);emit("game_data/sys/boot.bin",boot)
    require(emitted==set(rows),"Incomplete staged add-on")
    return stage

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True, help="Windows PAL installation")
    root = parser.parse_args().game.resolve(strict=True)
    recipe=read_json(HERE/"checksums.json"); rows=recipe["rows"]
    require(current(root,"build-kit/multiplayer/runtime-kit.json")==recipe["kit_sha256"],"The matching Windows multiplayer build kit is required.")
    require(current(root,"game_data/sys/main.dol")==recipe["dol_sha256"] and current(root,"game_data/files/Levels/RlevelDM_2.ssw")==recipe["donor_sha256"],"Install your supported original GameCube PAL disc first.")
    bundle=local(HERE,"ps2-content.zip")
    require(bundle.is_file() and digest(bundle)==recipe["content_sha256"],"The multiplayer content package is missing or damaged. Extract the complete Windows release and retry Build Game.")
    scratch=local(root,".ssne-ps2-import-237")
    with single_import(scratch):
        changed=[]
        for name,row in rows.items():
            actual=current(root,name)
            require(replaceable(row, actual),"Existing content differs; no files were changed: "+name)
            if actual!=row["sha256"]:changed.append(name)
        if not changed:
            retire_manual_launcher(root, scratch)
            print("Already installed. All 76 files verified."); return
        stage=local(scratch,"stage")
        if not stage.is_dir() or not all(current(stage,n)==rows[n]["sha256"] for n in changed):
            require(shutil.disk_usage(root).free>=650*1024**2,"At least 650 MiB free space is required.")
            with zipfile.ZipFile(bundle) as package:
                stage=generate(recipe,root,package,scratch)
        for name in changed:
            require(current(stage,name)==rows[name]["sha256"],"Incomplete conversion: "+name)
        # An interrupted commit can safely continue by recognizing completed entries.
        # Keep the DVD headers last so they only describe files which already exist.
        for name in sorted(changed,key=lambda n:(n.startswith("game_data/sys/"),n.endswith("boot.bin"),n)):
            dest=local(root,name)
            actual=current(root,name)
            if actual==rows[name]["sha256"]:continue
            require(replaceable(rows[name], actual),"Destination changed during import: "+name)
            dest.parent.mkdir(parents=True,exist_ok=True)
            os.replace(local(stage,name),dest)
        require(all(current(root,n)==row["sha256"] for n,row in rows.items()),"Final verification failed; run the importer to resume.")
        retire_manual_launcher(root, scratch)
        print("Imported and verified 15 official PS2 maps, their previews, six skins and the Online menu icon.")

if __name__=="__main__":
    try:
        main()
    except (OSError,ValueError,RuntimeError,KeyError,ImportError,struct.error,zipfile.BadZipFile) as exc:
        print("PS2 import: "+str(exc),file=sys.stderr)
        sys.exit(1)

