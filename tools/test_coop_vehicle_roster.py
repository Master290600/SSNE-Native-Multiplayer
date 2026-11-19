"""Exercise a campaign vehicle point with real peers joining and leaving a PAL host."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

from test_coop_campaign import Peers, sha


class VehicleRosterPeers(Peers):
    def wait_for_count(self, number):
        def ready():
            for role in self.processes:
                sample = self.performance(role)
                if sample.get("level") != self.level or not sample.get("local_ready"):
                    return False
                if sample.get("ready_players") != number:
                    return False
            return self.state().get("level") == self.level
        self.wait(ready, 90)
        self.delay(1.8)

    def join_extra(self, role):
        folder = self.folder / role
        folder.mkdir()
        (folder / "steps").mkdir()
        executable = folder / self.a.exe.name
        shutil.copy2(self.a.exe, executable)
        shutil.copy2(self.a.card, folder / "card.raw")
        shutil.copy2(self.folder / "host/settings-netplay.ini", folder / "settings-netplay.ini")
        (folder / "input.txt").write_text("")
        env = {k: v for k, v in os.environ.items() if not k.startswith("SSNE_")}
        env.update(SSNE_TEST_HIDDEN="1", SSNE_PAD_SCRIPT_FILE=str(folder / "input.txt"),
                   SSNE_PERF_LOG=str(folder / "performance.jsonl"), SSNE_COOP_CAMPAIGN_VERIFY="1",
                   SSNE_COOP_TEST_SKIP_DELAY_MS="1500", SSNE_TEST_SKIP_FIRST_VOTE="1")
        args = [str(executable), "--data", str(self.a.data), "--memcard", str(folder / "card.raw"),
                "--lan-port", str(self.a.port), "--lan-pin", "123456", "--player-name", role,
                "--mute", "--coop-join", "127.0.0.1"]
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        stream = (folder / "console.log").open("wb")
        self.streams.append(stream)
        self.processes[role] = subprocess.Popen(args, cwd=folder, env=env, startupinfo=startup,
            stdout=stream, stderr=stream, creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)

    def leave(self, role):
        self.pulse("QUIT", (role,))
        assert self.processes[role].wait(timeout=10) == 0
        del self.processes[role]

    def host_command(self, action):
        self.command(action)
        expected = f"sequence={self.command_sequence} action={action.split()[0]}"
        self.wait(lambda: expected in self.log("host"), 30)

    def vehicles(self):
        self.host_command("dump")
        ram = (self.folder / "host/campaign-capture.ram").read_bytes()
        def word(address):
            return struct.unpack_from(">I", ram, address - 0x80000000)[0]
        def point(address):
            return struct.unpack_from(">fff", ram, address - 0x80000000)
        table, count, capacity = struct.unpack_from(">III", ram, 0x2DB7C4)
        assert count <= capacity <= 32768
        result = {}
        for i in range(count):
            wrapper = word(table + 8*i + 4)
            if not 0x80000000 <= wrapper <= 0x80000000 + len(ram) - 0x94:
                continue
            if word(wrapper + 0x48) != 0x80209BB8:
                continue
            ref, car = word(wrapper + 0x50), word(wrapper + 0x60)
            assert ref not in result
            result[ref] = {"visible": bool(word(wrapper + 0x7C) and car),
                           "driver": word(car + 0x160) if car else 0,
                           "position": point(car + 0x20) if car else None}
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("exe", "data", "card", "output"):
        parser.add_argument("--" + key, type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument("--level", default="Rlevel1_3")
    parser.add_argument("--port", type=int, default=32641)
    a = parser.parse_args()
    a.output.mkdir(exist_ok=False, parents=True)
    original = sha(a.card)
    peers = VehicleRosterPeers(a, a.level, a.output / "peers")
    report = {"level": a.level, "exe_sha256": sha(a.exe), "samples": {}, "passed": False}

    def snapshot(label, expected):
        deadline = time.monotonic() + 12
        while True:
            vehicles = peers.vehicles()
            if sum(row["visible"] for row in vehicles.values()) == expected:
                report["samples"][label] = vehicles
                return vehicles
            if time.monotonic() >= deadline:
                raise AssertionError((label, expected, vehicles))
            peers.delay(.5)

    try:
        peers.start()
        peers.wait_for_count(2)
        authored = {ref: row for ref, row in peers.vehicles().items() if ref < 32761}
        assert len(authored) == 1, "This test needs one authored vehicle"
        reference, vehicle = next(iter(authored.items()))
        if not vehicle["visible"]:
            peers.host_command(f"event {reference} 11")
        x, y, z = peers.vehicles()[reference]["position"]
        peers.host_command(f"player_place 1 {x} {y+1} {z-12}")
        snapshot("two", 2)
        peers.join_extra("third")
        peers.wait_for_count(3)
        three = snapshot("three", 3)
        peers.delay(.6)
        assert peers.vehicles().keys() == three.keys()
        peers.leave("third")
        peers.wait_for_count(2)
        snapshot("after_departure", 2)
        peers.join_extra("returned")
        peers.wait_for_count(3)
        again = snapshot("after_rejoin", 3)
        assert again.keys() == three.keys(), "Rejoining allocated an extra vehicle twice"
        peers.leave("returned")
        peers.leave("client")
        peers.wait_for_count(1)
        solo = snapshot("host_alone", 1)
        assert solo[reference]["visible"]
        for role in ("client", "third", "returned"):
            log = (peers.folder / role / "ssne-coop-client.log").read_text(errors="replace")
            assert "[port] Stopped:" not in log and "[campaign-field-difference]" not in log
        report["passed"] = True
    finally:
        peers.close()
        assert original == sha(a.card)
        (a.output / "result.json").write_text(json.dumps(report, indent=2))
    print("Verified vehicle counts: 2, 3, 2, 3, 1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

