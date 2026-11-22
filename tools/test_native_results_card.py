"""Exercise original world exit with successful or denied native memory-card writes."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import struct
import time
import traceback
from test_native_cave_gate import CavePeers
from test_coop_campaign import sha


def profile_payload(card):
    data = card.read_bytes()
    block = 8192
    def u16(at):
        return struct.unpack_from(">H", data, at)[0]
    # Ring counters wrap; the two on-card copies generally differ by one commit.
    def newer(a,b,field):
        av,bv = u16(a*block+field),u16(b*block+field)
        return a if (av-bv)&65535 < 32768 else b
    directory = newer(1,2,block-6)*block
    bat = newer(3,4,4)*block
    for begin,end,stored in ((directory,directory+block-4,directory+block-4),
                             (bat+4,bat+block,bat)):
        words=struct.unpack_from(">"+str((end-begin)//2)+"H",data,begin)
        checksum=sum(words)&65535
        inverse=sum(65535-v for v in words)&65535
        assert (checksum%65535,inverse%65535)==(u16(stored),u16(stored+2)), "Invalid card metadata"
    for index in range(127):
        at = directory+index*64
        if data[at:at+6] != b"G3BP9G" or data[at+8:at+40].split(b"\0")[0] != b"SeriousSam":
            continue
        sector = u16(at+54)
        chunks = []
        for _ in range(u16(at+56)):
            assert 5 <= sector < len(data)//block, sector
            chunks.append(data[sector*block:(sector+1)*block])
            sector=u16(bat+10+(sector-5)*2)
        assert sector==65535
        return hashlib.sha256(b"".join(chunks)).hexdigest()
    raise AssertionError("No committed PAL SeriousSam save on active card directory")


def hold_card(card,share):
    create=ctypes.windll.kernel32.CreateFileW
    create.argtypes=(wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE)
    create.restype=wintypes.HANDLE
    h=create(str(card),0x80000000,share,None,3,128,None)
    if h==ctypes.c_void_p(-1).value: raise ctypes.WinError()
    return h


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ("exe","data","card","output"):
        parser.add_argument("--"+key,type=Path,required=True)
    parser.add_argument("--case", choices=("normal","locked","locked-publish","locked-replace"),required=True)
    parser.add_argument("--port",type=int,required=True)
    parser.add_argument("--fps",type=int,default=60)
    a=parser.parse_args()
    for k in ("exe","data","card","output"):setattr(a,k,getattr(a,k).resolve())
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.bots=0;a.mode=1;a.capacity=8;a.open_menu=False;a.mod_flags=0
    a.test_env={"SSNE_TEST_DEVICE_INPUT":"1","SSNE_DEV_NATIVE_MENU":"1","SSNE_COOP_TEST_CARD_OBSERVE":"1"}
    if a.case == "locked-publish":
        a.test_env.update(SSNE_TEST_CARD_LOCK_OFFSET="8192", SSNE_TEST_CARD_LOCK_BYTES="128",
                          SSNE_TEST_CARD_LOCK_OCCURRENCE="3", SSNE_TEST_CARD_LOCK_MS="1400")
    a.client_test_env={"SSNE_TEST_DEVICE_INPUT":"1"}
    close=ctypes.windll.kernel32.CloseHandle
    close.argtypes=(wintypes.HANDLE,)
    peers=CavePeers(a,"Rlevel1_1",a.output)
    report={"case":a.case,"game_sha":sha(a.exe),"passed":False}
    lock=None
    try:
        peers.start();peers.initial_ready(("host",));peers.start_role("client");peers.initial_ready(("host","client"))
        peers.wait(lambda:all(peers.performance(role).get("ready_players")==2 for role in ("host","client")),60)
        private=a.output/"host/card.raw"
        before_raw=sha(private)
        before=profile_payload(private)
        assert before==profile_payload(a.card)
        peers.command("link Rlevel1_2")
        peers.wait(lambda:"[coop-client-native-results]" in peers.log("client") and "[coop-results-setup]" in peers.log("host"),35)
        start=time.monotonic()
        if a.case in ("locked","locked-replace"):
            lock=hold_card(private,1 if a.case=="locked" else 3)
            peers.wait(lambda:"[coop-results-save-failed]" in peers.log("host") or
                       (a.case=="locked" and "aborted=1" in peers.log("host")),32)
            close(lock);lock=None
        peers.wait(lambda:all(peers.performance(role).get("level")=="Rlevel1_2" and peers.performance(role).get("local_ready") and peers.performance(role).get("ready_players")==2 for role in ("host","client")),45)
        host=peers.log("host")
        assert host.count("[coop-native-progress-save-request]")==1
        after=profile_payload(private)
        report.update(before_payload=before,after_payload=after,before_raw=before_raw,
                      after_raw=sha(private),seconds_to_next=round(time.monotonic()-start,2))
        if a.case!="normal":
            assert before==after, "previous committed payload was modified on failure"
            assert before_raw==sha(private), "failure modified the card image"
            assert host.count("[coop-results-save-failed]")==1
            notices=("The session will continue", "Die Sitzung geht weiter", "La partie continue")
            assert any(text in host for text in notices),"Missing nonblocking save failure notice"
            if a.case == "locked-publish":
                assert "address=2148558456 channel=0 code=-5" in host, "Final directory callback did not fail"
            def next_level():
                state=peers.state()
                if state.get("level")=="Rlevel1_2":
                    return next((o["target"] for o in state.get("objects",[]) if o.get("target") and not o.get("terminal")),None)
            peers.wait(next_level,15)
            target=next_level()
            peers.command("link "+target)
            peers.wait(lambda:all(peers.performance(role).get("level")==target and
                                  peers.performance(role).get("local_ready") and
                                  peers.performance(role).get("ready_players")==2
                                  for role in ("host","client")),50)
            report["retry_payload"]=profile_payload(private)
            report["retry_level"]=target
            final_log=peers.log("host")
            assert final_log.count("[coop-native-progress-save-request]")==2,"Next level never requested a save"
            assert report["retry_payload"]!=before and final_log.count("[coop-results-save-failed]")==1
        else:
            assert before!=after and "[coop-results-save-failed]" not in host
        report["passed"]=True
    except Exception:
        report["error"]=traceback.format_exc()
    finally:
        if lock:close(lock)
        peers.close()
        report["exit_codes"]={s:p.returncode for s,p in peers.processes.items()}
        report["passed"] &= all(c==0 for c in report["exit_codes"].values())
        (a.output/"result.json").write_text(json.dumps(report,indent=2),encoding="utf8")
        print(json.dumps(report),flush=True)
    return 0 if report["passed"] else 1


if __name__=="__main__":raise SystemExit(main())

