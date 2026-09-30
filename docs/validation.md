# Candidate validation — 30 September 2026

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
windows completed, but five new crash-report-client configuration entries
appeared between runs, after both off and on states. Their reports and kernel
fault logs are absent, so clean shutdown and stability are not established.
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
