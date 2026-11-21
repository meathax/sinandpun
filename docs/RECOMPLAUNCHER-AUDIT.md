# RecompLauncher audit — 2026-11-21

The sections below record the first audit. The follow-up is in [Wii setup and packaging](WII-SETUP.md).

Verdict: the core Windows integration is implemented and locally smoke-tested, but full launcher acceptance
and compliance with every instruction in the SDK agent prompt are not established. Do not describe this
audit ZIP as a fully qualified public release.

## What changed

- Retained Easy Setup, direct launch, TOML settings, save paths, folder actions and the themed README badge.
- Extracted-folder `--prepare` now generates all eight stage images with the existing disc import linker,
  under the selected data directory. Previously this branch only wrote configuration and a marker.
- Launcher packaging excludes disc-derived `modules/`; the release audit rejects their accidental inclusion.
  Setup version 2 and eight module entries in `prepare.done` force older installations to prepare again.
- Packaging stops on PowerShell errors, copies the manifest byte-for-byte and accepts an explicit Python
  executable, avoiding this machine's broken default Python launcher. Corrected the documented standalone paths.
- Added two packaging regressions; expanded the game-file extension exclusion checks.

## Checks and evidence

Official agent prompt and all three guide parts were retrieved from https://recomplauncher.com/sdk-files/.
The schema was downloaded from the RecompLauncher-SDK main branch and validated with jsonschema 4.26.0.
Raw guide/schema snapshots and run evidence are under `build/launcher-audit-evidence/`.

- Official JSON Schema: PASS, including the updated setup marker list.
- CMake/Ninja `WiiCompiled` rebuild: PASS (`build-final.log`). This is a dirty working-tree build;
  pre-existing unrelated edits were preserved. No source commit or remote publication was made.
- Python packaging/tool suite: 10 tests passed. Runtime-config and platform-path native tests exited 0.
- Packaged executable: extracted R2VE01 preparation exited 0; all eight generated modules matched
  `build/modules/stg00.bin` through `stg07.bin` byte-for-byte. Every `prepare.done` path existed.
- Missing game input exited 1. Logs and exact outcomes are in `runtime-results.json` and `prepare-verified.log`.
- Packaged launch with `--data`/`--game` and launch with no command-line arguments both remained running for
  18 seconds, with title-stream reads in the logs. Both were terminated by the audit. The no-argument test
  used `RECOMPLAUNCHER_DATA` to isolate state: this does not qualify an untouched first-run picker,
  normal quitting, visual correctness, a playthrough or save/reload.
- Fresh staging audit: zero errors/warnings. ZIP inspection confirmed the root manifest and executable,
  manifest byte identity, and absence of modules, UserData and recompdata. This structural audit does not
  establish provenance of every byte embedded in the executable.
- ZIP: `dist/SinAndPunishmentRecomp-0.1.0-launcher-audit-20261121-Windows-x64.zip`.
  SHA-256: `7985ce6cc44c82a6c1bec45da0a8f14c04e28cf9a29013b93547a4bb40e785a6`.

## Still open

- **Embedded keys (medium):** `runtime/src/disc_import.cpp` contains Wii common-key constants. This
  conflicts with the agent prompt's no-keys rule. Moving to player-supplied keys affects the existing
  interactive importer as well as launcher setup and needs a separate, tested migration. This audit
  does not certify the executable as containing no keys or no other embedded game-derived content.
- **Launcher acceptance (small):** run Add a build, the Developer-tab check, Easy Setup, a settings edit,
  Play, normal Quit and save backup in the actual launcher. Direct executable tests do not prove that UI flow.
- **Disc-image acceptance (medium):** ISO/WBFS support exists, but this audit tested the extracted-folder
  path only. The available local image is NKit, which the importer explicitly rejects.
- **Wii formats (medium/large):** RVZ and GCZ are missing; players must currently supply ISO/WBFS or extracted
  files. These are the Wii formats named in Part 3; CHD support for other consoles is not the relevant target.
- **Verified dumps (small once sourced):** `game.dumps` is omitted because no authoritative size/CRC32 was
  established. No hashes were invented. Region/folder checks are not full dump verification.
- **Catalog/release (small, external):** the retrieved PC catalog contained no `meathax/sinandpun` match.
  The generic badge link remains appropriate. Submit/claim the repo and publish a separately qualified
  versioned release with notes; no submission, push or publication was performed here.
- **Linux/macOS releases (large):** this package is Windows x64 only; other platforms were not qualified.
- **Recomp Achievements (medium/large):** no integration was established; achievements, Beaten/Mastered
  status and leaderboards remain additional work.
- **DLSS setup (medium):** launcher packages omit the optional vendor DLLs and expose Off/FSR only.
  No consent-based vendor setup or DLSS launcher option is implemented by this audit.
- **Controller polish and logs (small/medium):** controller-only completion, mode/pad environment hints,
  settings round-trip and launcher discovery of nested session logs still need acceptance testing.

The remaining gameplay acceptance work in `REMAINING.md` also applies. These launcher smoke tests do not
supersede it. A developer can claim the project at https://recomplauncher.com/dev; that action was not taken.
