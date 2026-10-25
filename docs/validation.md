# Candidate validation — 25 October 2026

Source-package candidate **0.1.0-rc4** uses CachyOS `cachyos-7.3.0-4`
(Linux 7.3) and builds the separate kernel as `7.3.0-5-cachyos-sparse-vm`.
Mesa remains at 26.2.5 with the rc3 patch set. The new kernel source archive's
SHA-256 and BLAKE2 match CachyOS's release asset and packaging recipe.

| Check | rc4 result / boundary |
| --- | --- |
| Kernel source and recipe | All 11 source/config/patch BLAKE2 checksums pass. The generic config is unchanged from rc3. `kernelrelease` returns `7.3.0-5-cachyos-sparse-vm`. |
| Kernel patch application | All nine patches apply with zero fuzz to the exact `cachyos-7.3.0-4` release archive. The application log has the same offsets as rc3, and all 14 patched target files match rc3 byte for byte. |
| Kernel compilation | All 11 affected AMDGPU C translation units compile with AMDGPU and HSA/SVM enabled. No compiler warnings or errors appear in their build log. This is an object build, not a full kernel/module link or the Clang/LTO package build. |
| Host regressions | The host suite passes on the newly patched kernel source and unchanged patched Mesa source, including batch fallback, synchronization, ownership, and protocol checks. Four native-result validator tests pass. |
| Mesa | The Mesa source, patches and recipe are unchanged. The complete RADV build and staging recorded for rc3 carry forward; no new RADV build or GPU test was run for rc4. |

The user's v4 system reports `linux-cachyos 7.3.0-5`, but the exact build inputs
of that installed package were not available for comparison. The latest public
CachyOS source release checked here is `cachyos-7.3.0-4`; the custom `-5`
identifies this separate package revision. A full Arch `makepkg` build, boot,
DKMS/initramfs integration, native GPU tests and game validation remain pending.
No new performance or stability claim is made. See
[rc4 machine-readable evidence](validation-rc4.json) and
[source provenance](provenance.md).

## Historical rc3 validation — 23 October 2026

Source-package candidate **0.1.0-rc3** updates both bases to released sources:
CachyOS **7.3.0-3**, based on Linux **7.3**, and Mesa **26.2.5**. The private
protocol and accepted sparse mapping profile remain v1. Historical performance
and stability observations below apply to rc2 or E28, not this refresh.

| Check | rc3 result / boundary |
| --- | --- |
| Source integrity | Mesa SHA-256 matches its official release notes; CachyOS SHA-256 matches its GitHub release asset digest. All recipe input checksums pass. Official Linux 7.3 SHA-256 matches kernel.org's checksum list. |
| Patch application | Four Mesa patches and nine kernel patches apply with `--batch --forward --fuzz=0` to the exact release archives. Kernel patches also apply to pristine upstream Linux 7.3; its 13 affected files match CachyOS byte for byte. Patched files match the compiled trees. |
| Upstream audit | Removed the obsolete MES self-test hunk. Preserved upstream GEM-close, CS error, user-queue/reset and separate Mesa guard-page cleanup behavior. See [provenance](provenance.md). |
| Host regressions | Exhaustive batching masks/fallbacks, wait ordering, ownership exclusions and protocol/eligibility tests pass. Extended the actual physical destruction test to cover padding, both synchronization modes and either unmap failing. Native result validator: four tests pass. |
| Kernel compilation | All 11 affected C translation units compile with GCC 16.2.1, x86-64 defconfig plus AMDGPU module and HSA/SVM enabled. This is a targeted build, not the recipe's full Clang/LTO configuration or a linked kernel/module. |
| RADV compilation | Complete release build and link, X11 + Wayland, ACO, LLVM disabled; GCC 16.2.1, libdrm 2.4.134, Meson 1.12.1, Ninja 1.13.2. |
| RADV staging | Updated `package()` runs successfully into a private staging directory. Contains the 64-bit driver, ICD, RADV defaults and license; excludes shared Mesa defaults. ICD points to `/usr/lib/libvulkan_radeon.so`; no RPATH/RUNPATH. |
| Native programs | Both compile with `-Wall -Wextra -Werror` and pass CPU self-tests. Compilation uses Vulkan headers bundled in the exact Mesa release. No GPU execution. |
| Tooling / recipes | Eight benchmark tests, Bash syntax, ShellCheck error checks and input checksums pass. |

Validation ran only in the isolated dot VM workspace. Missing build tools were
placed in a private task directory. No packages, kernel, driver or configuration
were installed on the live system. No desktop/game launch, GPU validation,
reboot, DKMS, initramfs or bootloader check was performed. A full Arch `makepkg`
build and hardware/native/game validation remain pending for rc3. No new
performance claim is made. Machine-readable pins, file hashes, build commands
and results are in [validation-rc3.json](validation-rc3.json).

## Historical rc2 validation — 30 September 2026

This is source-package candidate **0.1.0-rc2**. The accepted E28 gameplay result
remains the historical reference. The public packages and new activation
interface now have six completed off/on gameplay captures on an RX 7900 XTX.
The packaged native GPU suite and long-term stability remain unverified.

rc2 opens eligible native discrete AMD GPUs to testing through capability
checks. It adds a pre-Vega fragment-size check, generalizes native-test device
selection, and retains the accepted mapping and synchronization algorithms.
No additional GPU has been tested here. Package release numbers are now 2.

| Check | Result / boundary |
| --- | --- |
| Source downloads | Pinned kernel BLAKE2 and Mesa SHA-256 reproduced the accepted source hashes. |
| Patches | Nine kernel patches and four Mesa patches apply with zero fuzz. Resulting modified files match the compiled source trees. |
| New interface | Compiled the actual kernel and RADV activation functions with controlled boundaries: query is read-only, VM selection is independent and idempotent, unrelated flags/default-off make no private ioctl, errors and hardware/version mismatches reject activation. |
| Mapping host regression | Exhaustive 1–16-entry eligibility masks, bounded vectors, ordered scalar fallback, terminal error/no-replay behavior and independent contexts pass. |
| Queue/lifetime host regression | Actual extracted RADV functions pass required-wait ordering/failure tests and API-owned cleanup scope/exclusion checks. |
| Kernel compilation | All 12 affected AMDGPU C translation units compiled for rc1; rc2 recompiles the changed `amdgpu_vm.c`, retaining the other unchanged objects. GCC 13, x86-64 test configuration. |
| RADV compilation | Complete release build and link from rc1, followed by rc2's incremental rebuild and relink; Wayland + X11, ACO, LLVM disabled. Staged the revised Mesa `package()` function. |
| RADV package contents | Only the 64-bit library, its ICD, RADV defaults and license. The shared `00-mesa-defaults.conf` is excluded because Arch's Mesa package owns it. No staging-directory RPATH/RUNPATH. |
| Native programs | Both compile with warnings as errors and pass CPU self-tests; GPU execution is pending. |
| Native result validator | Rejects missing/duplicate cases, unchecked data and missing tiny-probe overlap. Accepts the known 0/16 observation from legacy long probes without treating it as overlap evidence. |
| Recipes | Bash syntax, ShellCheck error checks and every declared input checksum pass. |

Compilation used an isolated Ubuntu build environment with a private dependency
prefix, including libdrm 2.4.133. It was **not** a full Arch `makepkg` kernel
build, package installation, DKMS build, initramfs generation, boot test or GPU
test. No new kernel or driver binary is distributed here. See
`validation.json` for source/file hashes and recorded check details.

## Packaged hardware evidence

The [30 September RX 7900 XTX comparison](../benchmarks/results/2026-09-30-rx7900xtx/README.md)
records three complete 40-second runs per mode on
`7.2.8-2-cachyos-sparse-vm`, with `linux-cachyos-sparse-vm 7.2.8-2` and
`vulkan-radeon-sparse-vm 3:26.2.3-2` installed. The mapped RADV library matches
the installed library. The v1 activation diagnostic is recorded as present
only in enabled runs. All six use the same recorded binaries and saved gameplay
settings. This establishes observed package installation, boot and short game
operation with the new interface. It does not audit the full Arch build or
bootloader hooks; those logs were not supplied.

The median per-run P99 frame time fell from 38.47 to 27.50 ms. All measured
windows completed, and the tester subsequently reported no noticed crashes or
gameplay interruptions. Five new crash-report-client configuration entries
appeared between runs, after both off and on states. These entries alone do
not establish a gameplay crash; their cause is unknown without the reports.
Long-term stability and shutdown behaviour remain unverified.
No new packaged native GPU results accompanied these captures; historical E22
and E28 native results are not silently transferred to the public interface.

## Remaining maintainer checks

1. Preserve a known working kernel/driver for rollback. For each additional GPU,
   build/install using the README and record build or boot integration errors.
2. Confirm another ordinary Vulkan workload still starts with the flag absent;
   the recorded off runs currently establish this only for Rebirth.
3. For packaging validation, run the optional native suite once
   (`make -C tests self-test`, then
   `RADV_EXPERIMENTAL=sparse_vm bash tests/run-native.sh`). Keep its result/logs.
   This is a maintainer check, not a future player launch requirement.
4. Play normally and check shutdown in both modes. Retain any crash report or
   relevant kernel fault messages. For additional GPUs, record activation and
   compare the same save/settings and camera movement with the feature off/on.

No new graphics-setting experiment is requested. Passing means the packaging
retains the accepted smoothness and stability while removing the setup burden.
If it regresses, compare against a preserved working build before changing the
mapping algorithm. The project remains an experimental release candidate.
