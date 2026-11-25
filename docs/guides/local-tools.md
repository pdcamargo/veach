# Local tools outside the repo

Tools installed on a developer machine outside the repository, what they are for, and how to remove them.
**Agents ask the owner before installing anything here, and before accepting any license or terms on their
behalf** ([AGENTS.md](../../AGENTS.md#your-role)).

## xwin and `~/.xwin` (Windows headers for type-checking on macOS)

| | |
|---|---|
| What | [xwin](https://github.com/Jake-Shadle/xwin) (`~/.cargo/bin/xwin`, installed with `cargo install xwin --locked`), and the MSVC CRT and Windows SDK 10.0.26100 headers and libraries it downloaded to `~/.xwin` (about 630 MB) |
| Why | Nobody has a Windows machine ([ADR 0030](../adr/0030-windows-verified-in-hosted-ci.md)), so Windows-only code (the DX12 backend) is first compiled by the hosted CI job, about 4 minutes per round trip. With these headers, `tools/scripts/wincheck.sh <files>` type-checks Windows sources on macOS in seconds (`clang-cl -fsyntax-only`, plus the pinned Agility SDK headers, which the script caches in `build/wincheck/`) |
| License | Downloading the headers required accepting Microsoft's license for the MSVC and Windows SDK packages (`xwin --accept-license`). The agent accepted it on 2026-11-25 without asking first; the owner has since approved keeping it |
| Installed | 2026-11-25, for VCH-012. `cargo-xwin` in `~/.cargo/bin` predates it and is not ours |
| Remove | `cargo uninstall xwin && rm -rf ~/.xwin` (and `rm -rf build/wincheck`). Nothing in the build needs them; only `wincheck.sh` does |

MSVC in the Windows CI job is still the judge: clang-cl misses some MSVC-only warnings (C4324 slipped through
once).
