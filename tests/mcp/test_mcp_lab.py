#!/usr/bin/env python3
"""Drives veach-lab purely through MCP over stdio (docs/specs/mcp-server/spec.md).

  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> protocol
  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> --imgdiff <veach> --golden <png> --out <png> golden
  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> fly
  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> reload
  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> --imgdiff <veach> --golden <png> --out <dir> --rhi metal photo_mode
  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> --rhi metal gpu_timings

`protocol`, `fly` (the input spec's fly camera, input.AC5) and `reload` (hot reload, reload.AC5) run on the
null RHI anywhere. `golden` and `photo_mode` need Metal 3 (or DX12) with ray tracing; on other GPUs they print
VEACH_GPU_UNAVAILABLE (CTest: skipped) unless VEACH_REQUIRE_GPU=1, which makes that a failure.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import math
import pathlib
import re
import subprocess
import sys
import tempfile
import struct
import threading
import zlib


class McpClient:
    def __init__(self, command: list[str]):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, encoding="utf-8")
        self.stderr: list[str] = []
        threading.Thread(target=lambda: self.stderr.extend(self.proc.stderr), daemon=True).start()
        self.next_id = 0

    def request(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}}
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("the server exited:\n" + "".join(self.stderr))
        response = json.loads(line)
        assert response["id"] == self.next_id, response
        if "error" in response:
            raise RuntimeError(f"protocol error: {response['error']}")
        return response["result"]

    def notify(self, method: str) -> None:
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def call(self, tool: str, **arguments) -> dict:
        return self.request("tools/call", {"name": tool, "arguments": arguments})

    def call_json(self, tool: str, **arguments):
        result = self.call(tool, **arguments)
        assert not result.get("isError"), result
        return json.loads(result["content"][0]["text"])

    def initialize(self) -> dict:
        result = self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                             "clientInfo": {"name": "veach-tests", "version": "1"}})
        self.notify("notifications/initialized")
        return result

    def close(self) -> int:
        self.proc.stdin.close()
        return self.proc.wait(timeout=60)


def check(condition: bool, what: str) -> None:
    if not condition:
        raise AssertionError(what)
    print(f"ok: {what}")


def protocol(lab: str) -> None:
    client = McpClient([lab, "--mcp", "--headless"])
    info = client.initialize()
    check(info["protocolVersion"] == "2025-06-18", "[mcp.AC1] initialize negotiates the client's protocol version")

    tools = {t["name"]: t for t in client.request("tools/list")["tools"]}
    check({"query_entities", "get_component", "set_component", "spawn_entity", "step_frames", "screenshot",
           "render_graph_plan", "gpu_timings", "eval"} <= tools.keys(), "[mcp.AC1] all v0 tools are listed")
    names = tools["get_component"]["inputSchema"]["properties"]["component"]["enum"]
    check({"Window", "TriangleDemo", "Greeting"} <= set(names),
          "[mcp.AC2] component names come from reflection (engine, lab module and the hello.luau script component)")
    window_branch = [b for b in tools["set_component"]["inputSchema"]["allOf"]
                     if b["if"]["properties"]["component"]["const"] == "Window"][0]
    check("title" in window_branch["then"]["properties"]["value"]["properties"],
          "[mcp.AC2] set_component's value schema for Window is generated from reflection")

    windows = client.call_json("query_entities", **{"with": ["Window"]})
    check(any(e["name"] == "primary_window" for e in windows), "[mcp.AC3] query_entities finds the primary window")
    check(client.call_json("get_component", entity="primary_window", component="Window")["title"] == "Veach Lab",
          "[mcp.AC3] get_component reads the Window component")
    client.call_json("set_component", entity="primary_window", component="Window", value={"title": "Driven by MCP"})
    window = client.call_json("get_component", entity="primary_window", component="Window")
    check(window["title"] == "Driven by MCP" and window["width"] == 1280,
          "[mcp.AC3] set_component merges the given fields and keeps the rest")

    typo = client.call("get_component", entity="primary_window", component="Windw")
    text = typo["content"][0]["text"]
    check(typo.get("isError") and "Did you mean 'Window'?" in text and "[ecs.unknown_component]" in text,
          "[mcp.AC4] errors carry the error code and a did-you-mean hint")

    spawned = client.call_json("spawn_entity", name="probe", components={"Greeting": {"text": "via MCP"}})
    check("Greeting" in spawned["components"], "[mcp.AC5] spawn_entity creates an entity with a script-defined component")

    stepped = client.call_json("step_frames", count=3)
    check(stepped["frame"] == 3 and stepped["fixed_step"] == 3, "[mcp.AC6] step_frames advances exactly N frames")

    evaluated = client.call_json("eval", source='local veach = require("@veach")\n'
                                                'veach.log.info("from eval")\nreturn veach.time.frame() * 2')
    check(evaluated["returns"] == ["6"] and "[info] from eval" in evaluated["log"],
          "[mcp.AC7] eval returns values and the log lines it produced")
    failed = client.call("eval", source='local veach = require("@veach")\nveach.spawn("ghost")\nerror("boom")')
    check(failed.get("isError"), "[mcp.AC7] a failing eval is reported as an error")
    check(not client.call_json("query_entities", name_contains="ghost"), "[mcp.AC7] a failing eval applies nothing")

    check("clear" in client.call("render_graph_plan")["content"][0]["text"], "[mcp.AC8] render_graph_plan returns the plan")
    timings = client.call_json("gpu_timings")
    check(timings["backend"] == "null" and timings["frames_in_flight"] == 2, "[mcp.AC8] gpu_timings reports the backend")
    check(timings["passes"] == [] and timings["passes_unavailable"],
          "[rt.AC5] without a GPU, gpu_timings says why there are no per-pass times")
    module_tools(client, tools, info)
    check(client.close() == 0, "[mcp.AC10] the server exits 0 when the client disconnects")


def module_tools(client: McpClient, tools: dict, info: dict) -> None:
    """The space toolbox's tools, registered by a module the lab imports (VCH-100)."""
    names = list(tools)
    check(names[-2:] == ["orrery_state", "generate_system"] and "orrery_state" in info["instructions"],
          "[mcp.AC12] module tools are listed after the built-in ones and named in the instructions")
    check(tools["generate_system"]["inputSchema"]["required"] == ["seed", "address"],
          "[mcp.AC12] a module tool's input schema is the one it registered")

    au = 149597870700.0
    sol = client.call_json("orrery_state", system="sol", tick="378683424000", body="earth",
                           latitude=48.8566, longitude=2.3522)
    earth = sol["bodies"][0]
    check(sol["tick"] == "378683424000" and abs(earth["distance_to_star"] / au - 0.98332) < 1e-5,
          "[mcp.AC13] orrery_state: the Earth is 0.98332 AU from the Sun at J2000.0, in one call")
    check(abs(sol["sun"]["elevation"] - 18.09) < 0.01 and abs(sol["sun"]["local_solar_time"] - 12.10) < 0.01,
          "[mcp.AC13] orrery_state: the sun stands 18.09 degrees over Paris at 12:06 local solar time")
    planet = client.call_json("orrery_state", system="earthlike")["bodies"][1]
    check(planet["orbit"]["phase"] == 0.0 and abs(planet["spin"]["local_solar_time_at_prime_meridian"] - 12.0) < 1e-9,
          "[mcp.AC13] orrery_state: the Earth-like planet's orbit phase and local solar time at tick 0")
    typo = client.call("orrery_state", system="sol", body="eart")
    check(typo.get("isError") and "Did you mean 'earth'?" in typo["content"][0]["text"],
          "[mcp.AC13] a module tool's errors carry the code and a did-you-mean hint")

    system = client.call_json("generate_system", seed="42", address="S:3f2a91c07e5b0000")
    check(system["stages"]["space:system"]["hash"] == "2733265849347982535" and
          system["stages"]["space:body.physics"]["hash"] == "9754287468050259825",
          "[mcp.AC14] generate_system: SP-05's stage hashes for seed 42, S:3f2a91c07e5b0000 (64-bit as strings)")
    check(system["planet_count"] == 4 and system["bodies"][0]["address"] == "S:3f2a91c07e5b0000/B:1" and
          system["violations"] == [], "[mcp.AC14] generate_system: the records, valid")


def fly(lab: str) -> None:
    client = McpClient([lab, "--mcp", "--headless", "--script", "scripts/triangle_fly.luau"])
    client.initialize()

    def translation() -> list[float]:
        return client.call_json("get_component", entity="camera", component="Transform")["translation"]

    start = translation()
    client.call_json("send_input", key="w", down=True)
    client.call_json("step_frames", count=30)
    client.call_json("send_input", key="w", down=False)
    moved = translation()
    check(abs(moved[2] - (start[2] - 1.0)) < 1e-3 and abs(moved[0] - start[0]) < 1e-6,
          f"[input.AC5] holding w for 30 frames flies 1 m forward ({start} -> {moved})")

    client.call_json("send_input", key="left", down=True)
    client.call_json("step_frames", count=30)
    rotation = client.call_json("get_component", entity="camera", component="Transform")["rotation"]
    yaw = 2 * math.atan2(rotation["y"], rotation["w"])
    check(abs(yaw - 0.75) < 1e-3 and abs(rotation["x"]) < 1e-6 and abs(rotation["z"]) < 1e-6,
          f"[input.AC5] holding left for 30 frames turns 0.75 rad (rotation {rotation})")

    bad = client.call("send_input", key="spcae", down=True)
    text = bad["content"][0]["text"]
    check(bad.get("isError") and "[input.unknown_key]" in text and "Did you mean 'space'?" in text,
          "[input.AC5] send_input rejects an unknown key with a suggestion")
    check(client.close() == 0, "[mcp.AC10] the server exits 0 when the client disconnects")

    # input.AC6: the dev fly camera flies any scene's active camera, and writes nothing without input. Here a
    # turned camera (30 degrees about +Y), so forward is not -Z.
    client = McpClient([lab, "--mcp", "--headless", "--script", "scripts/triangle_camera_raster.luau"])
    client.initialize()
    start = client.call_json("get_component", entity="camera", component="Transform")
    client.call_json("step_frames", count=30)
    check(client.call_json("get_component", entity="camera", component="Transform") == start,
          "[input.AC6] without input the dev fly camera leaves the scene's camera exactly where it was")
    client.call_json("send_input", key="w", down=True)
    client.call_json("step_frames", count=30)
    client.call_json("send_input", key="w", down=False)
    moved = client.call_json("get_component", entity="camera", component="Transform")
    s, m = start["translation"], moved["translation"]
    step = [m[i] - s[i] for i in range(3)]
    expected = [-math.sin(math.radians(30)), 0.0, -math.cos(math.radians(30))]  # 1 m along the camera's forward
    check(all(abs(step[i] - expected[i]) < 1e-3 for i in range(3)),
          f"[input.AC6] holding w for 30 frames flies 1 m along the camera's own forward ({s} -> {m})")
    check(client.close() == 0, "[mcp.AC10] the server exits 0 when the client disconnects")


def replay(lab: str) -> None:
    """Record and replay (docs/specs/record-replay/spec.md)."""
    with tempfile.TemporaryDirectory() as tmp:
        recording = os.path.join(tmp, "fly.vreplay")
        client = McpClient([lab, "--mcp", "--headless", "--script", "scripts/triangle_fly.luau", "--record", recording])
        client.initialize()
        client.call_json("send_input", key="w", down=True)
        client.call_json("step_frames", count=20)
        client.call_json("send_input", key="w", down=False)
        client.call_json("send_input", key="left", down=True)
        client.call_json("step_frames", count=15)
        client.call_json("send_input", key="left", down=False)
        client.call_json("step_frames", count=5)
        recorded = client.call_json("get_component", entity="camera", component="Transform")
        check(client.close() == 0, "[replay.AC1] the recording run exits 0")
        with open(recording, encoding="utf-8") as f:
            lines = f.read().splitlines()
        check(len(lines) == 41 and json.loads(lines[0])["script"] == "scripts/triangle_fly.luau",
              f"[replay.AC1] a header and one line per frame ({len(lines)} lines)")

        client = McpClient([lab, "--mcp", "--headless", "--replay", recording])
        client.initialize()
        client.call_json("step_frames", count=40)
        replayed = client.call_json("get_component", entity="camera", component="Transform")
        check(replayed == recorded, f"[replay.AC1] the replay ends where the recording did ({replayed} vs {recorded})")
        check(client.close() == 0, "[replay.AC1] every frame's state matches (no replay.diverged)")

        run = subprocess.run([lab, "--headless", "--replay", recording, "--seed", "7"], capture_output=True,
                             text=True)
        check(run.returncode != 0 and "[replay.invalid]" in run.stdout + run.stderr,
              "[replay.AC2] a different seed is replay.invalid before the first frame")

        first = json.loads(lines[1])
        first["events"] = []  # the recorded 'w' press, dropped
        tampered = os.path.join(tmp, "tampered.vreplay")
        with open(tampered, "w", encoding="utf-8") as f:
            f.write("\n".join([lines[0], json.dumps(first)] + lines[2:]) + "\n")
        run = subprocess.run([lab, "--headless", "--replay", tampered], capture_output=True, text=True)
        out = run.stdout + run.stderr
        check(run.returncode == 1 and "[replay.diverged]" in out and "at frame 1" in out and
              re.search(r"first in \S*(FlyCamera|Transform|InputAction|Input)", out) is not None,
              f"[replay.AC3] a replay that differs stops at the first wrong frame, naming the component: {out[-400:]}")

        # replay.AC5: MCP world edits are part of the recording: set_component, spawn_entity and eval between frames.
        edited = os.path.join(tmp, "edits.vreplay")
        client = McpClient([lab, "--mcp", "--headless", "--script", "scripts/triangle_fly.luau", "--record", edited])
        client.initialize()
        client.call_json("step_frames", count=3)
        client.call_json("set_component", entity="camera", component="Transform",
                         value={"translation": [0.5, 0.25, 2.0]})
        client.call_json("spawn_entity", name="marker", components={"Transform": {"translation": [1, 2, 3]}})
        client.call_json("step_frames", count=3)
        client.call_json("eval", source="local veach = require('@veach')\n"
                                        "veach.spawn('from eval'):set(veach.components.Transform, "
                                        "{ translation = vector.create(4, 5, 6), rotation = veach.quat.identity(), "
                                        "scale = vector.one })")
        client.call_json("send_input", key="w", down=True)
        client.call_json("step_frames", count=10)
        recorded = [client.call_json("get_component", entity=name, component="Transform")
                    for name in ("camera", "marker", "from eval")]
        check(client.close() == 0, "[replay.AC5] the recording with edits exits 0")
        with open(edited, encoding="utf-8") as f:
            edit_lines = [json.loads(line) for line in f.read().splitlines()[1:]]
        tools = [e["tool"] for line in edit_lines for e in line.get("edits", [])]
        check(tools == ["set_component", "spawn_entity", "eval"], f"[replay.AC5] the edits are recorded: {tools}")
        client = McpClient([lab, "--mcp", "--headless", "--replay", edited])
        client.initialize()
        client.call_json("step_frames", count=16)
        replayed = [client.call_json("get_component", entity=name, component="Transform")
                    for name in ("camera", "marker", "from eval")]
        check(replayed == recorded, "[replay.AC5] the replay repeats the edits: the same camera and entities")
        check(client.close() == 0, "[replay.AC5] every frame's state matches (no replay.diverged)")


def replay_windowed(lab: str) -> None:
    """[replay.AC4] A windowed recording (frames of varying fixed steps) replays headless to the same state."""
    with tempfile.TemporaryDirectory() as tmp:
        recording = os.path.join(tmp, "windowed.vreplay")
        run = subprocess.run([lab, "--window", "--frames", "90", "--record", recording], capture_output=True,
                             text=True)
        out = run.stdout + run.stderr
        if run.returncode != 0 and ("platform" in out and "window" in out.lower()):
            print("VEACH_WINDOW_UNAVAILABLE: skipped (no window on this machine)")
            return
        check(run.returncode == 0, f"[replay.AC4] the windowed recording runs: {out[-300:]}")
        spun = re.findall(r"spinner turned ([\d.]+) rad", out)
        replayed = subprocess.run([lab, "--headless", "--replay", recording], capture_output=True, text=True)
        rout = replayed.stdout + replayed.stderr
        errors = [line for line in rout.splitlines() if " error " in line]
        check(replayed.returncode == 0, f"[replay.AC4] the headless replay matches every frame: {errors}")
        check(spun and spun == re.findall(r"spinner turned ([\d.]+) rad", rout),
              f"[replay.AC4] the spinner turned as far in the replay ({spun})")


COUNTER_SCRIPT = '''--!strict
local veach = require("@veach")
local Counter = veach.component("Counter", {{ value = 0 }})
veach.spawn("counter"):set(Counter, {{ value = 0 }})
veach.system("Count", "Simulation", veach.query(Counter), function(e, c)
	e:set(Counter, {{ value = c.value + {step} }})
end)
'''


def reload(lab: str) -> None:
    with tempfile.TemporaryDirectory(prefix="veach-reload-") as tmp:
        project = pathlib.Path(tmp)
        (project / "veach.project.json").write_text('{"name": "reload"}\n')
        script = project / "main.luau"
        script.write_text(COUNTER_SCRIPT.format(step=1))
        client = McpClient([lab, "--mcp", "--headless", "--project", str(project), "--script", "main.luau"])
        client.initialize()

        def value() -> float:
            return client.call_json("get_component", entity="counter", component="Counter")["value"]

        client.call_json("step_frames", count=2)
        check(value() == 2, "[reload.AC5] v1 counts by 1")

        script.write_text(COUNTER_SCRIPT.format(step=10) + "-- edited\n")
        result = client.call_json("reload")["reloaded"]
        check(len(result) == 1 and result[0]["path"] == "main.luau" and result[0]["reloaded"]
              and "error" not in result[0], f"[reload.AC5] reload picks up the edited script ({result})")
        client.call_json("step_frames", count=1)
        check(value() == 10, "[reload.AC5] after the reload the script runs fresh: a new counter, counting by 10")

        script.write_text(COUNTER_SCRIPT.format(step=10) + "error('broken edit')\n")
        result = client.call_json("reload", path="main.luau")["reloaded"]
        check(not result[0]["reloaded"] or "error" in result[0], "[reload.AC5] a broken edit is reported")
        check("broken edit" in result[0].get("error", ""), "[reload.AC5] the error says why")
        assets = {a["path"]: a for a in client.call_json("list_assets")["assets"]}
        check(assets["main.luau"]["reloads"] == 2 and "broken edit" in assets["main.luau"].get("reload_error", ""),
              f"[reload.AC5] list_assets shows the reloads and the last error ({assets['main.luau']})")
        client.call_json("step_frames", count=1)
        check(value() == 20, "[reload.AC5] the previous version keeps running")

        missing = client.call("reload", path="mian.luau")
        check(missing.get("isError") and "Did you mean 'main.luau'?" in missing["content"][0]["text"],
              "[reload.AC5] an unknown path is an error with a suggestion")
        client.close()


def flip(imgdiff: str, a: str, b: str) -> float:
    out = subprocess.run([imgdiff, "imgdiff", a, b, "--max-mean", "1"], capture_output=True, text=True).stdout
    return float(out.split("mean FLIP ")[1].split()[0])


def photo_mode(lab: str, imgdiff: str, golden_path: str, out_dir: str, rhi: str) -> None:
    client = McpClient([lab, "--mcp", "--headless", "--rhi", rhi, "--script", "scripts/cornell_box.luau"])
    try:
        client.initialize()
    except RuntimeError as error:
        if "rhi.backend_unavailable" in str(error) and os.environ.get("VEACH_REQUIRE_GPU") != "1":
            print(f"VEACH_GPU_UNAVAILABLE: skipped (the {rhi} device can't ray trace)")
            return
        raise
    os.makedirs(out_dir, exist_ok=True)

    def shot(samples: int, name: str) -> str:
        path = os.path.join(out_dir, name)
        result = client.call("photo_mode", samples=samples, path=path)
        check(not result.get("isError"), f"photo_mode {samples} succeeds: {result['content'][-1]['text']}")
        return path

    same = shot(64, "cornell_64.png")
    check(flip(imgdiff, golden_path, same) <= 0.01, "[photo.AC2] photo_mode 64 matches the golden run")

    client.call_json("step_frames", count=2)
    check(client.call_json("gpu_timings")["photo_mode_samples"] == 64 + 32,
          "[photo.AC2] accumulation continues while nothing changes")
    client.call_json("set_component", entity="camera", component="Transform",
                     value={"translation": [0.0, 1.0, 3.8]})
    client.call_json("step_frames", count=1)
    check(client.call_json("gpu_timings")["photo_mode_samples"] == 16,
          "[photo.AC2] moving the camera restarts the accumulation")

    reference = shot(256, "cornell_256.png")
    few = flip(imgdiff, reference, shot(8, "cornell_8.png"))
    more = flip(imgdiff, reference, shot(32, "cornell_32.png"))
    check(more < few, f"[photo.AC2] 4x the samples converges towards the reference (FLIP {few:.4f} -> {more:.4f})")
    check(client.close() == 0, "[mcp.AC10] the server exits 0 when the client disconnects")


def gpu_timings(lab: str, rhi: str) -> None:
    client = McpClient([lab, "--mcp", "--headless", "--rhi", rhi, "--script", "scripts/cornell_box_realtime.luau"])
    try:
        client.initialize()
    except RuntimeError as error:
        if "rhi.backend_unavailable" in str(error) and os.environ.get("VEACH_REQUIRE_GPU") != "1":
            print(f"VEACH_GPU_UNAVAILABLE: skipped (no {rhi} GPU)")
            return
        raise
    client.call_json("step_frames", count=3)
    plan = client.call("render_graph_plan")["content"][0]["text"]
    planned = re.findall(r"^\s+\d+\. (.+) \[(?:graphics|compute|transfer)\]$", plan, re.MULTILINE)
    timings = client.call_json("gpu_timings")
    print(json.dumps(timings, indent=2))
    check("passes_unavailable" not in timings, f"[rt.AC5] {rhi} times passes")
    check(timings["frame"] >= 3, "[rt.AC5] the timings are from the last frame stepped")
    names = [p["name"] for p in timings["passes"]]
    check(planned and names == planned, f"[rt.AC5] gpu_timings lists every pass of the frame, in order: {names}")
    check(all(isinstance(p["gpu_ms"], (int, float)) and math.isfinite(p["gpu_ms"]) and 0 <= p["gpu_ms"] < 10_000
              for p in timings["passes"]), "[rt.AC5] every pass has a GPU time in milliseconds")
    check(any(p["gpu_ms"] > 0 for p in timings["passes"]), "[rt.AC5] the passes that draw take GPU time")
    check(client.close() == 0, "[mcp.AC10] the server exits 0 when the client disconnects")


def read_png(path: str) -> tuple[int, int, list[tuple[int, int, int]]]:
    """An 8-bit RGB or RGBA PNG (what screenshots write) as rows of RGB tuples, top row first."""
    data = pathlib.Path(path).read_bytes()
    pos, idat, width, height, channels = 8, b"", 0, 0, 4
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, _, color = struct.unpack(">IIBB", chunk[:10])
            channels = 4 if color == 6 else 3
        elif kind == b"IDAT":
            idat += chunk
    raw = zlib.decompress(idat)
    stride = width * channels
    pixels, previous, i = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = previous[x]
            c = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        pixels += [tuple(line[x:x + 3]) for x in range(0, stride, channels)]
        previous = line
    return width, height, pixels


def srgb_to_linear(c: int) -> float:
    v = c / 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def realtime_client(lab: str, script: str = "scripts/cornell_box_realtime.luau") -> McpClient | None:
    client = McpClient([lab, "--mcp", "--headless", "--rhi", "metal", "--script", script])
    try:
        client.initialize()
    except RuntimeError as error:
        if "rhi.backend_unavailable" in str(error) and os.environ.get("VEACH_REQUIRE_GPU") != "1":
            print("VEACH_GPU_UNAVAILABLE: skipped (no Metal 3 GPU)")
            return None
        raise
    return client


def shot(client: McpClient, out_dir: str, name: str) -> list[tuple[int, int, int]]:
    path = os.path.join(out_dir, name + ".png")
    client.call("screenshot", path=path)
    return read_png(path)[2]


def sky(lab: str, out_dir: str) -> None:
    """sky.AC2 (the noon sky is blue) and sky.AC5 (Sky set over MCP changes the sun the next frame), at Bistro."""
    os.makedirs(out_dir, exist_ok=True)
    client = realtime_client(lab, "scripts/bistro_noon.luau")
    if client is None:
        return
    # Straight up: the camera's -Z turned 90 degrees about +X. The street's wires and lights cross some pixels, so
    # the checks use the frame's mean.
    up = {"x": math.sin(math.pi / 4), "y": 0.0, "z": 0.0, "w": math.cos(math.pi / 4)}
    client.call_json("set_component", entity="camera", component="Transform", value={"rotation": up})

    def mean(name: str) -> tuple[float, float, float]:
        pixels = shot(client, out_dir, name)
        return tuple(sum(srgb_to_linear(p[c]) for p in pixels) / len(pixels) for c in range(3))

    client.call_json("step_frames", count=8)
    noon = mean("zenith_noon")
    check(noon[2] > 1.5 * noon[0], f"[sky.AC2] the noon sky is blue: mean linear RGB {noon}")
    client.call_json("set_component", entity="sky", component="Sky", value={"time_of_day": 23.0})
    client.call_json("step_frames", count=1)
    dusk = mean("zenith_dusk")
    check(sum(dusk) < 0.2 * sum(noon), f"[sky.AC5] setting time_of_day to 23:00 darkens the sky the next frame: "
                                       f"{noon} -> {dusk}")
    check(client.close() == 0, "the lab exits 0")


def golden(lab: str, imgdiff: str, golden_path: str, out: str, rhi: str) -> None:
    client = McpClient([lab, "--mcp", "--headless", "--rhi", rhi])
    try:
        client.initialize()
    except RuntimeError as error:
        if "rhi.backend_unavailable" in str(error):
            if os.environ.get("VEACH_REQUIRE_GPU") == "1":
                raise
            print(f"VEACH_GPU_UNAVAILABLE: skipped (the {rhi} device can't ray trace)")
            return
        raise
    client.call_json("spawn_entity", name="triangle", components={"TriangleDemo": {"mode": "raytraced"}})
    client.call_json("step_frames", count=3)
    shot = client.call("screenshot")
    check(not shot.get("isError"), "[mcp.AC9] screenshot succeeds on the GPU")
    image = next(c for c in shot["content"] if c["type"] == "image")
    check(image["mimeType"] == "image/png", "[mcp.AC9] screenshot returns a PNG")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(base64.b64decode(image["data"]))
    diff = subprocess.run([imgdiff, "imgdiff", golden_path, out, "--max-mean", "0.01", "--diff", out + ".diff.png"],
                          capture_output=True, text=True)
    print(diff.stdout.strip())
    check(diff.returncode == 0, "[mcp.AC9] the image rendered through MCP matches the golden")
    check(client.close() == 0, "[mcp.AC10] the server exits 0 when the client disconnects")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    parser.add_argument("--imgdiff")
    parser.add_argument("--golden")
    parser.add_argument("--out")
    parser.add_argument("--rhi", default="metal")
    parser.add_argument("mode", choices=["protocol", "golden", "fly", "reload", "photo_mode", "gpu_timings", "replay",
                                         "replay_windowed", "sky"])
    args = parser.parse_args()
    if args.mode == "protocol":
        protocol(args.lab)
    elif args.mode == "replay":
        replay(args.lab)
    elif args.mode == "replay_windowed":
        replay_windowed(args.lab)
    elif args.mode == "fly":
        fly(args.lab)
    elif args.mode == "reload":
        reload(args.lab)
    elif args.mode == "gpu_timings":
        gpu_timings(args.lab, args.rhi)
    elif args.mode == "sky":
        sky(args.lab, args.out)
    elif args.mode == "photo_mode":
        photo_mode(args.lab, args.imgdiff, args.golden, args.out, args.rhi)
    else:
        golden(args.lab, args.imgdiff, args.golden, args.out, args.rhi)
    return 0


if __name__ == "__main__":
    sys.exit(main())
