# Record and replay

**Status:** Implemented
**Area:** runtime
**Owner:** dev lead

## Summary

`--record run.vreplay` writes what a run needs to be repeated exactly: the seed, the startup script, and for
every frame the number of fixed steps it ran, the platform events it applied (keys, mouse buttons, window
resizes, including events injected over MCP's `send_input`), and a hash of the simulation state after its
steps. `--replay run.vreplay` runs the same script with the same seed, applies the recorded events instead of
the keyboard's, runs the recorded number of steps per frame, and checks the hash every frame: the first frame
whose state differs is an error (`replay.diverged`) naming the frame and the component that differs.

## Motivation

A bug seen in a windowed session (a fly camera that drifts, a script that misbehaves after a key sequence) must
be reproducible headless, by an agent over MCP or in CI, without a person at the keyboard. The determinism
rules (fixed 1/60 s steps, `veach::Random` seeded per run, no wall clock in simulation) make simulation a
function of seed, script and input; recording the input and the step count per frame is enough to repeat it,
and the per-frame hash proves it was repeated. Roadmap: VISION item 3 (agents in the loop).

## Requirements

- `--record PATH` writes a JSON Lines file: a header (`format`, engine version, project name, script, seed,
  fixed step), then one line per frame: `{frame, steps, events, state}`. Events are the applied
  `PlatformEvent`s (type, window, key, width, height); quit and close requests end the recording. The primary
  window is recorded as window 0, because the platform's window ids differ between runs and between SDL and
  headless.
- `--replay PATH` takes the seed and the script from the header (a different `--script` or `--seed` is an
  error, `replay.invalid`), runs exactly the recorded frames (fewer with `--frames`), applies each frame's
  events and steps, and ignores the OS's input and resize events (it still honors quit and close). Headless or
  windowed.
- The state hash is FNV-1a over every entity's Scriptable components, serialized through reflection, in entity
  order. A mismatch logs `replay.diverged` with the frame and the first component that differs (by comparing
  per-component hashes, recorded alongside), and the replay stops: exit code 1.
- A file from another engine version, another fixed step or a malformed line is `replay.invalid`, with the fix.
- Hot reload is off while recording or replaying: a changed script would make the run unrepeatable.
- World edits made over MCP while recording (`set_component`, `spawn_entity`, `eval`) are recorded with the next
  frame (`edits`: the tool and its arguments) and replayed through the same tools before that frame's events and
  steps. A recording without edits reads as before.

**Out of scope**
- Rendering and GPU state: replay repeats simulation; images follow from it.

## Acceptance criteria

- **replay.AC1**: A recorded run with key input (the lab's fly camera driven by injected keys) replays
  headless with the same state hash every frame, and ends with the camera where the recording ended.
- **replay.AC2**: Replaying with a different seed or script is `replay.invalid` before the first frame.
- **replay.AC3**: A replay whose simulation differs (the recording edited by hand) stops at the first wrong
  frame with `replay.diverged`, naming the component.
- **replay.AC4**: A windowed recording, whose frames ran a varying number of fixed steps, replays headless to
  the same final state.
- **replay.AC5**: A recording made over MCP with `set_component`, `spawn_entity` and `eval` between frames records
  those edits and replays headless to the same state every frame: the same camera and the same spawned entities.

Tests: `replay: fly camera recorded over MCP replays headless (null RHI)` (AC1-AC3, AC5) and `replay: a windowed
recording replays headless` (AC4), in `tests/mcp/test_mcp_lab.py`.

## Links

- Backlog: VCH-014. Determinism rules: [AGENTS.md](../../../AGENTS.md); input: [input spec](../input/spec.md).
