#!/usr/bin/env python3
"""Repository hygiene checks. Run from the repo root; CI and CTest run it too.

1. Every TODO/FIXME in code references a backlog item: TODO(VCH-123): ...
   and that item exists in docs/backlog/BACKLOG.md.
2. No `flecs::` in apps/ — apps use the veach:: aliases (ADR 0005).
3. CLAUDE.md is exactly one line: @AGENTS.md (ADR 0018).
4. Every .slang file with a parameter block has its C++ mirror checked against the reflected layout:
   parameters_match<...>(<namespace>::layout::<stem>, ...) somewhere in C++ (VCH-041, ADR 0024).
5. Every .slang file that uses DescriptorHandle imports veach.bindless, so debug builds check its handles
   (ADR 0039).
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CODE_DIRS = ["engine", "apps", "tools", "tests", "cmake", ".github"]
CODE_SUFFIXES = {".cpp", ".hpp", ".h", ".mm", ".inl", ".luau", ".cmake", ".py", ".yml", ".yaml", ".json", ".sh"}
CODE_NAMES = {"CMakeLists.txt"}
MARKER = re.compile(r"\b(TODO|FIXME|XXX|HACK)\b(?!\(VCH-\d{3,}\))")
REFERENCE = re.compile(r"\b(?:TODO|FIXME|XXX|HACK)\(VCH-(\d{3,})\)")
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
                          "`import veach.bindless;` (its handles would go unchecked, ADR 0039)")

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
