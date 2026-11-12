"""AFK human replacement: real Uzi pickup/fire, ammo-driven native weapon switch."""
from test_game_bots import Bots
from test_coop_campaign import sha
from pathlib import Path
import argparse,json,re,traceback,time,math

def loop_events(log,slot):
 result=[]
 for line in log.splitlines():
  if '[coop-uzi-loop-' not in line:continue
  data=dict(re.findall(r'(\w+)=([^ ]+)',line))
  if data.get('slot')==str(slot):result.append(data)
 return result

def active(log,slot):
 values={}
 for e in loop_events(log,slot):values[int(e['origin'])]=e['active']=='1'
 return [origin for origin,on in values.items() if on]

def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--old-switch',action='store_true');ap.add_argument('--afk-slot',type=int,choices=(0,1),default=1);ap.add_argument('--port',type=int,default=27874);ap.add_argument('--fps',type=int,default=60)
 a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.output.parent.mkdir(parents=True,exist_ok=True);a.bots=0;a.mode=1;a.capacity=8;a.open_menu=False;a.mod_flags=1
 afk={'SSNE_COOP_TEST_AFK':'1','SSNE_COOP_TEST_AFK_MASK':str(1<<a.afk_slot),'SSNE_COOP_TEST_AFK_DELAY_MS':str(45000 if a.afk_slot==0 else 5000),'SSNE_COOP_TEST_AFK_RETURN_MS':'120000'}
 a.test_env={'SSNE_COOP_TEST_PICKUP':'37','SSNE_COOP_TEST_PICKUP_AT_PLAYERS':'1','SSNE_COOP_TEST_PICKUP_MASK':str(1<<a.afk_slot),'SSNE_COOP_TEST_ENEMY_TYPE':'12','SSNE_COOP_TEST_ENEMY_COUNT':'10','SSNE_COOP_TEST_ENEMY_DISTANCE':'15','SSNE_COOP_TEST_ENEMY_SLOT':str(a.afk_slot),'SSNE_COOP_CAPTURE_RAM':'1','SSNE_COOP_CAPTURE_MIN_PLAYERS':'2'}
 if a.old_switch:a.test_env['SSNE_TEST_WEAPON_SWITCH_LOOP_FIX']='0'
 if a.afk_slot==0:a.test_env['SSNE_COOP_TEST_ENEMY_AGAIN_FRAME']='3000'
 a.client_test_env={}
 (a.test_env if a.afk_slot==0 else a.client_test_env).update(afk)
 p=Bots(a,'Rlevel1_3',a.output);before=sha(a.card);r={'passed':False,'exe_sha256':sha(a.exe),'afk_slot':a.afk_slot,'afk_delay_ms':45000 if a.afk_slot==0 else 5000,'old_switch':a.old_switch,'fixture':'Original Uzi pickup and native AFK combat; original inventory setter empties Uzi stock, leaving the native AFK weapon selection to choose the pistol. Audio muted externally; native voice/event lifetimes observed.'}
 try:
  p.start();p.wait(lambda:p.performance('host').get('local_ready'),90);p.start_role('client');p.wait(lambda:all(p.performance(role).get('local_ready') and p.performance(role).get('ready_players')==2 for role in ('host','client')),90)
  p.wait(lambda:('[coop-afk-state]' in p.log('host') and any('slot='+str(a.afk_slot)+' ' in x and 'afk=1 ' in x for x in p.log('host').splitlines() if '[coop-afk-state]' in x)),45)
  p.wait(lambda:bool(active(p.log('host'),a.afk_slot)),60);r['active_origins_before']=active(p.log('host'),a.afk_slot)
  cut=len(p.log('host'));start=time.monotonic();p.command('empty_primary '+str(a.afk_slot)+' 2')
  p.wait(lambda:any('slot='+str(a.afk_slot)+' weapon=1 ' in x for x in p.log('host')[cut:].splitlines() if '[coop-afk-weapon-choice]' in x),25);r['switch_seconds']=time.monotonic()-start
  p.delay(2);host=p.log('host');client=p.log('client');r['host_stopped_origins']=[int(e['origin']) for e in loop_events(host,a.afk_slot) if e['active']=='0'];r['client_stopped_origins']=[int(e['origin']) for e in loop_events(client,a.afk_slot) if e['active']=='0']
  r['client_retirements']=[x for x in client.splitlines() if '[coop-weapon-loop-retired]' in x]
  r['native_stop_during_switch']=[x for x in host[cut:].splitlines() if '[coop-weapon-switch-loop-stop]' in x and 'slot='+str(a.afk_slot)+' ' in x]
  switch_rows=[dict(re.findall(r'(\w+)=([^ ]+)',x)) for x in r['native_stop_during_switch']]
  r['switch_loop_origins']=[int(x['origin']) for x in switch_rows if x.get('active_handle')=='1' and x.get('weapon')=='2']
  r['active_loop_interrupted']={x.get('lane') for x in switch_rows if x.get('active_handle')=='1' and x.get('weapon')=='2'}=={'0','1'}
  r['switch_origins_stopped_both']=bool(r['switch_loop_origins']) and all(x in r['host_stopped_origins'] and x in r['client_stopped_origins'] and x not in active(host,a.afk_slot) for x in r['switch_loop_origins'])
  r['old_loop_stopped_host']=all(x in r['host_stopped_origins'] for x in r['active_origins_before']);r['old_loop_stopped_client']=all(x in r['client_stopped_origins'] for x in r['active_origins_before'])
  r['still_active_old_origins']=[x for x in active(host,a.afk_slot) if x in r['active_origins_before']]
  p.pulse('UISHOT');r['stock_emptied']='action=empty_primary slot='+str(a.afk_slot)+' weapon=2 ' in host
  r['passed']=r['active_loop_interrupted'] and r['switch_origins_stopped_both'] and r['stock_emptied'] and r['old_loop_stopped_host'] and r['old_loop_stopped_client'] and not r['still_active_old_origins']
 except Exception:r['error']=traceback.format_exc();print(r['error'],flush=True)
 finally:
  p.close();r.update(visible=p.visible,focused=p.focused,exits={role:x.returncode for role,x in p.processes.items()},original_card_unchanged=sha(a.card)==before)
  r['passed'] &= not p.visible and not p.focused and r['original_card_unchanged'] and all(code==0 for code in r['exits'].values())
  (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps(r,indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
