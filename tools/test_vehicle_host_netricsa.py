"""Check the first Roman Jeep with two local PAL peers and the host briefing open."""
import argparse
import json
import math
from pathlib import Path
import struct
import time

from test_vehicle_input import AttachedMenus, VehiclePeers, occupied


class BriefingPeers(VehiclePeers):
    def wait(self, condition, timeout=60):
        # Generic campaign waits periodically press Back to advance results.
        # Any such input here would close the host's briefing.
        deadline = time.monotonic() + timeout
        while not condition():
            assert all(p.poll() is None for p in self.processes.values()), "Test game exited while waiting"
            if time.monotonic() > deadline:
                raise TimeoutError("Native campaign state did not become ready")
            self.delay(.12)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "data", "card", "output"):
        parser.add_argument("--" + name, required=True, type=lambda path: Path(path).resolve())
    parser.add_argument("--port", type=int, default=28648)
    parser.add_argument("--vehicle-ref", type=int, choices=(166, 32767), default=166)
    args = parser.parse_args()
    args.level = "Rlevel1_3"
    args.fps = 60
    args.output.mkdir(parents=True, exist_ok=False)
    peers = BriefingPeers(args)
    menus = AttachedMenus(peers)
    samples = []

    def tap_client(scancode):
        peers.hold("client", keys=[scancode])
        peers.delay(.22)
        peers.hold("client")
        peers.delay(.45)

    def sample(label):
        result = peers.dump(label, actors, wrapper)
        with (peers.folder / "host/campaign-capture.ram").open("rb") as ram:
            ram.seek(result["car"] - 0x80000000 + 0x20)
            result["car_position"] = struct.unpack(">fff", ram.read(12))
            ram.seek(actors[1] - 0x80000000 + 0x20)
            result["client_position"] = struct.unpack(">fff", ram.read(12))
            ram.seek(actors[1] - 0x80000000 + 0x6BC)
            result["client_use_target"] = struct.unpack(">I", ram.read(4))[0]
            for name, offset in (("trigger_activations", 0x94), ("trigger_ran", 0xA4)):
                ram.seek(trigger - 0x80000000 + offset)
                result[name] = struct.unpack(">I", ram.read(4))[0]
        result["host_info"] = menus.state("host").get("info")
        samples.append(result)
        assert result["host_info"] == 1, "Host briefing closed during vehicle test"
        return result

    try:
        peers.start()
        peers.wait(lambda: peers.ready(args.level) and menus.state("host").get("info") == 1 and
                   peers.state().get("level") == args.level, 95)
        state = peers.state()
        if menus.state("client").get("info"):
            tap_client(43)  # Tab closes only the client's briefing.
        deadline = time.monotonic() + 12
        while menus.state("client").get("info") or not peers.input_state("client").get("player_control"):
            assert time.monotonic() < deadline, "Client did not return to gameplay"
            peers.delay(.1)
        actors = {o["slot"]: o["address"] for o in state["objects"]
                  if o.get("vtable") == 0x802121E8 and o.get("slot") in (0, 1)}
        deadline = time.monotonic() + 30
        while True:
            peers.action("dump")
            with (peers.folder / "host/campaign-capture.ram").open("rb") as ram:
                ram.seek(0x2DB7C4)
                table, count = struct.unpack(">II", ram.read(8))
                assert count <= 32768
                ram.seek(table - 0x80000000)
                refs = dict(struct.iter_unpack(">H2xI", ram.read(count * 8)))
            if args.vehicle_ref in refs:
                break
            assert time.monotonic() < deadline, f"Vehicle {args.vehicle_ref} missing in native object registry"
            peers.delay(.5)
        wrapper, trigger = refs[args.vehicle_ref], refs[180]
        initial = sample("initial")
        assert not initial["driver"]
        tap_client(8)
        assert not sample("use_from_spawn")["driver"], "Use reached a distant Jeep"
        x, y, z = initial["car_position"]
        # Walk the same approach on an empty parallel lane and straight into
        # the parked vehicle. Measure host physics independently of E/Use or
        # client-side graphics. Both paths start at the same ground height.
        def walk(label, lane):
            peers.action(f"player_place 1 {x + lane} {y + 3} {z - 9}")
            peers.delay(.55)
            before = sample(label + "_start")
            peers.hold("client", keys=[26])
            peers.delay(1.7)
            peers.hold("client")
            peers.delay(.3)
            after = sample(label + "_end")
            assert not before["driver"] and not after["driver"]
            assert after["players"]["1"]["movement_mode"] != 3
            assert abs(after["client_position"][0] - before["client_position"][0]) < 1.5
            return before, after

        free_start, free_end = walk("on_foot_free_lane", 12)
        approach, stopped = walk("on_foot_vehicle_lane", 0)
        assert free_end["client_position"][2] > z + 3, "Free control lane failed to pass the far side"
        assert approach["client_position"][2] + 1 < stopped["client_position"][2] < stopped["car_position"][2] - 1.5, (
            "Client walked through the parked vehicle", approach["client_position"], stopped["client_position"])
        # The only gameplay shortcut is placement outside the original Jeep,
        # facing its body. The native trigger, raycast, and Enter/Exit all run.
        peers.action(f"player_place 1 {x} {y + 3} {z - 6}")
        peers.delay(.8)
        assert sample("in_front")["client_use_target"] == wrapper, "Jeep not targetable with host briefing open"
        tap_client(8)  # E
        before = sample("entered")
        assert occupied(before, 1, actors) and before["input"]["client"]["vehicle_control"]
        peers.hold("client", keys=[26])  # W
        peers.delay(3)
        peers.hold("client")
        moving = sample("driven")
        assert occupied(moving, 1, actors) and math.dist(before["car_position"], moving["car_position"]) > 2
        tap_client(8)
        after = sample("exited")
        assert not after["driver"] and not after["players"]["1"]["vehicle"]
        assert initial["trigger_ran"] == 0 and after["trigger_ran"] == 1, "Authored first-entry action did not run"
    finally:
        peers.close()
        (args.output / "observations.json").write_text(json.dumps(samples, indent=2), encoding="utf-8")
    assert all(p.returncode == 0 for p in peers.processes.values())
    print("PASS: client walked into the Jeep, stopped at its body, entered, drove, and exited with the host briefing open.")


if __name__ == "__main__":
    main()
