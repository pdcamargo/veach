#!/usr/bin/env python3
"""Repository hygiene checks. Run from the repo root; CI and CTest run it too.

1. Every TODO/FIXME in code references a backlog item: TODO(VCH-123): ...
   and that item exists in docs/backlog/BACKLOG.md.
2. No `flecs::` in apps/ — apps use the veach:: aliases (ADR 0006).
3. CLAUDE.md is exactly one line: @AGENTS.md (ADR 0022).
4. Every .slang file with a parameter block has its C++ mirror checked against the reflected layout:
   parameters_match<...>(<namespace>::layout::<stem>, ...) somewhere in C++ (VCH-041, ADR 0027).
5. Every .slang file that uses DescriptorHandle imports veach.bindless, so debug builds check its handles
   (ADR 0027).
7. Canonical floating point (ADR 0043): cmake/VeachCompiler.cmake sets -ffp-contract=off -fno-fast-math (Clang) and
   /fp:precise (MSVC) globally, and no build file turns on fast math or FP contraction.
6. The engine never knows the space toolbox exists: nothing under engine/ names `veach/space`, `veach::space` or
   `veach_space` (ADR 0042). What the toolbox needs that is generic moves down into the engine instead.
8. The engine owns the threads (ADR 0073): no code under engine/, space/, apps/ or tools/ starts a thread
   (std::thread, std::jthread, std::async, pthread_create, CreateThread, _beginthread) or gives Flecs its own
   (set_threads). Work goes to veach::JobSystem. Tests may start threads to test thread safety.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CODE_DIRS = ["engine", "space", "apps", "tools", "tests", "cmake", ".github"]
CODE_SUFFIXES = {".cpp", ".hpp", ".h", ".mm", ".inl", ".luau", ".cmake", ".py", ".yml", ".yaml", ".json", ".sh",
                 ".slang"}
CODE_NAMES = {"CMakeLists.txt"}
MARKER = re.compile(r"\b(TODO|FIXME|XXX|HACK)\b(?!\(VCH-\d{3,}\))")
REFERENCE = re.compile(r"\b(?:TODO|FIXME|XXX|HACK)\(VCH-(\d{3,})\)")
SPACE = re.compile(r"veach/space\b|veach::space\b|\bveach_space\b")
GETENV = re.compile(r"\bgetenv\s*\(|_dupenv_s\s*\(")
THREAD = re.compile(r"\bstd::j?thread\b(?!::(?:hardware_concurrency|id)\b)|\bstd::async\b|\bpthread_create\b|"
                    r"\bCreateThread\b|\b_beginthread(?:ex)?\b|set_(?:task_)?threads\s*\(")
# Threads the engine owns outside the job system, each with its reason. Add to this list only with an ADR.
THREAD_ALLOWED = {
    # The MCP server's stdin reader blocks in getline for the whole session; on a worker it would take one
    # away from jobs for good (ADR 0073).
    "engine/runtime/src/mcp_server.cpp",
}
SELF = pathlib.Path(__file__).resolve()


def hidden_or_build(path: pathlib.Path) -> bool:
    """Dependencies, build trees and hidden directories (agent worktrees under .claude/) are not ours to check."""
    parts = path.relative_to(ROOT).parts
    return "build" in parts or any(p.startswith(".") for p in parts)


def code_files():
    for top in CODE_DIRS:
        base = ROOT / top
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.is_file() and (path.suffix in CODE_SUFFIXES or path.name in CODE_NAMES) and path != SELF:
                yield path
    yield ROOT / "CMakeLists.txt"


def main() -> int:
    errors: list[str] = []
    backlog = (ROOT / "docs/backlog/BACKLOG.md").read_text(encoding="utf-8")
    known_ids = set(re.findall(r"\bVCH-(\d{3,})\b", backlog))

    for path in code_files():
        rel = path.relative_to(ROOT).as_posix()
        for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if MARKER.search(line):
                errors.append(f"{rel}:{lineno}: bare TODO — write TODO(VCH-NNN): ... and add the item to "
                              "docs/backlog/BACKLOG.md")
            for item in REFERENCE.findall(line):
                if item not in known_ids:
                    errors.append(f"{rel}:{lineno}: TODO references VCH-{item}, which is not in BACKLOG.md")
            if rel.startswith("engine/") and SPACE.search(line):
                errors.append(f"{rel}:{lineno}: the engine names the space toolbox; it must not know it exists "
                              "(ADR 0042). Move what the toolbox needs into the engine as a generic feature.")
            if GETENV.search(line) and rel not in ("engine/core/src/env.cpp",) and not rel.endswith((".py", ".sh", ".yml")):
                errors.append(f"{rel}:{lineno}: read environment variables with veach::env() (veach/core/env.hpp); "
                              "MSVC deprecates getenv, and warnings are errors")
            if (rel.split("/")[0] in ("engine", "space", "apps", "tools") and rel not in THREAD_ALLOWED
                    and path.suffix in (".cpp", ".hpp", ".h", ".mm", ".inl") and THREAD.search(line)):
                errors.append(f"{rel}:{lineno}: the engine owns the threads (ADR 0073); run the work on "
                              "veach::JobSystem (parallel_for, TaskGroup) instead of starting a thread")
            if rel.startswith("apps/") and "flecs::" in line:
                errors.append(f"{rel}:{lineno}: `flecs::` in apps/ — use the veach:: aliases (veach::World, "
                              "veach::Entity, ...) or veach::ecs:: for raw Flecs")

    cpp_sources = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in code_files()
                             if p.suffix in {".cpp", ".mm", ".hpp"})
    for shader in sorted(ROOT.glob("**/shaders/*.slang")):
        if hidden_or_build(shader):
            continue
        if "ConstantBuffer<" in shader.read_text(encoding="utf-8") and \
                not re.search(r"layout::" + re.escape(shader.stem) + r"\b", cpp_sources):
            errors.append(f"{shader.relative_to(ROOT).as_posix()}: its parameter block has no C++ layout check; add "
                          f"static_assert(veach::rhi::parameters_match<T>(<namespace>::layout::{shader.stem}, {{...}}))")

    for shader in sorted(ROOT.glob("**/*.slang")):
        if hidden_or_build(shader) or shader.name == "bindless.slang":
            continue
        text = shader.read_text(encoding="utf-8")
        if "DescriptorHandle" in text and not re.search(r"^import veach\.bindless;", text, re.MULTILINE):
            errors.append(f"{shader.relative_to(ROOT).as_posix()}: uses DescriptorHandle without "
                          "`import veach.bindless;` (its handles would go unchecked, ADR 0027)")

    compiler = (ROOT / "cmake/VeachCompiler.cmake").read_text(encoding="utf-8")
    for flag in ("-ffp-contract=off", "-fno-fast-math", "/fp:precise"):
        if flag not in compiler:
            errors.append(f"cmake/VeachCompiler.cmake: missing {flag}; canonical floating point needs it on every CPU "
                          "target (ADR 0043)")
    fast_math = re.compile(r"-ffast-math|-ffp-contract=(?:fast|on)\b|/fp:fast|/fp:contract|-Ofast")
    build_files = [ROOT / "CMakeLists.txt", ROOT / "CMakePresets.json", *sorted((ROOT / "cmake").glob("*.cmake")),
                   *sorted(p for top in ("engine", "space", "apps", "tools", "tests") for p in (ROOT / top).rglob("CMakeLists.txt"))]
    for path in build_files:
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if fast_math.search(line) and not line.lstrip().startswith("#"):
                errors.append(f"{path.relative_to(ROOT).as_posix()}:{lineno}: fast math or FP contraction breaks "
                              "canonical determinism (ADR 0043); shaders keep fast math through cmake/VeachShaders.cmake")

    claude = (ROOT / "CLAUDE.md").read_text(encoding="utf-8")
    if claude != "@AGENTS.md\n":
        errors.append("CLAUDE.md must contain exactly one line, '@AGENTS.md'; put instructions in AGENTS.md")

    for error in errors:
        print(error)
    if errors:
        print(f"\n{len(errors)} problem(s).")
        return 1
    print("repo hygiene: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
