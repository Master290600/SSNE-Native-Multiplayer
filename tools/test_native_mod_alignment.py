"""Native PAL enemy bones vs render-root projection and transported aim, hidden pair."""
from test_game_bots import Bots
from test_coop_campaign import sha
from pathlib import Path
import argparse,json,traceback,math,statistics

def records(path):
 if not path.exists():return []
 out=[]
 for line in path.read_text(encoding='utf8').splitlines():
  try:out.append(json.loads(line))
  except ValueError:pass
 return out

def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--port',type=int,default=27872);ap.add_argument('--fps',type=int,default=60)
 a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.test_env={'SSNE_TEST_DEVICE_INPUT':'1','SSNE_MOD_TEST_TRACE':'mod-trace.jsonl','SSNE_COOP_TRACE_LIFETIMES':'1','SSNE_COOP_TEST_ENEMY_TYPE':'12','SSNE_COOP_TEST_ENEMY_COUNT':'2','SSNE_COOP_TEST_ENEMY_DISTANCE':'12','SSNE_COOP_TEST_ENEMY_SLOT':'0'}
 a.output.parent.mkdir(parents=True,exist_ok=True);a.bots=0;a.mode=1;a.open_menu=False;a.client_test_env={'SSNE_TEST_DEVICE_INPUT':'1','SSNE_MOD_TEST_TRACE':'mod-trace.jsonl'};m=Bots(a,'Rlevel1_3',a.output);before=sha(a.card);r={'passed':False,'exe_sha256':sha(a.exe),'peers':{}}
 try:
  m.start();m.wait(lambda:m.performance('host').get('local_ready'),90);m.pulse('GODON',('host',));m.start_role('client')
  m.wait(lambda:all(m.performance(role).get('local_ready') and m.performance(role).get('ready_players')==2 for role in ('host','client')),90)
  m.delay(1)
  if '[coop-client-level-netricsa]' in m.log('client'):m.pulse('KEY_TAB',('client',),seconds=.3);m.delay(.7)
  m.wait(lambda:'[coop-test-enemy-wave]' in m.log('host'),40);m.delay(2);m.pulse('ESPON');m.delay(1);m.pulse('UISHOT')
  m.pulse('AIMON',seconds=1);m.delay(1.5);m.pulse('AIMOFF');m.pulse('UISHOT');m.delay(1)
  for role in ('host','client'):
   data=records(a.output/role/'mod-trace.jsonl');bones=[x for x in data if x.get('type')=='bone' and 'render_world' in x]
   finite=[x for x in bones if all(isinstance(v,(float,int)) and math.isfinite(v) for v in x['world']+x['render_world']+x['screen']+x['render_screen'])]
   world=[math.dist(x['world'],x['render_world']) for x in finite];pixels=[math.hypot((x['screen'][0]-x['render_screen'][0])*960,(x['screen'][1]-x['render_screen'][1])*540) for x in finite]
   aims=[x for x in data if x.get('type')=='aim' and x.get('enabled') and x.get('enemy') and x.get('bone')!=4294967295]
   r['peers'][role]={'bones':len(bones),'finite_bones':len(finite),'world_error_max':max(world,default=None),'pixel_error_max':max(pixels,default=None),'pixel_error_median':statistics.median(pixels) if pixels else None,'aim_samples':len(aims),'aim_eye_angles':[math.degrees(math.atan2(math.hypot(x['eye'][0],x['eye'][1]),-x['eye'][2])) for x in aims],'performance':m.performance(role)}
  r['diagnostic_completed']=all(v['bones'] and v['aim_samples'] for v in r['peers'].values())
  r['passed']=r['diagnostic_completed'] and all(v['pixel_error_max'] is not None and v['pixel_error_max']<2 and max(v['aim_eye_angles'][-3:])<1 for v in r['peers'].values())
 except Exception:r['error']=traceback.format_exc();print(r['error'],flush=True)
 finally:
  m.close();r.update(visible=m.visible,focused=m.focused,exits={role:p.returncode for role,p in m.processes.items()},original_card_unchanged=sha(a.card)==before)
  r['passed'] &= not m.visible and not m.focused and r['original_card_unchanged'] and all(code==0 for code in r['exits'].values())
  (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps({k:v for k,v in r.items() if k!='peers'},indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
