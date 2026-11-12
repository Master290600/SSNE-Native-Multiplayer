"""Native Host start/cancel/rehost, GUI identity and Esc/Back regression."""
from test_native_frontend import Menus,sha
from pathlib import Path
import argparse,json,traceback

def main():
    ap=argparse.ArgumentParser()
    for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
    ap.add_argument('--cycles',type=int,default=3)
    ap.add_argument('--port',type=int,default=27846)
    a=ap.parse_args()
    for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
    a.output.mkdir(parents=True,exist_ok=False);m=Menus(a,'',a.output);before=sha(a.card)
    r={'passed':False,'exe_sha256':sha(a.exe),'cycles':[]}
    try:
        m.start_role('host',1);pid=m.processes['host'].pid
        for cycle in range(a.cycles):
            m.network('host');gui=m.state()['gui'];m.choose('host',0);m.page('host',2);m.choose('host',0)
            m.wait(lambda:m.state().get('active') and m.state().get('native_root')==0xa7);m.delay(1)
            assert m.state()['gui']==gui,('frontend recreated on host start',gui,m.state())
            assert '[native-lobby-adopted]' in m.log('host')
            m.press('host','A');m.delay(1);m.press('host','START');m.wait(lambda:m.state().get('lobby'))
            for repeat in range(2):
                m.press('host','B');m.wait(lambda:not m.state().get('lobby'),8)
                assert m.state()['active'] and m.state()['scene']==3,m.state()
                m.press('host','START');m.wait(lambda:m.state().get('lobby'),8)
            m.press('host','B');m.wait(lambda:not m.state().get('lobby'),8)
            for _ in range(6):
                if not m.state()['active']:break
                m.press('host','B');m.delay(.5)
            m.wait(lambda:not m.state().get('active') and m.state().get('native_root')==0x3d and not m.state().get('returning'))
            assert m.state()['gui']==gui,('frontend recreated on menu cancel',gui,m.state())
            assert m.processes['host'].pid==pid
            r['cycles'].append({'cycle':cycle,'gui_before':gui,'gui_after':m.state()['gui'],'same_process':True})
            print('Cycle '+str(cycle)+' retained frontend and Back worked',flush=True)
        m.network('host');m.choose('host',2);m.page('host',0);m.wait(lambda:m.state().get('native_root')==0xa7)
        m.press('host','B');m.wait(lambda:m.state().get('native_root')==0x3d)
        m.network('host');m.choose('host',0);m.page('host',2);m.choose('host',0);m.wait(lambda:m.state().get('native_root')==0xa7)
        m.press('host','A');m.delay(1);m.press('host','START');m.wait(lambda:m.state().get('lobby'))
        m.press('host','START');m.delay(1);m.press('host','A');m.wait(lambda:'original difficulty selection opened' in m.log('host'));m.press('host','A')
        m.wait(lambda:m.state().get('scene')==4 and m.performance('host').get('local_ready'),90);m.delay(5)
        for _ in range(3):
            m.press('host','START');m.wait(lambda:m.state().get('pause'),8)
            m.press('host','B');m.wait(lambda:not m.state().get('pause'),8)
        p=m.performance('host')['position'];m.pulse('SUP',('host',),seconds=1);m.delay(1.1);q=m.performance('host')['position']
        import math
        r['movement_after_escape']=math.hypot(p[0]-q[0],p[2]-q[2]);assert r['movement_after_escape']>.5
        m.press('host','UISHOT');r['passed']=True
    except Exception:r['error']=traceback.format_exc()
    finally:
        r['last_state']=m.state().copy();m.close();r.update(visible=m.visible,focused=m.focused,exits={role:p.returncode for role,p in m.processes.items()},original_card_unchanged=sha(a.card)==before)
        r['passed'] &= not m.visible and not m.focused and r['original_card_unchanged'] and all(x==0 for x in r['exits'].values())
        (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps(r,indent=2),flush=True)
    return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
