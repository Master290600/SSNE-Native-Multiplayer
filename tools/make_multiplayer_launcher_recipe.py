"""Developer-only sparse PAL patch recipe; never package translated game bodies.
Input and reference are private local ssne-recomp/Python-patcher outputs. The
published recipe contains only original-function hashes and sparse replacement
edits. No generated source, object, executable or raw memory is copied.
"""
from pathlib import Path
import argparse,hashlib,json,re

PREFIX='#include "ssne/native_party.h"\n#include "ssne/replica_simulation.h"\n#include "ssne/native_results.h"\n#include <algorithm>\n'
def sha(b):return hashlib.sha256(b).hexdigest()
def read_lf(path):
 raw=path.read_bytes()
 if not raw.isascii() or b'\0' in raw:raise ValueError('Expected ASCII generated source: '+path.name)
 text=raw.replace(b'\r\n',b'\n')
 if b'\r' in text:raise ValueError('Unexpected standalone CR: '+path.name)
 return raw,text.decode('ascii')
def project_path(path,project):
 value=path.resolve().relative_to(project.resolve()).as_posix()
 if any(p in ('','.','..') for p in value.split('/')):raise ValueError('Unsafe sidecar path')
 return value

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 for name in ('input','reference','runtime-descriptor','symbols','patcher','output'):ap.add_argument('--'+name,type=Path,required=True)
 ap.add_argument('--sidecar-output',type=Path);ap.add_argument('--project',type=Path)
 a=ap.parse_args();base=a.input.resolve();reference=a.reference.resolve();output=a.output.resolve()
 if base==reference or base in output.parents or reference in output.parents:raise ValueError('Keep the recipe outside private generated trees')
 edits=json.loads((reference/'patch-edits.json').read_text(encoding='utf-8'))
 grouped={}
 for item in edits:
  if set(item)!={'file','address','before','after','edits'}:raise ValueError('Unexpected sparse edit schema')
  name=item['file']
  if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.]*\.cpp',name):raise ValueError('Unsafe unit path')
  if not re.fullmatch('[0-9A-F]{8}',item['address']):raise ValueError('Bad function address')
  grouped.setdefault(name,[]).append(item)
 before_names={p.name for p in base.iterdir() if p.suffix in ('.cpp','.h')};after_names={p.name for p in reference.iterdir() if p.suffix in ('.cpp','.h')}
 if before_names!=after_names:raise ValueError('Reference/generated file sets differ')
 files=[];newlines=set();functions=0;edit_count=0
 for name in sorted(before_names):
  old_raw,old=read_lf(base/name);new_raw,new=read_lf(reference/name);patched=old;records=[]
  for item in grouped.pop(name,[]):
   marker='\nvoid f_'+item['address'];start=patched.find(marker)
   if start<0 or patched.find(marker,start+1)>=0:raise ValueError('Missing/ambiguous function '+item['address'])
   end=patched.find('\nvoid f_',start+1);end=len(patched) if end<0 else end;body=patched[start:end]
   if sha(body.encode())!=item['before']:raise ValueError('Function input differs '+item['address'])
   last_end=-1
   for edit in item['edits']:
    if set(edit)!={'start','length','text'} or not isinstance(edit['start'],int) or isinstance(edit['start'],bool) or not isinstance(edit['length'],int) or isinstance(edit['length'],bool):raise ValueError('Invalid edit shape')
    pos,length,text=edit['start'],edit['length'],edit['text']
    if pos<0 or length<0 or pos<last_end or pos+length>len(body) or not isinstance(text,str) or not text.isascii() or '\r' in text or '\0' in text:raise ValueError('Invalid edit bounds/text')
    last_end=pos+max(length,1)
   for edit in reversed(item['edits']):body=body[:edit['start']]+edit['text']+body[edit['start']+edit['length']:]
   if sha(body.encode())!=item['after']:raise ValueError('Function output differs '+item['address'])
   patched=patched[:start]+body+patched[end:];records.append({k:v for k,v in item.items() if k!='file'});functions+=1;edit_count+=len(item['edits'])
  if records:patched=PREFIX+patched
  if patched!=new:raise ValueError('Sparse recipe does not reproduce '+name)
  newline='crlf' if b'\r\n' in new_raw else 'lf';encoded=patched.replace('\n','\r\n').encode('ascii') if newline=='crlf' else patched.encode('ascii')
  if encoded!=new_raw:raise ValueError('Mixed/reference output newlines '+name)
  newlines.add(newline);files.append({'name':name,'input_bytes_sha256':sha(old_raw),'before_sha256':sha(old.encode()),'after_sha256':sha(new.encode()),'output_bytes_sha256':sha(new_raw),'functions':records})
 if grouped or len(newlines)!=1:raise ValueError('Unexpected remaining units or mixed output convention')
 descriptor_raw=a.runtime_descriptor.read_bytes();descriptor_sha=sha(descriptor_raw);symbols_sha=sha(a.symbols.read_bytes())
 descriptor=json.loads(descriptor_raw)
 if descriptor.get("patcher_sha256")!=sha(a.patcher.read_bytes()):raise ValueError("Patcher differs from the reviewed runtime descriptor")
 recipe={'schema':1,'kind':'ssne-pal-translation-edits','format':'ascii-lf','output_newline':next(iter(newlines)),'runtime_kit_sha256':descriptor_sha,'symbols_sha256':symbols_sha,'patcher_sha256':sha(a.patcher.read_bytes()),'header_prefix':PREFIX,'function_count':functions,'edit_count':edit_count,'changed_unit_count':sum(bool(x['functions']) for x in files),'files':files}
 # Keep the established PAL patch scope explicit. A changed patcher requires
 # deliberate review, not silently producing a different public recipe.
 if (functions,edit_count,recipe['changed_unit_count'])!=(192,288,22):raise ValueError('Reviewed PAL patch scope changed')
 output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(recipe,indent=2,ensure_ascii=True)+'\n',encoding='utf-8',newline='\n')
 if a.sidecar_output:
  if not a.project:raise ValueError('--sidecar-output needs --project')
  sidecar={'schema':1,'runtime_kit_sha256':descriptor_sha,'patch_recipe':project_path(output,a.project),'patch_recipe_sha256':sha(output.read_bytes()),'symbols':project_path(a.symbols,a.project),'symbols_sha256':symbols_sha}
  a.sidecar_output.parent.mkdir(parents=True,exist_ok=True);a.sidecar_output.write_text(json.dumps(sidecar,indent=2)+'\n',encoding='utf-8',newline='\n')
 print(json.dumps({'files':len(files),'functions':functions,'edits':edit_count,'changed_units':recipe['changed_unit_count'],'recipe_sha256':sha(output.read_bytes()),'output_newline':recipe['output_newline']}))
if __name__=='__main__':main()
