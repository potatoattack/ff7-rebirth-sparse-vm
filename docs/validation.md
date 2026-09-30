# Candidate validation — 30 September 2026

This is source-package candidate **0.1.0-rc2**. The accepted E28 gameplay result
is the reference. The new activation interface and public packages are not yet
hardware-validated.

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

## First hardware check

1. Preserve the accepted E28 setup as rollback. Build and install both recipes
   using the README, then boot the new separately named kernel. Record any build
   or boot integration errors rather than bypassing a failed step.
2. Confirm another ordinary Vulkan workload still starts with the flag absent.
3. For this initial packaging validation, run the optional native suite once
   (`make -C tests self-test`, then
   `RADV_EXPERIMENTAL=sparse_vm bash tests/run-native.sh`). Keep its result/logs.
   This is a maintainer check, not a future player launch requirement.
4. Launch Rebirth with the single flag. Compare the accepted save/settings and
   camera movement against E28, then play normally. Capture whether the v1
   activation message appeared, any remaining hitching, freezes or GPU faults.

No new graphics-setting experiment is requested. Passing means the packaging
retains the accepted smoothness and stability while removing the setup burden.
If it regresses, compare against the preserved E28 build before changing the
mapping algorithm. Public release and wider hardware testing follow this check.
