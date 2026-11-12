"""Hidden PAL weapon encounter; fixture pickups/placement, real bot controls and ammo."""
from test_game_bots import Bots
from test_coop_campaign import sha
from pathlib import Path
import argparse, json, time, traceback, re

def main():
    ap=argparse.ArgumentParser()
    for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
    ap.add_argument('--weapon',type=int,default=4)
    ap.add_argument('--mode',type=int,default=2)
    ap.add_argument('--seconds',type=int,default=45)
    ap.add_argument('--distance',type=float,default=15)
    ap.add_argument('--port',type=int,default=27819)
    ap.add_argument('--client',action='store_true')
    ap.add_argument('--moving',action='store_true')
    a=ap.parse_args()
    for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
    a.bots=1;a.level='Rlevel1_1'
    a.test_env={'SSNE_COOP_TEST_PICKUP':str(35+a.weapon),'SSNE_COOP_TEST_PICKUP_AT_PLAYERS':'1','SSNE_COOP_TEST_PICKUP_MASK':'2','SSNE_COOP_TEST_BOT_FIRE_TRACE':'1'}
    p=Bots(a,a.level,a.output);before=sha(a.card)
    r={'passed':False,'exe_sha256':sha(a.exe),'weapon':a.weapon,'mode':a.mode,'samples':[],
       'fixture':'One normal native weapon pickup spawned at bot; original player position setter places encounter. AI, weapon spin-up, ammunition, hits and damage run normally.'}
    try:
        p.start();p.delay(2);p.pulse('F12',('host',))
        if a.mode!=1:
            p.wait(lambda:'[native-mode-menu] original mode selection opened' in p.log('host'),40);p.delay(2)
            for _ in range(a.mode-1):p.pulse('DOWN',('host',))
            for token in ('A','A','START','A'):p.pulse(token,('host',));p.delay(2)
        p.wait(lambda:p.ready(),90)
        if a.client:
            p.start_role('client');p.delay(2);p.pulse('F12',('client',));p.wait(lambda:p.performance('client').get('local_ready'),60)
        p.wait(lambda:any(o.get('slot')==1 and o['weapon_owned_counts'][a.weapon]>0 for o in p.state().get('objects',[])),30)
        initial=p.state();r['samples'].append(initial)
        if a.mode==1:
            enemies=[o for o in initial['objects'] if o['kind']==1 and o.get('health',0)>0]
            r['enemy_health_before']={str(o['id']):o['health'] for o in enemies}
            q=enemies[0]['position']
            for slot in (0,2) if a.client else (0,):
                p.command('player_place '+str(slot)+' '+str(q[0]+(5 if slot==2 else 0))+' '+str(q[1])+' '+str(q[2]+a.distance+5));p.delay(.4)
            p.command('player_place 1 '+str(q[0])+' '+str(q[1])+' '+str(q[2]+a.distance))
        else:
            q=next(o for o in initial['objects'] if o.get('slot')==0)['position']
            p.command('player_place 1 '+str(q[0]-a.distance)+' '+str(q[1])+' '+str(q[2]))
        p.delay(1)
        until=time.monotonic()+a.seconds;step=0
        while time.monotonic()<until:
            p.inspect()
            if any(x.poll() is not None for x in p.processes.values()):raise RuntimeError('Peer exited')
            r['samples'].append(p.state())
            if a.moving:
                p.pulse('SRIGHT' if step%2==0 else 'SLEFT',('host',),seconds=.5);step+=1
            else:p.delay(.25)
        # Remove all opposing humans from perception using the same fixture
        # position handler, then check an ordinary AI decision releases R.
        for slot in ((0,2) if a.client else (0,)) + ((1,) if a.mode==1 else ()):
            p.command('player_place '+str(slot)+' -110 1 17.75');p.delay(.4)
        marker=len(p.log('host'));p.delay(3)
        tail=p.log('host')[marker:]
        r['released_without_target']=any('target=0 ' in x and 'fire=0 ' in x for x in tail.splitlines() if '[coop-afk-bot-input]' in x)
        p.close()
        r['errors']=[line for role in p.processes for line in p.log(role).splitlines() if '[port] Stopped:' in line or '[campaign-field-difference]' in line]
        r['weapon_choices']=[line for line in p.log('host').splitlines() if '[coop-afk-weapon-choice]' in line]
        r['fire_evidence']=[line for line in p.log('host').splitlines() if '[coop-afk-bot-input]' in line or '[coop-afk-weapon-evidence]' in line]
        bots=[o for s in r['samples'] for o in s.get('objects',[]) if o.get('slot')==1]
        r['weapons']=sorted(set(o['weapon'] for o in bots));r['ammo_values']=sorted(set(tuple(o['ammo']) for o in bots))
        r['health_values']={str(slot):sorted(set(o['health'] for s in r['samples'] for o in s.get('objects',[]) if o.get('slot')==slot)) for slot in (0,1)}
        traces=[dict(re.findall(r'(\w+)=([^ ]+)',x)) for x in p.log('host').splitlines() if '[game-bot-fire-trace]' in x]
        traces=[x for x in traces if int(x['weapon'])==a.weapon and int(x['chosen'])==a.weapon]
        r['ammo_spent']=sum(max(0,int(x['ammo'])-int(y['ammo'])) for x,y in zip(traces,traces[1:]))
        r['held_tracking_decisions']=sum(x['held']=='1' and .99<float(x['cosine'])<=.996 and x['fire']=='1' for x in traces)
        r['fire_trace']=traces
        r['native_loops']={role:[x for x in p.log(role).splitlines() if ('[coop-minigun-loop-' in x if a.weapon==4 else '[coop-uzi-loop-' in x) and 'slot=1 ' in x] for role in p.processes} if a.weapon in (2,4) else {}
        maximum=0;last=0;run_start=None
        for x in traces:
            tick=int(x['tick'])
            if x['fire']!='1':run_start=None
            elif run_start is None or tick-last>12:run_start=tick
            if run_start is not None:maximum=max(maximum,tick-run_start)
            last=tick
        r['longest_trigger_ticks']=maximum
        damaged=any(h<100 for h in r['health_values']['0'])
        if a.mode==1:
            final={str(o['id']):o.get('health') for o in r['samples'][-1].get('objects',[])}
            r['enemy_damage_observed']=[key for key,h in r['enemy_health_before'].items() if key not in final or (final[key] is not None and final[key]<h)]
            damaged=bool(r['enemy_damage_observed'])
        r['passed']=a.weapon in r['weapons'] and r['ammo_spent']>=2 and not r['errors'] and damaged and r['released_without_target']
        if a.weapon==4:r['passed'] &= maximum>=48 and all(any('active=1 ' in x for x in lines) for lines in r['native_loops'].values())
    except Exception:r['error']=traceback.format_exc()
    finally:
        p.close();r.update(exits={role:x.returncode for role,x in p.processes.items()},visible=p.visible,focused=p.focused,card_unchanged=sha(a.card)==before)
        r['passed'] &= not p.visible and not p.focused and r['card_unchanged'] and all(x==0 for x in r['exits'].values())
        (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8')
        print(json.dumps({k:v for k,v in r.items() if k not in ('samples','fire_evidence','ammo_values','fire_trace','native_loops')},indent=2),flush=True)
    return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
