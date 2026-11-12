"""Start through native Host menus and measure real stick movement, without placement."""
from test_native_frontend import Menus,sha
from pathlib import Path
import argparse,json,traceback,math

def main():
    ap=argparse.ArgumentParser()
    for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
    ap.add_argument('--mode',type=int,default=1)
    ap.add_argument('--client',action='store_true')
    ap.add_argument('--controls',action='store_true')
    ap.add_argument('--lobby-back',action='store_true')
    ap.add_argument('--port',type=int,default=27844)
    a=ap.parse_args()
    for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
    a.test_env={'SSNE_COOP_TRACE_LIFETIMES':'1','SSNE_COOP_CAMPAIGN_VERIFY':'1'}
    a.output.mkdir(parents=True,exist_ok=False);m=Menus(a,'',a.output)
    r={'passed':False,'exe_sha256':sha(a.exe),'mode':a.mode,'movements':[]};before=sha(a.card)
    def move(role,token):
        p=m.performance(role).get('position');assert p is not None,m.performance(role)
        m.pulse(token,(role,),seconds=1.5);m.delay(1.1)
        q=m.performance(role).get('position');assert q is not None,m.performance(role)
        d=math.hypot(q[0]-p[0],q[2]-p[2]);r['movements'].append({'role':role,'input':token,'before':p,'after':q,'distance':d,'state':m.state(role).copy()})
        print(role+' '+token+' distance='+str(d),flush=True)
    try:
        m.start_role('host',1);m.network('host');m.choose('host',0);m.page('host',2);pin=m.state()['pin'];m.choose('host',0)
        m.wait(lambda:m.state().get('active') and m.state().get('native_root')==0xa7);m.delay(1)
        for _ in range(a.mode-1):m.press('host','DOWN')
        m.press('host','A');m.delay(1)
        m.press('host','START');m.wait(lambda:m.state().get('lobby'))
        if a.client:
            m.start_role('client',0);m.network('client');m.choose('client',1);m.page('client',3);m.field('client',1,2,pin);m.choose('client',2);m.wait(lambda:m.state('client').get('lobby'))
            if a.lobby_back:
                m.press('client','B');m.page('client',7);m.choose('client',0);m.wait(lambda:m.state('client').get('lobby'))
                r['client_lobby_back_verified']=True
        m.press('host','START');m.delay(1);m.press('host','A');m.delay(1)
        if a.mode==1:
            m.wait(lambda:'original difficulty selection opened' in m.log('host'));m.press('host','A')
        m.wait(lambda:all(m.state(role).get('scene')==4 and m.performance(role).get('local_ready') for role in m.processes),90)
        m.delay(5)
        for _ in range(6):
            if not any(m.state(role).get('movie') for role in m.processes):break
            m.pulse('A',tuple(m.processes));m.delay(2)
        m.delay(2);r['initial']={role:m.state(role).copy() for role in m.processes}
        for role in m.processes:
            for token in ('SUP','SDOWN','SLEFT','SRIGHT'):move(role,token)
            if a.controls:
                m.press(role,'START');m.wait(lambda:m.state(role).get('pause'))
                p=m.performance(role)['position'];m.pulse('SUP',(role,),seconds=.5);m.delay(1.2);q=m.performance(role)['position']
                assert math.hypot(p[0]-q[0],p[2]-q[2])<.2,('moved while paused',role,p,q)
                m.press(role,'X');m.page(role,7);m.choose(role,0)
                m.wait(lambda:not m.state(role).get('visible'))
                if m.state(role).get('pause'):m.press(role,'B')
                m.wait(lambda:not m.state(role).get('pause'))
                move(role,'SRIGHT');assert r['movements'][-1]['distance']>.5,('movement after connection/pause',role)
                m.pulse('FIRE',(role,),seconds=1.5);m.delay(1)
        if a.controls:
            m.delay(2);idle={role:m.performance(role)['position'] for role in m.processes};m.delay(2)
            r['idle_drift']={role:math.hypot(m.performance(role)['position'][0]-p[0],m.performance(role)['position'][2]-p[2]) for role,p in idle.items()}
            assert all(d<.2 for d in r['idle_drift'].values()),r['idle_drift']
            r['fire_verified']={role:any(('[coop-weapon-shot-host]' in x or (('-loop-host]' in x) and 'active=1 ' in x)) and 'slot='+str(m.state(role)['local_slot'])+' ' in x for x in m.log('host').splitlines()) for role in m.processes}
            assert all(r['fire_verified'].values()),r['fire_verified']
        m.pulse('UISHOT',tuple(m.processes));m.close()
        r['errors']=[x for role in m.processes for x in m.log(role).splitlines() if '[port] Stopped:' in x or '[campaign-field-difference]' in x]
        r['passed']=all(sum(x['distance']>.5 for x in r['movements'] if x['role']==role)>=2 for role in m.processes) and not r['errors']
    except Exception:r['error']=traceback.format_exc()
    finally:
        m.close();r.update(visible=m.visible,focused=m.focused,exits={role:p.returncode for role,p in m.processes.items()},original_card_unchanged=sha(a.card)==before)
        r['passed'] &= not m.visible and not m.focused and r['original_card_unchanged'] and all(x==0 for x in r['exits'].values())
        (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps(r,indent=2),flush=True)
    return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
