"""First-boot PAL language selection through original pointer-controlled UI.
The game creates a new private memory card. No forced-language environment hook,
preopened GUI, copied save or direct configuration assignment is used. Native
logo/title Continue is attempted through the same fresh mouse Left/A adapter;
unsupported original title semantics are reported rather than remapped.
"""
from test_native_mouse import MouseMenus,Pointer,public_menu,COMMAND_FILE
from native_mouse_inventory import original_inventory
from test_coop_campaign import sha
from pathlib import Path
import argparse,json,os,shutil,subprocess,time,traceback


class FirstBootMenus(MouseMenus):
 def start_role(self,role,language=None):
  folder=self.folder/role;folder.mkdir(parents=True);(folder/'steps').mkdir();shutil.copy2(self.a.exe,folder/'game.exe')
  (folder/'input.txt').write_text('');(folder/'command.txt').write_text('');(folder/'settings.ini').write_text(self.a.private_settings)
  card=folder/'card.raw';assert not card.exists(),'First-boot card must not already exist'
  env={key:value for key,value in os.environ.items() if not key.startswith('SSNE_')}
  env.update(SSNE_TEST_HIDDEN='1',SSNE_DEV_NATIVE_MENU='1',SSNE_TEST_DEVICE_INPUT='1',SSNE_NATIVE_MOUSE_COMMAND=COMMAND_FILE,SSNE_PAD_SCRIPT_FILE=str(folder/'input.txt'),SSNE_TEST_SCREENSHOT_DIR=str(folder/'steps'),SSNE_PERF_LOG=str(folder/'performance.jsonl'),SSNE_NATIVE_MENU_COMMAND=str(folder/'command.txt'),SSNE_NATIVE_MENU_PORT=str(self.a.port),SSNE_NATIVE_MENU_DISCOVERY_PORT=str(self.a.port+1))
  assert 'SSNE_NATIVE_TEST_LANGUAGE' not in env
  self.test_environment_keys=sorted(key for key in env if key.startswith('SSNE_'))
  startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
  stream=(folder/'console.log').open('wb');self.streams.append(stream)
  self.processes[role]=subprocess.Popen([str(folder/'game.exe'),'--data',str(self.a.data),'--memcard',str(card),'--mute','--fps',str(self.a.fps)],cwd=folder,env=env,stdout=stream,stderr=stream,startupinfo=startup,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
  # Unlike Menus.start_role, stop before scene3 and never auto-press B/Start.


def has_root(p,root):return any(surface['root']==root for surface in p.observe().get('surfaces',[]))


def continue_original_boot(p):
 deadline=time.monotonic()+90;next_click=time.monotonic()+.8;attempts=0;title_actions=[];boot_uv=[.98,.05];p.report['title_scene_observed']=False
 while not(p.m.state().get('scene')==3 and has_root(p,0x3d)):
  p.m.inspect();ack=p.ack();native=p.observe()
  if p.m.state().get('scene')==2:p.report['title_scene_observed']=True
  if time.monotonic()>deadline:
   raise TimeoutError('Original boot did not reach Main through observed native pointer actions; '+json.dumps({'menu':p.m.state(),'ack':ack,'mouse':native}))
  if time.monotonic()>=next_click:
   if ack.get('presentation') and not native.get('has_surface'):
    p.m.press('host','UISHOT');p.move(boot_uv)
    # Move is acknowledged asynchronously and can span the title-to-Main
    # transition. Revalidate immediately before Down so a planned title
    # click cannot activate the Main row that appeared under its point.
    ack=p.ack();native=p.observe()
    if not ack.get('presentation') or native.get('has_surface') or has_root(p,0x3d):
     p.cancel();p.report['steps'].append({'action':'cancel_stale_boot_pointer_intent','menu':public_menu(p.m.state()),'reason':'Native GUI appeared before presentation Down.'});continue
    # This corner is inside the game image and blank on Main. If its GUI
    # appears between the guard and Down, the pending point cannot select a
    # Main row. Release/cancel that race without claiming presentation proof.
    before=dict(ack);scene_before=p.m.state().get('scene');activations_before=p.observe().get('activations',0);p.button(True,boot_uv);after_down=dict(p.ack())
    if after_down.get('fallback_sequence',0)<=before.get('fallback_sequence',0) and has_root(p,0x3d):
     p.button(False,boot_uv);p.cancel();assert p.observe().get('activations',0)==activations_before,('Stale boot intent activated a native Main control',boot_uv,p.observe())
     p.report['steps'].append({'action':'cancel_boot_down_after_main_transition','uv':boot_uv,'menu':public_menu(p.m.state()),'presentation_click_proven':False,'native_activations_unchanged':True});continue
    p.m.delay(.06);p.button(False,boot_uv);p.m.delay(.25);after=dict(p.ack())
    assert after_down.get('fallback_sequence',0)>before.get('fallback_sequence',0),('Boot Left/A did not reach presentation adapter',before,after_down)
    attempts+=1;action={'action':'original_boot_fresh_pointer_left','uv':boot_uv,'scene_before':scene_before,'scene_after':p.m.state().get('scene'),'fallback_sequence_before':before.get('fallback_sequence'),'fallback_sequence_after':after_down.get('fallback_sequence'),'scope':'Original no-GUI Left/A duties only; no synthetic Start or button remap.'};p.report['steps'].append(action)
    if scene_before==2:title_actions.append(action)
   elif ack.get('active') and has_root(p,0xdd):
    try:p.widget('footer.confirm',root=0xdd)
    except AssertionError:pass
    else:
     p.m.press('host','UISHOT');p.click('footer.confirm',root=0xdd);p.report['steps'].append({'action':'original_first_boot_card_prompt_confirm','root':0xdd})
   next_click=time.monotonic()+1.2
  p.m.delay(.1)
 p.report['boot_pointer_continue_attempts']=attempts;p.report['title_pointer_actions']=title_actions
 p.report['title_pointer_continue_supported']=True if title_actions else None
 p.report['title_pointer_scope']='This language case requires actual Main and native language/localized Multiplayer Home. A brief observed scene2 is not assumed to be an interactive Title; accepted presentation actions are recorded separately, and reaching Main without one carries no Title mouse-support claim. No keyboard fallback or forced scene is used.'


def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--card',type=Path,help='Optional original-card integrity check only; never used as the boot card.')
 ap.add_argument('--language',type=int,choices=(0,1,2),default=1);ap.add_argument('--aspect',choices=('4x3','16x9','21x9'),default='16x9');ap.add_argument('--fps',type=int,default=60);ap.add_argument('--port',type=int,default=27970);a=ap.parse_args()
 for key in ('exe','data','output'):setattr(a,key,getattr(a,key).resolve())
 if a.card:a.card=a.card.resolve()
 original=sha(a.card) if a.card else None;a.output.mkdir(parents=True,exist_ok=False)
 aspect,width={'4x3':(0,960),'16x9':(3,960),'21x9':(3,1260)}[a.aspect]
 a.private_settings='[graphics]\nrenderer=gpu\nscale=1\naspect='+str(aspect)+'\nhud_edges=1\nside_fill=1\nvsync=0\nwindow_w='+str(width)+'\nwindow_h=540\ntop_bar=0\nfullscreen=0\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n'
 m=FirstBootMenus(a,'',a.output);report={'passed':False,'exe_sha256':sha(a.exe),'driver_sha256':sha(Path(__file__)),'language':a.language,'aspect':a.aspect,'fps':a.fps,'forced_language_override_present':False,'original_source_card_used_for_boot':False,'steps':[],'scope':__doc__};p=Pointer(m,report)
 try:
  m.start_role('host');report['test_environment_keys']=m.test_environment_keys
  m.wait(lambda:has_root(p,0xb7) and p.ack().get('active'),90);p.command('host','cancel');m.press('host','UISHOT')
  chooser=p.observe();chosen=0xb9+a.language;assert chosen in (0xb9,0xba,0xbb)
  report['chooser_root']=0xb7;report['chosen_widget']=chosen;report['chooser_before']=chooser
  widget=p.widget(chosen,root=0xb7);report['chooser_choice_bounds']=widget['bounds'];p.click(chosen,root=0xb7)
  m.wait(lambda:m.state().get('language')==a.language,40);report['selected_native_language']=m.state()['language']
  continue_original_boot(p);m.delay(.5);m.press('host','UISHOT');assert m.state().get('language')==a.language
  report['main_root']=m.state().get('native_root');report['main_language']=m.state().get('language');assert report['main_root']==0x3d
  inventory=original_inventory(a.data,a.language);report['main_multiplayer_label_expected']=inventory['widgets'][0x3f].get('label')
  p.click(0x3f,root=0x3d);m.wait(lambda:m.state().get('page')==1,15);m.delay(.4);m.press('host','UISHOT')
  assert m.state().get('language')==a.language
  assert '[native-network-menu] page=1 language='+str(a.language) in m.log('host')
  report['mp_home_page']=m.state()['page'];report['mp_home_language']=m.state()['language'];report['localization_proof_scope']='Original selected native language, original resource/source path and native Main/Home screenshots; no OCR claim.'
  card=a.output/'host/card.raw';report['fresh_card_created']=card.exists() and card.stat().st_size>0 and '[exi] created memory card A:' in (a.output/'host/console.log').read_text(encoding='utf8',errors='replace')
  assert report['fresh_card_created'],'Original EXI did not create a new formatted private card'
  p.click(0x14d);p.expect_root(0x3d);report['passed']=True
 except Exception:
  report['error']=traceback.format_exc();print(report['error'],flush=True)
  if m.processes.get('host') and m.processes['host'].poll() is None:
   try:m.press('host','UISHOT')
   except Exception:pass
 finally:
  p.save_coverage(original_inventory(a.data,a.language));report['final_state']=public_menu(m.state())
  if m.processes.get('host') and m.processes['host'].poll() is None:
   try:p.cancel()
   except Exception:pass
  m.close();report.update(visible=m.visible,focused=m.focused,exits={r:x.returncode for r,x in m.processes.items()},original_card_unchanged=sha(a.card)==original if a.card else None)
  report['passed'] &= not m.visible and not m.focused and all(code==0 for code in report['exits'].values()) and (report['original_card_unchanged'] is not False)
  (a.output/'result.json').write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf8');print(json.dumps({k:report.get(k) for k in ('passed','language','selected_native_language','main_language','mp_home_language','title_pointer_continue_supported','error')},indent=2),flush=True)
 return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
