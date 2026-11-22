# Sin & Punishment: Star Successor release bug audit

The audit covers the Windows x64 recomp and its local testing package. The source fixes are in commit `bb627cc`. Both audit executables included graphics changes that were uncommitted at build time. The graphics implementation was subsequently committed in 38f7ab1. Those source changes are outside the audit commit. A separate build replaced the refreshed executable in `ready` during final verification. The replacement passed the package audit and all seven setup checks against a stable on-disk hash.

## Verification results

- Release rebuild completed. The isolated candidate used for the boot smoke and launcher ZIP has SHA-256 `F78F73EDAB50240F3E7644CC9CEF4C6876218407ED70F2607933517DCA0BE36E`. The replacement verified in `ready` has SHA-256 `2858A8089CAF7034FA56585CC693DA3604B02C314398BA1219268D50B0057EE2`.
- The rebuilt translator suite passed 767 tests. Eight opt-in tests skipped. Windows runtime CTest passed 15 of 15. QA-tool tests passed 10 of 10.
- The tested executable prepared an extracted game folder, a synthetic ISO, a synthetic GCZ, and a synthetic RVZ. Each preparation generated all eight modules byte-for-byte against the local references.
- The missing-key and busy-import cases left existing staging intact. A bad relocation in stage seven failed while preserving the existing configuration, prepared marker, and all eight modules.
- An 18-second headless boot initialized the GX worker and read the title music stream. This verifies startup with the packaged executable and local game data.
- The package audit reported zero errors and zero warnings for the refreshed `ready` and the private launcher staging directory. The launcher archive was built locally for testing. The executable and archive both passed the historical disc-key signature scan. Nineteen existing ready configuration and NAND files retained their hashes.

The corresponding evidence is in `release-audit-rebuild.log`, `release-audit-translator.log`, `release-audit-ctest.log`, `audit-821ffa41`, `release-audit-boot-stderr.log`, and `release-audit-final-package.json` under the local build/final-qa directory. The replacement checks are in `audit-f9df93ea` and `release-audit-current-package.json`.

## Confirmed defects corrected

| Area | Defect and effect | Correction |
| --- | --- | --- |
| Disc import | Every attempt used the same staging directory. Even an attempt rejected for a missing key could clean up a different attempt’s extraction. | Acquiring an exclusive sibling lock establishes ownership before staging can be removed or created. Cleanup runs only for the owner. The OS releases the lock when the process exits. |
| Disc import | Closing a buffered output stream could fail after the final write, and cancellation just before publication could still replace the previous game. | Check stream status after closing and check cancellation before moving the staged directory into place. Normalize the parent for relative destinations. |
| Stage module prelink | The linker checked a relocation’s starting byte, then wrote the full value. A truncated record could overwrite following bytes or leave the output buffer. Large section layouts and generated branch stubs could exceed the runtime’s two-MiB stage window. | Check the complete halfword or word against its own section. Bound the layout and stub writes using widened address arithmetic. |
| Setup from extracted data | Setup invalidated the prepared marker and replaced modules before validating all eight inputs. An error in a later stage could damage a working setup. | Link every stage before touching the marker or existing modules. Check the marker stream after closing. A later I/O failure during module publication can still require another prepare attempt. |
| Renderer lifetime | Failed or repeated Reflex attachment could retain a driver handle or device. The optional status read and producer-frame update could race detachment. | Release failed attachment resources, guard repeat attachment, and keep frame publication and status read under the lifetime lock. |
| Testing package | Required runtime copies could fail without a terminating error or be discovered after existing files were removed. | Fail on copy errors and check the executable, core DLLs, bootstrap directory, AMD assets and eight local modules before refresh. Runtime-file publication still uses individual copies. |
| Launcher packaging | The archive script created an empty graphics notices directory. | Require and copy the notices for the graphics SDKs already included in the launcher package. |

## Coverage and remaining test scope

| Area | Automated or source coverage | Remaining scope |
| --- | --- | --- |
| Translator and CPU | Translator unit suite, PowerPC paired-float tests, host-context tests and native compile of the generated game and stage code. | Optional external oracle and corpus integrations, end-to-end guest execution across all stages. |
| Disc, launch and modules | Runtime setup fixtures, module comparisons, failure preservation, package and bootstrap integrity. | Real retail image and damaged-media coverage beyond the fixtures, including very slow drives and cancellation during live extraction. |
| Input, controller and motion | Controls, focus loss, expression bindings, Bluetooth path and motion tests. | Physical controller and gyro feel, hotplug under load and couch co-op. |
| Configuration and storage | Platform paths, runtime configuration, NAND save and guest path tests. The refresh separately compares existing NAND and configuration files by hash. | User-facing save/load during a full run, including interrupted writes and alternate profiles. |
| Audio | Audio slot guard test and review of the native DMA path. | Listening pass for dialogue, music, mixes and endpoint changes. |
| Rendering and performance | Native renderer build, package presence and Reflex lifetime review. | Visual fidelity, HDR/VRR, cross-vendor GPU modes, frame-generation cadence and long-running performance. |
| Optional online services | Native achievements integration builds with the executable. | Live sign-in, award submission, reconnect, and service behavior when offline. |
