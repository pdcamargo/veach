#!/usr/bin/env python3
"""Drives veach-lab purely through MCP over stdio (docs/specs/mcp-server/spec.md).

  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> protocol
  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> --imgdiff <veach> --golden <png> --out <png> golden
  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> fly
  python3 tests/mcp/test_mcp_lab.py --lab <veach-lab> reload

`protocol`, `fly` (the input spec's fly camera, input.AC5) and `reload` (hot reload, reload.AC5) run on the
null RHI anywhere. `golden` needs Metal 3 + ray tracing; on other GPUs it prints
VEACH_GPU_UNAVAILABLE (CTest: skipped) unless VEACH_REQUIRE_GPU=1, which makes that a failure.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading


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
    check(client.close() == 0, "[mcp.AC10] the server exits 0 when the client disconnects")


def fly(lab: str) -> None:
    client = McpClient([lab, "--mcp", "--headless", "--script", "scripts/fly_camera.luau"])
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
    yaw = client.call_json("get_component", entity="camera", component="FlyCamera")["yaw"]
    check(abs(yaw - 0.75) < 1e-3, f"[input.AC5] holding left for 30 frames turns 0.75 rad (yaw {yaw})")

    bad = client.call("send_input", key="spcae", down=True)
    text = bad["content"][0]["text"]
    check(bad.get("isError") and "[input.unknown_key]" in text and "Did you mean 'space'?" in text,
          "[input.AC5] send_input rejects an unknown key with a suggestion")
    check(client.close() == 0, "[mcp.AC10] the server exits 0 when the client disconnects")


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
    parser.add_argument("mode", choices=["protocol", "golden", "fly", "reload"])
    args = parser.parse_args()
    if args.mode == "protocol":
        protocol(args.lab)
    elif args.mode == "fly":
        fly(args.lab)
    elif args.mode == "reload":
        reload(args.lab)
    else:
        golden(args.lab, args.imgdiff, args.golden, args.out, args.rhi)
    return 0


if __name__ == "__main__":
    sys.exit(main())
