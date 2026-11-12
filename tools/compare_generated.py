"""Compare private translations without writing any game code to the report."""
import argparse
import hashlib
import json
from pathlib import Path

def normalized(path):
    return path.read_text(encoding='utf-8-sig').replace('\r\n','\n').encode()

def compare(left,right):
    names={p.name for folder in (left,right) for p in folder.iterdir() if p.suffix in ('.cpp','.h')}
    differences=[]
    for name in sorted(names):
        a,b=left/name,right/name
        hashes=[hashlib.sha256(normalized(p)).hexdigest() if p.exists() else None for p in (a,b)]
        if hashes[0]!=hashes[1]:differences.append({'file':name,'reference_sha256':hashes[0],'candidate_sha256':hashes[1]})
    return {'compared_files':len(names),'matching_files':len(names)-len(differences),
            'equivalent_source':bool(names) and not differences,'differences':differences,
            'scope':'UTF-8 source bytes with normalized line endings; not a gameplay or binary equivalence test.'}

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--reference',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    result=compare(args.reference,args.candidate)
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='differences'},indent=2))
    raise SystemExit(0 if result['equivalent_source'] else 1)
