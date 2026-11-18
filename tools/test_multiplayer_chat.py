"""Private multiplayer chat acceptance using real SDL text/key events.

CLI host/client and automatic intro retirement are declared setup fixtures,
not LAN-browser or original host-creation acceptance. A God/all-weapons fixture
and ordinary number4 selection supply Uzis so FIRE suppression is measurable.
T, text, Enter, Escape and hotkeys enter the ordinary SDL event path; raw W,
Space and FIRE snapshots enter the normal device bindings. No desktop input,
window focus, production settings or original save-card writes are performed.
An optional WorldLink shortcut checks history across a native level change.
"""
from pathlib import Path
import argparse
import json
import math
import os
import struct
import time
import traceback
from test_custom_crosshair import (CrosshairMenus, CrosshairPointer, SettingsPointer,
                                    direct_gameplay_setup, source_state, record)
from test_host_f12_cheats import atomic_text, raw_keys, select_uzis, fresh_players
from test_native_mouse import ident, public_menu
from test_native_mouse_multiplayer import (information_surface, information_controls,
    wait_information_retired, open_pause, connection, wait_lobby, page)
from test_coop_campaign import sha

ROLES=('host','client')
KEY_FILE='device-keys.txt'
T,ENTER,ESCAPE,F3,F12=23,40,41,60,69


class ChatMenus(CrosshairMenus):
    def __init__(self,*args):
        super().__init__(*args);self.chat_cache={}
    def chat(self,role='host'):
        return self.copied_json(role,'chat-ui-state.json',self.chat_cache)


def key(p,role,code):
    p.command(role,'key down '+str(code));p.m.delay(.06)
    p.command(role,'key up '+str(code));p.m.delay(.08)


def open_chat(p,role='host'):
    key(p,role,T)
    p.m.wait(lambda:p.m.chat(role).get('active'),8)
    trace=p.m.chat(role)
    assert trace.get('draft')=='' and trace.get('cursor')==0,('Opening T leaked into chat draft',role,trace)
    bounds=trace.get('input_rect',[])
    assert len(bounds)==4 and all(math.isfinite(value) for value in bounds) and bounds[2]>bounds[0] and bounds[3]>bounds[1],trace
    record(p,'t_opens_empty_real_editor',role=role,chat=trace)


def native_gameplay_unchanged(p,role):
    state=p.m.state(role)
    assert state.get('scene')==4 and state.get('active') and state.get('page')==0,('Chat changed native session/page',role,state)
    assert not state.get('pause') and not state.get('info') and not state.get('movie'),('Chat confirmation escaped to native pause/info/presentation',role,state)
    assert not p.observe(role).get('has_surface') and not p.ack(role).get('presentation'),('Chat opened a native menu or presentation surface',role,p.m.diagnostics(role))


def screenshot(p,label,role='host'):
    folder=p.m.folder/role/'steps';before=set(folder.glob('ui-*.png'))
    p.command(role,'ui_shot')
    p.m.wait(lambda:bool(set(folder.glob('ui-*.png'))-before),8)
    path=max(set(folder.glob('ui-*.png'))-before,key=lambda entry:entry.stat().st_mtime_ns)
    record(p,'actual_chat_window_screenshot',role=role,label=label,path=str(path))
    return str(path)


def histories(m):return {role:m.chat(role).get('messages',[]) for role in ROLES}


def wait_history(p,expected):
    p.m.wait(lambda:all(p.m.chat(role).get('messages')==expected for role in ROLES),12)
    for role in ROLES:
        sequence=[message['sequence'] for message in p.m.chat(role)['messages']]
        assert sequence==list(range(1,len(expected)+1)),('Chat duplicated/reordered messages',role,sequence)
        assert not p.m.chat(role).get('send_failed'),('Chat unexpectedly failed',role,p.m.chat(role))


def send(p,role,text,expected):
    open_chat(p,role);p.command(role,'text '+text)
    p.m.wait(lambda:p.m.chat(role).get('draft')==text,8)
    assert p.m.chat(role).get('cursor')==len(text.encode('utf8'))
    key(p,role,ENTER)
    p.m.wait(lambda:not p.m.chat(role).get('active'),8)
    expected.append({'sequence':len(expected)+1,'name':role,'text':text})
    wait_history(p,expected)
    assert p.m.chat(role).get('last_submission')==text,('Wrong editor submission',role,p.m.chat(role))
    for peer in ROLES:
        assert expected[-1]['sequence'] in p.m.chat(peer).get('visible_sequences',[]),('New chat did not appear in HUD',peer,p.m.chat(peer))
        native_gameplay_unchanged(p,peer)
    record(p,'real_sdl_chat_sent_and_echoed_once',sender=role,text=text,peers=histories(p.m))


def sample(p,label):
    m=p.m;fresh_players(m);m.campaign_command('dump');m.delay(.10)
    data=(m.folder/'host/campaign-capture.ram').read_bytes()
    assert len(data) in (0x01800000,0x04000000),len(data)
    def values(fmt,address):
        offset=address-0x80000000;size=struct.calcsize(fmt)
        assert 0<=offset<=len(data)-size,(hex(address),size)
        return struct.unpack_from(fmt,data,offset)
    result={}
    for actor in m.campaign().get('objects',[]):
        if actor.get('slot') not in (0,1) or 'ammo' not in actor:continue
        slot=int(actor['slot']);address=int(actor['address'])
        assert values('>I',address+0x48)[0]==0x802121e8,actor
        inventory=values('>I',address+0x5e8)[0]
        result[slot]={'position':list(values('>3f',address+0x20)),
                      'ammo':list(values('>18H',inventory)),
                      'secondary':list(values('>18H',inventory+0x24)),
                      'weapon':values('>I',address+0x5dc)[0],
                      'infinite':values('>I',inventory+0xd8)[0]}
    assert set(result)=={0,1},result
    record(p,label,players=result,chat={role:m.chat(role) for role in ROLES})
    return result


def devices(m,role,keys=(),fire=False):
    raw_keys(m,(role,),keys)
    atomic_text(m.folder/role/'input.txt','FIRE@0:100000' if fire else '')


def isolation(p,role,expected):
    m=p.m;slot=0 if role=='host' else 1
    open_chat(p,role);draft='Schreibe gerade im Chat: 100% ###literal'
    p.command(role,'text '+draft);m.wait(lambda:m.chat(role).get('draft')==draft,8)
    screenshot(p,role+'_active_editor',role)
    ini=m.folder/role/'settings-netplay.ini';ini_before=ini.read_bytes()
    for code in (F3,F12):key(p,role,code)
    assert m.chat(role).get('active') and m.chat(role).get('draft')==draft
    assert not m.ui(role).get('menu_open'),('F12 escaped from active chat',role,m.ui(role))
    assert ini.read_bytes()==ini_before,('Gameplay hotkey persisted a setting while typing',role)
    native_gameplay_unchanged(p,role)
    before=sample(p,role+'_chat_before_held_w_space_fire')
    assert before[slot]['weapon']==2 and not before[slot]['infinite'],('Suppression needs a normal Uzi with ammo consumption',role,before)
    try:
        devices(m,role,(26,44),True) # Raw W/Space plus normal virtual left mouse.
        m.delay(.70);during=sample(p,role+'_chat_during_held_w_space_fire')
    finally:devices(m,role)
    delta=math.dist(before[slot]['position'],during[slot]['position'])
    assert delta<.08,('Chat leaked held movement/jump into native gameplay',role,delta,before,during)
    assert before[slot]['ammo']==during[slot]['ammo'] and before[slot]['secondary']==during[slot]['secondary'],('Chat leaked FIRE into native gameplay',role,before,during)
    assert m.chat(role).get('draft')==draft and m.chat(role).get('active')
    key(p,role,ESCAPE);m.wait(lambda:not m.chat(role).get('active') and m.chat(role).get('draft')=='',8)
    wait_history(p,expected);native_gameplay_unchanged(p,role)
    assert ini.read_bytes()==ini_before
    record(p,'active_chat_blocks_devices_hotkeys_and_escape_does_not_pause',role=role,position_delta=delta)


def editor_limits_and_cancel(p,expected):
    open_chat(p)
    maximum='ä'*128;p.command('host','text '+maximum)
    p.m.wait(lambda:p.m.chat().get('draft')==maximum,8)
    p.command('host','text x');p.m.delay(.15)
    assert p.m.chat()['draft']==maximum and p.m.chat()['cursor']==256,('Editor split UTF-8 or exceeded256bytes',p.m.chat())
    key(p,'host',ESCAPE);p.m.wait(lambda:not p.m.chat().get('active') and not p.m.chat().get('draft'),8)
    wait_history(p,expected);native_gameplay_unchanged(p,'host')
    open_chat(p);key(p,'host',ENTER)
    p.m.wait(lambda:not p.m.chat().get('active'),8)
    wait_history(p,expected);native_gameplay_unchanged(p,'host')
    record(p,'utf8_editor_limit_cancel_and_empty_enter_do_not_send',bytes=256)


def controls_resume(p,expected):
    m=p.m;before=sample(p,'chat_closed_before_real_movement_and_fire')
    try:
        devices(m,'host',(26,),True);m.delay(.55)
        during=sample(p,'chat_closed_real_movement_and_fire')
    finally:devices(m,'host')
    delta=math.dist(before[0]['position'],during[0]['position'])
    assert delta>.20,('Normal movement failed after chat closed',delta,before,during)
    assert any(after<old for after,old in zip(during[0]['ammo'],before[0]['ammo'])),('Normal FIRE failed after chat closed',before,during)
    wait_history(p,expected);native_gameplay_unchanged(p,'host')
    record(p,'closed_chat_restores_actual_native_movement_and_ammunition_use',position_delta=delta)


def level_transition(p,expected):
    p.m.campaign_command('link Rlevel1_2')
    p.m.wait(lambda:all(p.m.performance(role).get('level')=='Rlevel1_2' and p.m.performance(role).get('local_ready') and p.m.performance(role).get('ready_players')==2 for role in ROLES),120)
    wait_history(p,expected)
    record(p,'session_chat_history_survives_native_worldlink_fixture',target='Rlevel1_2',scope='Authored WorldLink shortcut, not a normal full-level playthrough.')


def leave_and_reset(p):
    # The declared CLI bootstrap may return to the original profile overview
    # (9A), whereas a native-menu-created session returns to the main menu (3D).
    # Exercise both original Leave buttons without requiring either return path.
    for role in ('client','host'):
        if not p.m.state(role).get('active'):
            raise AssertionError(('Peer ended before its explicit mouse Leave',role))
        if p.m.state(role).get('scene')==4:
            if p.m.state(role).get('info'):
                own=ident(p.m.state(role)['local_slot'])
                p.m.wait(lambda:p.ack(role).get('active') and any(ident(surface.get('slot',999))==own for surface in p.m.information(role).get('surfaces',[])),12)
                initial=information_surface(p,role)
                body=information_controls(p,role,initial).get('info.body')
                assert body,('Next-level Info lacks its owned native mouse Back region',role,initial)
                before=p.observe(role).get('activations',0)
                p.click_uv(p.center(body),role,button='right')
                assert p.observe(role).get('activations',0)==before+1,('Info Back did not activate exactly once',role)
                p.m.wait(lambda:not p.m.state(role).get('info'),12)
                record(p,'next_level_information_mouse_back_before_chat_leave',role=role,slot=own,initial=initial)
            if not p.m.state(role).get('pause'):
                wait_information_retired(p,role,'next_level_information_retired_before_chat_leave')
            open_pause(p,role);connection(p,role);context='original_pause_connection'
        else:
            wait_lobby(p,role);p.click(0x153,role,root=0x9a);page(p,7,role)
            context='original_lobby_connection'
        p.click(0x14c,role)
        p.m.wait(lambda:not p.m.state(role).get('active') and p.m.state(role).get('scene')==3,30)
        p.m.wait(lambda:bool(p.observe(role).get('surfaces')),12)
        roots={ident(surface['root']) for surface in p.observe(role)['surfaces']}
        assert roots & {0x3d,0x9a},('CLI Leave did not restore an original main/profile menu surface',role,roots,p.m.diagnostics(role))
        p.m.wait(lambda:not p.m.chat(role).get('active') and p.m.chat(role).get('messages')==[],12)
        if role=='client':p.m.wait(lambda:p.m.state().get('players')==1,20)
        record(p,'cli_session_original_mouse_leave_clears_chat',role=role,context=context,
               state=public_menu(p.m.state(role)),native_roots=sorted(roots),chat=p.m.chat(role),
               scope='Declared CLI setup may restore original profile overview9A or main menu3D; no native host-creation return-path claim.')
        screenshot(p,role+'_chat_cleared_after_original_leave',role)
    for role in ROLES:
        key(p,role,T);p.m.delay(.15)
        assert not p.m.chat(role).get('active') and p.m.chat(role).get('messages')==[]
        assert p.m.state(role).get('scene')==3 and not p.m.state(role).get('active'),('T opened offline chat or changed original menu',role,p.m.diagnostics(role))
    record(p,'original_mouse_leave_clears_chat_and_t_does_not_open_offline',chat={role:p.m.chat(role) for role in ROLES})


def exercise(p):
    m=p.m;m.start_role('host',m.a.language);direct_gameplay_setup(p)
    m.wait(lambda:all('messages' in m.chat(role) for role in ROLES),12)
    assert all(m.chat(role)['messages']==[] and not m.chat(role).get('active') for role in ROLES)
    m.press('host','GODON') # Declared setup only; never a Chat UI action.
    select_uzis(p);m.delay(.5)
    record(p,'declared_god_all_weapons_setup_with_normal_ammunition',flags=5,weapon_selection='normal number4',kbm_player=m.a.kbm_player)
    expected=[]
    send(p,'host','Grüße vom Host: 100% ###literal',expected);screenshot(p,'host_message_in_hud')
    send(p,'client','Bonjour du client, à bientôt! 100% ###',expected);screenshot(p,'client_reply_in_hud','client')
    editor_limits_and_cancel(p,expected)
    for role in ROLES:isolation(p,role,expected)
    controls_resume(p,expected)
    if m.a.level_change:level_transition(p,expected)
    if not m.a.skip_leave:leave_and_reset(p)
    record(p,'chat_ui_acceptance_finished',messages=expected,level_change=m.a.level_change,explicit_leave=not m.a.skip_leave)


def arguments():
    parser=argparse.ArgumentParser(description=__doc__)
    for key_name in ('exe','data','card','png','output'):parser.add_argument('--'+key_name,type=Path,required=True)
    parser.add_argument('--port',type=int,default=28840)
    parser.add_argument('--kbm-player',type=int,choices=(1,2,3,4),default=2)
    parser.add_argument('--level-change',action='store_true')
    parser.add_argument('--skip-leave',action='store_true')
    a=parser.parse_args()
    for key_name in ('exe','data','card','png','output'):setattr(a,key_name,getattr(a,key_name).resolve())
    assert all(str(getattr(a,key_name)).isascii() for key_name in ('exe','data','card','png','output')),'Use private ASCII test paths'
    assert a.exe.is_file() and a.card.is_file() and a.png.is_file() and (a.data/'sys/main.dol').is_file()
    assert 1024<a.port<65528
    a.case='host-client-direct';a.language=a.client_language=1;a.fps=60;a.allow_start_key_fallback=False
    a.test_env={'SSNE_TEST_CHAT':'1','SSNE_TEST_DEVICE_KEY_FILE':KEY_FILE,'SSNE_COOP_CAMPAIGN_VERIFY':'1','SSNE_NETPLAY_MOD_FLAGS':'5'}
    a.private_settings='[graphics]\nrenderer=gpu\nscale=1\naspect=3\nhud_edges=1\nside_fill=1\nvsync=0\nwindow_w=1100\nwindow_h=850\ntop_bar=0\nfullscreen=0\n[audio]\nmuted=1\n[general]\npause_unfocused=0\npause_in_menu=0\nskip_intros=1\ngame_language=1\nkbm_player='+str(a.kbm_player)+'\n'
    return a


def main():
    a=arguments();a.output.mkdir(parents=True,exist_ok=False);before=source_state(a)
    report={'passed':False,'scope':__doc__,'steps':[],'exe_sha256':before['exe'],'driver_sha256':sha(Path(__file__)),'kbm_player':a.kbm_player}
    m=ChatMenus(a,'Rlevel1_1',a.output/'host-client-direct');p=CrosshairPointer(m,report)
    try:exercise(p);report['passed']=True
    except Exception:
        report['error']=traceback.format_exc();print(report['error'],flush=True)
        report['failure_diagnostics']={role:{**m.diagnostics(role),'chat':m.chat(role)} for role in m.processes}
    finally:
        for role,child in m.processes.items():
            if child.poll() is None:
                try:
                    devices(m,role)
                    if m.chat(role).get('active'):key(p,role,ESCAPE)
                except Exception:pass
        m.close()
        report.update(visible=m.visible,focused=m.focused,launches=[{'role':entry['role'],'pid':entry['process'].pid,'exit':entry['process'].returncode} for entry in m.launches],final={role:{**m.diagnostics(role),'chat':m.chat(role)} for role in m.processes})
        after=source_state(a);report['source_files_unchanged']={name:before[name]==after[name] for name in before}
        report['passed'] &= all(report['source_files_unchanged'].values()) and bool(m.launches) and not m.visible and not m.focused and all(entry['process'].returncode==0 for entry in m.launches)
        (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
        print(json.dumps({name:report[name] for name in ('passed','source_files_unchanged')},indent=2),flush=True)
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
