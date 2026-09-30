# Design

The game repeatedly changes sparse texture mappings while rendering. The
accepted changes reduce mapping work and unnecessary dependencies. They do not
disable DirectStorage, change game shaders, preload all textures or impose a
frame cap.

| Component | Retained behavior |
| --- | --- |
| RADV sparse submission | Batches eligible 64 KiB replacements, at most 16 entries, within a single sparse submission. No cross-submission accumulation. |
| Required queue ordering | Honors incoming sparse-submission waits before mutations. Dedicated sparse-queue paths keep their existing implicit behavior. |
| AMDGPU batches | Admits eligible exact cross-BO replacements under the shared reservation; validates every entry before mutation. |
| Scalar fallback | Uses eligible direct replacements; otherwise preserves the fallback mapping path. |
| Leaf tables | Retains leaf page tables in opted-in VMs, reducing later split/retirement work. |
| Split guard | Keeps stronger synchronization when a partial update would split a larger mapping. |
| Resource destruction | Uses explicit cleanup only for eligible API-owned completed-use resources; driver-internal, imported/display and nonlocal allocations remain excluded. |
| Lifetime and faults | Retains old backing lifetime checks, move dependencies, update fences, TLB ordering and terminal error handling. |

The first eight kernel patches and first three RADV patches are the accepted
implementation. The ninth kernel patch and fourth RADV patch change selection
and negotiation. They remove the writable global `ff7_vm_cross_bo`,
`ff7_vm_explicit_direct` and `ff7_vm_leaf_only` switches. Read-only counters keep
their original names so developers can compare results with earlier evidence.

## Private protocol v1

This protocol is local to this repository. Its operation numbers are private,
not assigned upstream. Future changes must explicitly version incompatible
semantics; this release is pinned to the supplied source versions.

It uses the existing `DRM_IOCTL_AMDGPU_VM` request and its eight-byte
`union drm_amdgpu_vm`. No patched libdrm is needed.

| Operation | Input | Result |
| --- | --- | --- |
| `0x53560001` QUERY | `in.flags = 0` | Returns `0x53564d3100000001` in `out.flags`; does not enable anything. |
| `0x53560002` ENABLE | `in.flags = 1` | Enables profile v1 for this DRM file's VM and returns the same token. Idempotent. |

Unknown versions return `EINVAL`. Unsupported hardware or VM updater/state
returns `EOPNOTSUPP`. Reservation failures propagate. The kernel holds the VM's
root reservation while checking support and selecting the profile. A selected
graphics VM cannot subsequently convert to a compute VM.

The kernel accepts private mapping flags and the private batch operation only
after ENABLE. Other DRM files remain unselected. Selection persists until the
VM is destroyed. The leaf policy still latches at the first private mapping
operation, matching the accepted implementation; there is no live policy reset.

RADV invokes QUERY and ENABLE only when the normal `RADV_EXPERIMENTAL` parser
selects the new `sparse_vm` token. It verifies both return tokens. A failed
negotiation fails that device's creation with a diagnostic. Unset means no
negotiation, no private mapping requests and no optional counter files.

The selected profile is always batching + cross-BO + explicit sparse handling
+ eligible API-owned destruction + guarded direct replacements + retained leaf
tables. Rejected vectors are not recursively split (`split_rejected = false`),
matching the accepted E28 run. The driver does not expose independent switches
that could accidentally recreate combinations associated with earlier faults.

## Boundaries

Candidate rc2 replaces the NAVI31 PCI check with capability and VM checks.
RADV requires normal sparse support, dedicated VRAM and graphics capability,
and rejects VirtIO. It does not force Mesa's separate experimental `sparse`
feature on hardware where normal sparse support is disabled. The kernel rejects
APUs, SR-IOV virtual functions, debug VMs, compute VMs and CPU page-table updates.
It requires a leaf block exponent above 4; before Vega, the configured fragment
exponent must also be at least 4 so an aligned 64 KiB entry need not split into
smaller fragments. The same helper checks QUERY/ENABLE and leaf-policy selection.

The pinned kernel routes PTE encoding through its hardware-specific SDMA
callbacks. The reviewed SDMA 3–7 implementations emit ten dwords for a
contiguous PTE range; the accepted 16-entry batch remains bounded. Leaf retention
uses the device's actual page-table geometry. The per-operation checks still
require eligible existing leaf mappings and stable VRAM backing, and preserve
the split guard, old-backing lifetime, move dependencies and TLB ordering.

These facts justify opening other eligible discrete AMD GPUs to experimental
testing. They do not prove hardware correctness or game smoothness. Only the
RX 7900 XTX has original E28 gameplay evidence and completed public-package
off/on captures confirming per-VM activation. The packaged native GPU suite
and long-term stability remain unverified. Other families need native
data/ordering and overlap results plus gameplay. APUs and virtual devices
need separate work.

Read-only kernel counters are system-wide. They attribute work but cannot prove
which process caused a change when multiple opted-in applications run. Developer
tests should run without another opted-in application. Optional
`RADV_SPARSE_VM_STATS_DIR` counters are per RADV winsys and are created only for
an enabled profile; they are not needed for gameplay.
