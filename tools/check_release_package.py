"""Audit an integrated launcher ZIP before upload; never extracts or executes it."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import re
import zipfile
from audit_source import inspect

ROOT_FILES={"SSNE-Native.exe","gctool.exe","known_dumps.txt","LICENSE.txt",
            "THIRD-PARTY-NOTICES.txt","README.txt","launcher-integration.json","package-files.json"}
FORBIDDEN_PARTS={"game_data","generated","generated-multiplayer","recompiled","saves","states",
                 "work","native-multiplayer-work","toolchain","downloads",".git","__pycache__"}
FORBIDDEN_EXT={".iso",".gcm",".rvz",".wia",".gcz",".ciso",".dol",".raw",".ram",".state",".coop",".bik"}
TOOLS={"SSNE-Native.exe","gctool.exe","build-kit/ssne-recomp.exe"}

def need(value,message):
    if not value:raise ValueError(message)

def safe_path(name):
    p=PurePosixPath(name)
    need(name and not p.is_absolute() and ".." not in p.parts and ":" not in name and "\\" not in name,"Unsafe ZIP path: "+name)
    need(not any(part.lower() in FORBIDDEN_PARTS for part in p.parts),"Private/game directory: "+name)
    need(p.suffix.lower() not in FORBIDDEN_EXT,"Disc/save/generated artifact: "+name)
    need(name in ROOT_FILES or p.parts[0] in {"source","licenses","build-kit"},"Unexpected release root: "+name)
    if p.suffix.lower()==".exe":need(name in TOOLS,"Built game or unknown executable: "+name)
    if p.suffix.lower() in {".a",".obj",".o",".lib"}:need(p.parts[0]=="build-kit","Object outside runtime kit: "+name)
    return p

def audit(path):
    with zipfile.ZipFile(path) as archive:
        info=[entry for entry in archive.infolist() if not entry.is_dir()]
        need(1<=len(info)<=2048,"Unexpected archive file count")
        need(sum(entry.file_size for entry in info)<=128*1024*1024,"Archive exceeds launcher package size limit")
        names={};contents={}
        for entry in info:
            safe_path(entry.filename)
            need(entry.filename.lower() not in names,"Duplicate/case-colliding ZIP path")
            need(not entry.flag_bits&1 and (entry.external_attr>>16)&0o170000!=0o120000,"Encrypted or linked ZIP entry")
            need(entry.file_size<=32*1024*1024,"Oversized release entry: "+entry.filename)
            names[entry.filename.lower()]=entry.filename;contents[entry.filename]=archive.read(entry)
    need(ROOT_FILES<=contents.keys(),"Required launcher/release files are missing")
    def digest(name):return hashlib.sha256(contents[name]).hexdigest()
    manifest=json.loads(contents["package-files.json"])
    need(manifest.get("schema")==1,"Unsupported package manifest")
    listed={}
    for item in manifest["files"]:
        name=item["path"];safe_path(name);need(name not in listed,"Duplicate manifest path")
        need(name in contents and digest(name)==item["sha256"],"Package checksum differs: "+name);listed[name]=item["sha256"]
    need(set(listed)==set(contents)-{"package-files.json"},"ZIP and manifest file sets differ")
    source_count=0
    for name,data in contents.items():
        if name.startswith("source/"):
            issues=inspect(name[7:],data);need(not issues,"Source audit failed: "+name+"; "+", ".join(issues));source_count+=1
        if name in TOOLS or PurePosixPath(name).suffix.lower() in {".a",".obj",".o",".lib"}:
            need(re.search(rb"f_80[0-9A-Fa-f]{6}",data) is None,"Generated game function symbol in shipped binary: "+name)
    need(source_count>0,"Matching source is absent")
    base="build-kit/multiplayer/";descriptor=json.loads(contents[base+"runtime-kit.json"])
    side=json.loads(contents[base+"launcher-kit.json"]);recipe=json.loads(contents[base+"translation-recipe.json"])
    marker=json.loads(contents["launcher-integration.json"])
    need(side["runtime_kit_sha256"]==recipe["runtime_kit_sha256"]==digest(base+"runtime-kit.json"),"Runtime/recipe binding differs")
    need(side["patch_recipe"]==base+"translation-recipe.json" and side["patch_recipe_sha256"]==digest(base+"translation-recipe.json"),"Recipe checksum differs")
    need(side["symbols_sha256"]==digest(side["symbols"]),"PAL symbol checksum differs")
    need(descriptor["recompiler_sha256"]==digest("build-kit/ssne-recomp.exe"),"Recompiler checksum differs")
    need(58<=len(descriptor["files"])<=128 and descriptor["protocol"]==63,"Unexpected native kit scope")
    for item in descriptor["files"]:need(digest(base+item["path"])==item["sha256"],"Native runtime checksum differs: "+item["path"])
    need(marker["launcher_sha256"]==digest("SSNE-Native.exe") and marker["launcher_kit_sha256"]==digest(base+"launcher-kit.json"),"Launcher binding differs")
    return {"passed":True,"files":len(contents),"source_files":source_count,"runtime_build":descriptor["build"],
            "protocol":descriptor["protocol"],"launcher_sha256":digest("SSNE-Native.exe"),
            "contains_disc_data":False,"contains_built_game":False}

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("archive",type=Path);parser.add_argument("--report",type=Path);args=parser.parse_args()
    report=audit(args.archive)
    if args.report:args.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf8")
    print(json.dumps(report,indent=2))
