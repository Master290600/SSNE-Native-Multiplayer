"""Private F12 host/client cheat regression through measured ImGui controls.

Declared --coop-host/--coop-join and automatic intro retirement provide gameplay
setup only. This does not test native host creation, LAN discovery, a full level
or a campaign. Every cheat change uses the measured F12 pointer adapter. Raw
Space/weapon-key files and FIRE feed ordinary device bindings inside hidden,
private children; no desktop focus/input or production settings/cards are used.
"""
from pathlib import Path
import argparse
import json
import math
import os
import re
import struct
import time
import traceback
from test_custom_crosshair import (CrosshairMenus, CrosshairPointer, SettingsPointer,
                                    direct_gameplay_setup, source_state, record)
from test_coop_campaign import sha

ROLES=('host','client')
KEY_FILE='device-keys.txt'
CHEAT_IDS=('cheat_god','cheat_ammo','cheat_weapons','cheat_fly','cheat_unlock','cheat_off')


def atomic_text(path,value):
    pending=path.with_suffix(path.suffix+'.pending');pending.write_text(value,encoding='ascii')
    until=time.monotonic()+2
    while True:
        try:os.replace(pending,path);return
        except PermissionError:
            if time.monotonic()>until:raise
            time.sleep(.01)


def players(m):
    return {int(p['slot']):p for p in m.campaign().get('objects',[])
            if 'slot' in p and 'ammo' in p and int(p['slot']) in (0,1)}


def fresh_players(m):
    previous=m.campaign().get('tick',0)
    m.wait(lambda:m.campaign().get('tick',0)>previous and set(players(m))=={0,1},10)
    return players(m)


def ram_snapshot(p,label):
    m=p.m;m.campaign_command('dump');m.delay(.15)
    data=(m.folder/'host/campaign-capture.ram').read_bytes()
    assert len(data) in (0x01800000,0x04000000),('Unexpected private RAM size',len(data))
    def read(fmt,address):
        size=struct.calcsize(fmt);offset=address-0x80000000
        assert 0<=offset<=len(data)-size,('Guest address outside snapshot',hex(address),size)
        return struct.unpack_from(fmt,data,offset)
    def u32(a):return read('>I',a)[0]
    result={}
    for slot,entry in players(m).items():
        actor=int(entry['address']);assert u32(actor+0x48)==0x802121e8,entry
        inventory=u32(actor+0x5e8);weapon=u32(actor+0x5dc)
        instance=u32(actor+0x4bc+24*weapon);modes=u32(instance+0x1d0)
        result[slot]={'address':actor,'position':list(read('>3f',actor+0x20)),
                      'flags':u32(actor+0x1c),'flight':u32(actor+0x35c),
                      'inventory':inventory,'infinite':u32(inventory+0xd8),'weapon':weapon,
                      'primary':list(read('>18H',inventory)),
                      'secondary':list(read('>18H',inventory+0x24)),
                      'maximum':list(read('>18H',inventory+0xb4)),
                      'primary_ammo_type':read('>i',modes+0x38)[0],
                      'health':read('>f',actor+0xcc)[0]}
    assert set(result)=={0,1},('Both real party inventories required',result)
    record(p,label,players=result,host_ui_cheats=m.ui().get('cheats'),
           note='Frame-boundary dump may temporarily lift host flight flags; movement and stable UI flying state establish flight effect.')
    return result


def raw_keys(m,roles,codes):
    value=' '.join(map(str,codes))
    for role in roles:atomic_text(m.folder/role/KEY_FILE,value)


def key_pulse(m,roles,codes,seconds=.22):
    raw_keys(m,roles,codes)
    try:m.delay(seconds)
    finally:raw_keys(m,roles,())
    m.delay(.25)


def open_cheats(settings,role='host'):
    settings.open(role);settings.click('tab_cheats',role)
    settings.m.wait(lambda:any(w['id']=='cheat_ammo' for w in settings.m.ui(role).get('widgets',[])),10)


def switch(p,settings,key,wanted):
    assert p.m.ui().get('menu_open')
    state=p.m.ui().get('cheats',{});assert key in state,('Missing cheat diagnostic',key,state)
    if bool(state[key])!=wanted:settings.click('cheat_'+key)
    p.m.wait(lambda:bool(p.m.ui().get('cheats',{}).get(key))==wanted,8)
    record(p,'f12_host_switch',key=key,value=wanted,ui=p.m.ui())


def shared(m,wanted):
    return all(all(bool(m.ui(role).get('cheats',{}).get(key))==value for key,value in wanted.items())
               for role in ROLES)


def client_read_only(p,settings):
    m=p.m;open_cheats(settings,'client')
    state=m.ui('client');widgets={w['id']:w for w in state.get('widgets',[])}
    assert all(key in widgets and not widgets[key]['enabled'] for key in CHEAT_IDS),('Client gameplay cheats must all be disabled',widgets)
    before=dict(state['cheats']);host_before=dict(m.ui()['cheats'])
    # Inspect all disabled controls. Also hit the measured visible ammo box to
    # prove the normal ImGui disabled region ignores a real pointer release.
    control=widgets['cheat_ammo'];bounds=SettingsPointer.intersect(control)
    assert control.get('visible') and bounds,('Disabled ammo box must be visible',control)
    x=(bounds[0]+bounds[2])*.5;y=(bounds[1]+bounds[3])*.5
    for action in ('move','down left','up left'):p.command('client',f'{action} {x} {y}')
    m.delay(.3)
    assert m.ui('client')['cheats']==before and m.ui()['cheats']==host_before,('Client changed gameplay cheats',before,m.ui('client'))
    record(p,'client_f12_controls_disabled_and_pointer_ignored',controls={key:widgets[key] for key in CHEAT_IDS},cheats=before)
    settings.close('client')


def select_uzis(p):
    m=p.m
    for attempt in range(3):
        current=fresh_players(m);roles=tuple(ROLES[slot] for slot in (0,1) if current[slot]['weapon']!=2)
        if not roles:return
        # SDL scancode33 is the normal number4 binding: Uzi/Minigun group.
        key_pulse(m,roles,(33,));m.delay(1)
    current=fresh_players(m);assert all(current[slot]['weapon']==2 for slot in (0,1)),('Normal number4 did not select Uzis',current)


def verify_replica(p,offset):
    m=p.m;patterns={
        'world':r'\[campaign-verify\][^\n]* differences=(\d+)',
        'inventory':r'\[coop-client-inventory\][^\n]* players=(\d+) differences=(\d+)',
        'player_pose':r'\[coop-client-player-pose\][^\n]* players=(\d+) differences=(\d+)'}
    def rows():
        log=m.log('client')[offset:]
        return {key:re.findall(pattern,log) for key,pattern in patterns.items()}
    m.wait(lambda:all(rows().values()),20)
    found=rows()
    assert all(int(value)==0 for value in found['world']),('World replica mismatch',found)
    for name in ('inventory','player_pose'):
        assert all(int(count)>=2 and int(difference)==0 for count,difference in found[name]),(name,found)
    record(p,'client_replica_world_inventory_and_player_pose_verified',samples=found)


def exercise(p,settings):
    m=p.m;m.start_role('host',m.a.language);direct_gameplay_setup(p)
    fresh_players(m);log_start=len(m.log('client'))
    open_cheats(settings)
    for key in ('god','ammo','weapons'):switch(p,settings,key,True)
    settings.close();m.wait(lambda:shared(m,{'god':True,'ammo':True,'weapons':True}),12)
    client_read_only(p,settings);select_uzis(p)
    on_before=ram_snapshot(p,'ammo_on_before_real_shots')
    assert all(row['infinite']==1 and row['weapon']==2 for row in on_before.values()),on_before
    m.pulse('FIRE',ROLES,seconds=1.5);on_after=ram_snapshot(p,'ammo_on_after_real_shots')
    for slot in (0,1):
        assert on_after[slot]['primary']==on_before[slot]['primary'] and on_after[slot]['secondary']==on_before[slot]['secondary'],('Infinite ammunition was consumed',slot,on_before,on_after)
    open_cheats(settings);switch(p,settings,'ammo',False);settings.close()
    m.wait(lambda:shared(m,{'god':True,'ammo':False,'weapons':True}),12)
    off_before=ram_snapshot(p,'ammo_off_before_real_shots')
    assert all(row['infinite']==0 for row in off_before.values()),off_before
    m.pulse('FIRE',ROLES,seconds=1.5);off_after=ram_snapshot(p,'ammo_off_after_real_shots')
    for slot in (0,1):
        assert off_after[slot]['weapon']==2 and any(a<b for a,b in zip(off_after[slot]['primary'],off_before[slot]['primary'])),('Identical normal firing did not consume ammunition with cheat off',slot,off_before,off_after)
    record(p,'real_shots_prove_ammo_on_off_for_host_and_client')
    open_cheats(settings);switch(p,settings,'ammo',True);settings.close()
    before_flight=ram_snapshot(p,'flight_off_baseline')
    open_cheats(settings);switch(p,settings,'fly',True);settings.close()
    m.wait(lambda:bool(m.ui().get('cheats',{}).get('flying')),12)
    before=ram_snapshot(p,'flight_on_before_input')
    key_pulse(m,('host',),(44,),seconds=.45)  # Real Space binding, held ascent.
    above=ram_snapshot(p,'flight_after_held_space')
    assert above[0]['position'][1]>before[0]['position'][1]+.05,('Space did not move host upwards',before,above)
    m.delay(.5);neutral=ram_snapshot(p,'flight_neutral_stays_airborne')
    assert abs(neutral[0]['position'][1]-above[0]['position'][1])<.01,('Neutral flight fell or drifted',above,neutral)
    m.pulse('FIRE',('host',),seconds=.45);below=ram_snapshot(p,'flight_after_held_fire')
    assert below[0]['position'][1]<neutral[0]['position'][1]-.05,('Fire did not move host downwards',neutral,below)
    for sample in (before,above,neutral,below):
        assert sample[1]['flight']==before_flight[1]['flight'] and (sample[1]['flags']&0xc000)==(before_flight[1]['flags']&0xc000),('Host flight changed client physics flags',before_flight,sample)
    open_cheats(settings);switch(p,settings,'fly',False);settings.close()
    m.wait(lambda:not m.ui().get('cheats',{}).get('flying'),12)
    restored=ram_snapshot(p,'flight_off_restores_original_bits')
    assert restored[0]['flight']==before_flight[0]['flight'] and (restored[0]['flags']&0xc000)==(before_flight[0]['flags']&0xc000),('Flight disabled did not restore original host bits',before_flight,restored)
    verify_replica(p,log_start)
    open_cheats(settings);settings.click('cheat_off');settings.close()
    m.wait(lambda:shared(m,{'god':False,'ammo':False,'weapons':False}) and not m.ui().get('cheats',{}).get('flying'),12)
    final=ram_snapshot(p,'f12_all_off_party_restore')
    assert all(row['infinite']==0 for row in final.values()),final
    record(p,'f12_all_off_updates_both_peers',host=m.ui()['cheats'],client=m.ui('client')['cheats'])


def arguments():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('exe','data','card','png','output'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--port',type=int,default=28760)
    parser.add_argument('--kbm-player',type=int,choices=(1,2,3,4),default=2)
    a=parser.parse_args()
    for key in ('exe','data','card','png','output'):setattr(a,key,getattr(a,key).resolve())
    assert all(str(getattr(a,key)).isascii() for key in ('exe','data','card','png','output')),'Use private ASCII test paths'
    assert a.exe.is_file() and a.card.is_file() and a.png.is_file() and (a.data/'sys/main.dol').is_file()
    assert 1024<a.port<65528
    a.case='host-client-direct';a.language=a.client_language=1;a.fps=60;a.allow_start_key_fallback=False
    a.test_env={'SSNE_TEST_DEVICE_KEY_FILE':KEY_FILE,'SSNE_COOP_CAMPAIGN_VERIFY':'1'}
    a.private_settings='[graphics]\nrenderer=gpu\nscale=1\naspect=3\nhud_edges=1\nside_fill=1\nvsync=0\nwindow_w=1100\nwindow_h=850\ntop_bar=0\nfullscreen=0\n[audio]\nmuted=1\n[general]\npause_unfocused=0\npause_in_menu=0\nskip_intros=1\ngame_language=1\nkbm_player='+str(a.kbm_player)+'\n'
    return a


def main():
    a=arguments();a.output.mkdir(parents=True,exist_ok=False);before=source_state(a)
    report={'passed':False,'scope':__doc__,'steps':[],'exe_sha256':before['exe'],'driver_sha256':sha(Path(__file__)),'kbm_player':a.kbm_player}
    m=CrosshairMenus(a,'Rlevel1_1',a.output/'host-client-direct');p=CrosshairPointer(m,report);settings=SettingsPointer(p)
    try:exercise(p,settings);report['passed']=True
    except Exception:
        report['error']=traceback.format_exc();print(report['error'],flush=True)
        report['failure_diagnostics']={role:m.diagnostics(role) for role in m.processes}
    finally:
        for role,child in m.processes.items():
            if child.poll() is None:
                try:raw_keys(m,(role,),());(m.folder/role/'input.txt').write_text('');settings.close(role)
                except Exception:pass
        m.close()
        report.update(visible=m.visible,focused=m.focused,launches=[{'role':entry['role'],'pid':entry['process'].pid,'exit':entry['process'].returncode} for entry in m.launches],final={role:m.diagnostics(role) for role in m.processes})
        after=source_state(a);report['source_files_unchanged']={key:before[key]==after[key] for key in before}
        report['passed'] &= all(report['source_files_unchanged'].values()) and bool(m.launches) and not m.visible and not m.focused and all(entry['process'].returncode==0 for entry in m.launches)
        (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
        print(json.dumps({key:report[key] for key in ('passed','source_files_unchanged')},indent=2),flush=True)
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
