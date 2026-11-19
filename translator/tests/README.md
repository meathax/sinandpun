# Translator tests

## Default Sin baseline

Run from the repository root:

```powershell
dotnet test translator/Translator.sln -c Release
```

The default suite exercises the decoder, IR/SSA, code generation, synthetic DOLs,
project manifests, generic Kamek/overlay parsers, ABI analysis, source indexing and
build-shard generation. It requires the .NET SDK and restored test packages, but
does not require a WiiCompiled launcher, retail game assets, a PC runtime or a C++
compiler. The production policy test reads `projects/r2ve01/recomp.yml`.
Synthetic Mario Kart-address examples remain where they test generic translator
behavior; they are fixtures, not evidence that Sin has executed.

Optional tests report **skipped** when not requested. They do not silently return
success. A requested integration with missing inputs fails explicitly.

## Optional integrations

| Category | Activation | What it establishes |
| --- | --- | --- |
| `ExternalDol` | Set `RECOMP_GENERIC_DOL` to a DOL file | Parsing and one entry-point translation; not complete execution coverage. |
| `LlvmOracle` | Set `RECOMP_LLVM_ORACLE=1`; provide LLVM 18 tools (through WSL on Windows) | Selected decoder mnemonics compared with LLVM. |
| `DonorIntegration` | Set `RECOMP_DONOR_ROOT` to a compatible Wiicompiled checkout | Donor runtime registration/address maps and optional RetroRewind fixtures; not Sin gameplay coverage. |

Example external-DOL check:

```powershell
$env:RECOMP_GENERIC_DOL = 'D:\path\to\main.dol'
dotnet test translator/Translator.sln -c Release --filter Category=ExternalDol
```

The mixed-file donor tests remain discoverable and explicitly skipped by default.
Launcher-dependent donor tests are compiled only with an explicit build property:

```powershell
$env:RECOMP_DONOR_ROOT = 'D:\path\to\Wiicompiled'
dotnet test translator/Translator.sln -c Release -p:EnableDonorIntegrationTests=true --filter Category=DonorIntegration
```

That mode references the donor's `Launcher/WiiCompiled.Setup.Common` project and
copies its staged RetroWFC payload fixtures. It includes all of
`RegionalRetroRewindTests.cs` and the production-payload method of
`RetroWfcPayloadLoweringTests.cs`. The latter file's synthetic tests always remain
in the default suite. Donor mode is separate from the default Sin validation gate.

Clear optional environment variables before recording a default-suite baseline:

```powershell
Remove-Item Env:RECOMP_GENERIC_DOL, Env:RECOMP_LLVM_ORACLE, Env:RECOMP_DONOR_ROOT -ErrorAction SilentlyContinue
```

## Retained legacy exclusions

These seven source files were already excluded before the Sin baseline repair.
They remain preserved in the tree and are **not** counted as passing or skipped
test cases in the default run:

- `MtxIntegrationTests.cs`
- `CodeGenInlineCoverageTests.cs`
- `CodeGenTests.cs`
- `DolFileTests.cs`
- `BasicBlockTests.cs`
- `PpcDisassemblerTests.cs`
- `PpcRuntimeHelperTests.cs`

They depend on legacy game/runtime/compiler fixtures and require a dedicated
native integration harness before they can be reinstated. The donor switch does
not automatically enable them. Default-suite success does not validate native
PowerPC floating-point/paired-single execution, complete game boot or gameplay.
