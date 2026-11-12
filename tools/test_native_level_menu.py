"""Native level carousel snapshots and selection/back regression, private hidden GPU."""
from test_native_frontend import Menus,sha
from test_native_widescreen import capture
from pathlib import Path
from types import SimpleNamespace
import argparse,json,traceback

def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--port',type=int,default=27868);ap.add_argument('--fps',type=int,default=60)
 ap.add_argument('--aspects',nargs='+',choices=('4x3','16x9','21x9'),default=('4x3','16x9','21x9'))
 ap.add_argument('--capture-frame',type=int,default=0)
 a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.output.mkdir(parents=True,exist_ok=False);before=sha(a.card);r={'passed':False,'exe_sha256':sha(a.exe),'cases':[],'visual_review_passed':None}
 specs={'4x3':(0,960,640),'16x9':(3,960,853),'21x9':(3,1260,1120)}
 for name in a.aspects:
  aspect,width,output=specs[name];b=SimpleNamespace(**vars(a));b.private_settings='[graphics]\nrenderer=gpu\nscale=1\naspect='+str(aspect)+'\nhud_edges=1\nvsync=0\nwindow_w='+str(width)+'\nwindow_h=540\ntop_bar=0\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n'
  if a.capture_frame:b.test_env={'SSNE_GPU_CAPTURE':'capture:'+str(a.capture_frame)+':1'}
  m=Menus(b,'',a.output/name);c={'aspect':name,'passed':False,'captures':[]};r['cases'].append(c)
  def shot(label):c['captures'].append(capture(m,label,[output,448],[width,540]))
  try:
   m.start_role('host',1);m.network('host');m.choose('host',0);m.page('host',2);m.choose('host',0)
   m.wait(lambda:m.state().get('active') and m.state().get('native_root')==0xa7);m.delay(1);m.press('host','A');m.delay(1);m.press('host','START');m.wait(lambda:m.state().get('lobby'))
   m.press('host','START');m.wait(lambda:m.state().get('native_root')==0x4b);m.delay(2);shot('01-level-first')
   if a.capture_frame:m.delay(25)
   m.press('host','SRIGHT');m.delay(2);shot('02-level-next')
   m.press('host','SLEFT');m.delay(2);shot('03-level-return')
   m.press('host','B');m.wait(lambda:m.state().get('lobby'));c['back_to_lobby']=True
   m.press('host','START');m.wait(lambda:m.state().get('native_root')==0x4b);m.delay(2);shot('04-level-reopened')
   m.press('host','A');m.wait(lambda:'original difficulty selection opened' in m.log('host'));c['difficulty_opened']=True;c['passed']=True
  except Exception:c['error']=traceback.format_exc();print(c['error'],flush=True)
  finally:
   m.close();c.update(visible=m.visible,focused=m.focused,exits={role:p.returncode for role,p in m.processes.items()});c['passed'] &= not m.visible and not m.focused and all(code==0 for code in c['exits'].values())
   (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8')
  print(name+' carousel passed='+str(c['passed']),flush=True)
 r['original_card_unchanged']=sha(a.card)==before;r['passed']=all(c['passed'] for c in r['cases']) and r['original_card_unchanged']
 r['scope']='Dimensions and native selection/back only. Artwork/frame/thumbnail alignment requires manual review. No desktop input or campaign completion claim.'
 (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8')
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
