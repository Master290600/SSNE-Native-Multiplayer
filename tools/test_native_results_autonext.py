"""Private native result flow: original exit event, never a forced next load.
Exploration is shortened via the original WorldLink handler; this tests the
result GUI, AFK input and synchronized continuation, not a campaign playthrough.
"""
from test_native_cave_gate import CavePeers
from test_coop_campaign import sha
from pathlib import Path
import argparse,json,time,traceback,re

def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--host-modal',action='store_true');ap.add_argument('--plain-auto',action='store_true');ap.add_argument('--case',choices=('auto-afk','manual-afk','toggle-afk'),required=True)
 ap.add_argument('--port',type=int,default=27888);ap.add_argument('--fps',type=int,default=60)
 a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.output.parent.mkdir(parents=True,exist_ok=True);a.bots=0;a.mode=1;a.capacity=8;a.open_menu=False;a.mod_flags=0
 common={'SSNE_TEST_DEVICE_INPUT':'1','SSNE_DEV_NATIVE_MENU':'1','SSNE_TEST_ANIMATION_LIFETIME_TRACE':'1','SSNE_NATIVE_MENU_DISCOVERY_PORT':str(a.port+1)}
 a.test_env=dict(common,SSNE_COOP_TEST_AFK='1',SSNE_COOP_TEST_AFK_MASK='1',SSNE_COOP_TEST_AFK_DELAY_MS='5000',SSNE_COOP_TEST_AFK_RETURN_MS='300000')
 a.client_test_env=dict(common)
 p=CavePeers(a,'Rlevel1_1',a.output);before=sha(a.card)
 r={'passed':False,'case':a.case,'plain_auto':a.plain_auto,'host_modal':a.host_modal,'exe_sha256':sha(a.exe),'steps':[],'scope':__doc__}
 def menu(role):
  try:return json.loads((a.output/role/'native-menu-state.json').read_text())
  except (OSError,ValueError):return {}
 def step(text):r['steps'].append(text);print(text,flush=True)
 def press(role,button,seconds=.18):p.pulse(button,(role,),seconds)
 def choose(role,index):
  for _ in range(7):
   if menu(role).get('focus')==index:break
   press(role,'DOWN')
  assert menu(role).get('focus')==index,menu(role)
  press(role,'A')
 def result_sequences(role):
  marker='[coop-native-results]' if role=='host' else '[coop-client-native-results]'
  return re.findall(re.escape(marker)+r' sequence=(\d+)',p.log(role))
 try:
  p.start();p.initial_ready(('host',));p.start_role('client');p.initial_ready(('host','client'))
  p.wait(lambda:all(p.performance(role).get('local_ready') and p.performance(role).get('ready_players')==2 for role in ('host','client')),90)
  p.wait(lambda:'slot=0 afk=1 ' in p.log('host'),30)
  if menu('client').get('info'):press('client','KEY_TAB')
  p.delay(1);state=p.state();links=[x for x in state['objects'] if x.get('target') and not x.get('terminal')]
  assert links,'No normal next-level WorldLink'
  link=next((x for x in links if x['target']=='Rlevel1_2'),links[0]);target=link['target'];r['target']=target;r['exit_fixture']='Original native WorldLink event; exploration shortcut only.'
  cut={role:len(p.log(role)) for role in ('host','client')};p.command('link '+target)
  p.wait(lambda:bool(result_sequences('host')),30);opened=time.monotonic();r['host_result_sequence']=result_sequences('host')[-1]
  p.wait(lambda:bool(result_sequences('client')),15);r['client_result_sequence']=result_sequences('client')[-1]
  assert r['host_result_sequence']==r['client_result_sequence'];step('Both peers display the same original result sequence with host AFK')
  if not a.plain_auto:
   p.wait(lambda:menu('client').get('results_root')==0xBC,5)
   result_pause=menu('client').get('pause')
   press('client','KEY_ENTER');p.delay(.7)
   client_confirmation=menu('client');r['client_confirmation_state']=client_confirmation
   assert client_confirmation.get('results_root')==0xBC and client_confirmation.get('pause')==result_pause,client_confirmation
   assert not (client_confirmation.get('input',0)&0x1000),'Result Enter leaked native Start/Pause'
   assert '[coop-results-auto-continue]' not in p.log('host')[cut['host']:] and p.performance('host').get('level')=='Rlevel1_1'
   step('Client confirmation cannot independently advance the authority')
  p.pulse('UISHOT')
  if a.case=='auto-afk':
   if not a.plain_auto:
    modal_role='host' if a.host_modal else 'client'
    modal_cut=len(p.log(modal_role));press(modal_role,'X');p.wait(lambda:menu(modal_role).get('page')==7,10)
    assert menu(modal_role).get('gui_port')==menu(modal_role).get('local_slot'),menu(modal_role)
    p.pulse('UISHOT');step(modal_role.title()+' Connection modal left open during host countdown')
    deadline=time.monotonic()+25
    while '[coop-results-auto-continue]' not in p.log('host')[cut['host']:]:
     modal_log=p.log(modal_role)[modal_cut:]
     assert '[coop-results-cleanup]' not in modal_log and '[coop-results-setup]' not in modal_log,'Connection dispatched result lifecycle while suspended'
     if time.monotonic()>deadline:raise TimeoutError('No automatic Continue')
     p.delay(.1)
   else:p.wait(lambda:'[coop-results-auto-continue]' in p.log('host')[cut['host']:],25)
   r['auto_observed_seconds']=time.monotonic()-opened
   assert 14.0<=r['auto_observed_seconds']<=19.0,r['auto_observed_seconds']
   step('Authority runs original Continue after fifteen seconds without host input')
  else:
   if a.case=='toggle-afk':
    modal_cut=len(p.log('host'));press('host','X');p.wait(lambda:menu('host').get('page')==7,10);p.pulse('UISHOT')
    choose('host',3);p.wait(lambda:'slot=0 afk=0 ' in p.log('host')[cut['host']:],10);step('Host disables AFK through original Connection row on the result screen')
    choose('host',0);p.wait(lambda:menu('host').get('page')==0 and menu('host').get('results_root')==0xBC,10)
    assert result_sequences('host')[-1]==r['host_result_sequence']
    modal_log=p.log('host')[modal_cut:];assert '[coop-results-cleanup]' not in modal_log and '[coop-results-setup]' not in modal_log,'AFK modal reopened or retired the original result variables'
    step('Returning preserves result sequence, variables and score GUI')
    press('host','X');p.wait(lambda:menu('host').get('page')==7,10);choose('host',0);p.wait(lambda:menu('host').get('page')==0 and menu('host').get('results_root')==0xBC,10)
    modal_log=p.log('host')[modal_cut:];assert '[coop-results-cleanup]' not in modal_log and '[coop-results-setup]' not in modal_log,'Reopening changed result lifecycle'
    step('Connection can reopen and return without changing the original result')
   press('host','KEY_ENTER');p.delay(.45)
   if p.performance('host').get('level')=='Rlevel1_1':press('host','KEY_ENTER')
   step('Host manual confirmation sent'+(' while still AFK' if a.case=='manual-afk' else ' after disabling AFK'))
  p.wait(lambda:all(p.performance(role).get('level')==target and p.performance(role).get('local_ready') and p.performance(role).get('ready_players')==2 for role in ('host','client')),90)
  r['ready_seconds']=time.monotonic()-opened
  if a.case=='auto-afk':p.wait(lambda:all(not menu(role).get('visible') for role in ('host','client')),10)
  else:assert '[coop-results-auto-continue]' not in p.log('host')[cut['host']:],'Manual case was only advanced by timeout'
  r['auto_events']=[x for x in p.log('host')[cut['host']:].splitlines() if '[coop-results-auto-continue]' in x]
  r['progress_saves']=[x for x in p.log('host')[cut['host']:].splitlines() if '[coop-native-progress-save-request]' in x]
  assert len(r['progress_saves'])==1,r['progress_saves']
  if a.case!='toggle-afk':assert 'slot=0 afk=0 ' not in p.log('host')[cut['host']:]
  r['final_performance']={role:p.performance(role) for role in ('host','client')}
  p.pulse('UISHOT');step('Both peers ready in original next level, one progress save, no stale result modal');r['passed']=True
 except Exception:r['error']=traceback.format_exc();print(r['error'],flush=True)
 finally:
  p.close();r.update(visible=p.visible,focused=p.focused,exits={role:x.returncode for role,x in p.processes.items()},original_card_unchanged=sha(a.card)==before)
  r['passed'] &= not p.visible and not p.focused and r['original_card_unchanged'] and all(code==0 for code in r['exits'].values())
  (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps({k:r.get(k) for k in ('passed','case','auto_observed_seconds','ready_seconds','error')},indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
