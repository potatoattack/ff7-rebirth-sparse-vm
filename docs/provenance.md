# Sources and attribution

| Input | Pin |
| --- | --- |
| Kernel | [CachyOS `cachyos-7.3.1-1` release archive](https://github.com/CachyOS/linux/releases/tag/cachyos-7.3.1-1), commit `36ae4c219cfbd02576048c7ca1d366cea5db8034`, based on Linux 7.3.1; released 30 October 2026 |
| Kernel configuration / packaging reference | `CachyOS/linux-cachyos` commit [`343523fffa2cbfd37e37a04e793d641cbd0fe975`](https://github.com/CachyOS/linux-cachyos/tree/343523fffa2cbfd37e37a04e793d641cbd0fe975/linux-cachyos), `config` and `PKGBUILD` |
| RADV | Official [Mesa 26.2.6 release archive](https://archive.mesa3d.org/mesa-26.2.6.tar.xz), tag commit `8e87a503ebd91408c88596d1b9eeb5f4c0b2088d`; [release notes and SHA-256](https://docs.mesa3d.org/relnotes/26.2.6.html), released 28 October 2026 |
| Accepted algorithm | E27 kernel's eight patches; E16 RADV's three patches; E28 selection of the guarded direct profile |

Every source and patch is checksummed in its PKGBUILD. The checked-in kernel
configuration is the generic x86-64 CachyOS config, not a copy of the development
machine's config. Distribution training-profile options are disabled by the
recipe; the source version, scheduler patches and accepted AMDGPU changes stay
with the pinned CachyOS release. The new kernel package has a separate name.

## 2 November 2026 stable source refresh

[Kernel.org](https://www.kernel.org/) lists Linux **7.3.1** as the latest stable
release and 7.4-rc1 as development. [Mesa's release index](https://docs.mesa3d.org/relnotes.html)
lists **26.2.6** as its latest stable release; 26.3 release candidates are excluded.
CachyOS already publishes a matching `cachyos-7.3.1-1` source archive, so this
refresh preserves the project's CachyOS base rather than substituting vanilla
Linux. The package revision resets to 1 for the new kernel version. The generic
configuration is byte-identical to rc4 and to the pinned packaging reference.

All 13 existing kernel patch target files are byte-identical between the rc4
and new CachyOS archives and between official Linux 7.3 and 7.3.1. The official
7.3.1 stable tag points to commit `79a907609d53d5cd645b42c11550d295d43abf99`.
The kernel series also applies to that exact upstream release. All 16 existing
Mesa patch target files are byte-identical between 26.2.5 and 26.2.6 (the series
also creates `ff7_vm_batch.h`). No patch has landed upstream or become obsolete
in these bases: the private protocol, explicit update context, bounded batching,
guarded direct updates, leaf retention and API-owned destruction remain local.

All nine kernel and four Mesa patches are retained in their original order.
Their diff bodies are unchanged; only kernel patch 1's subject names the new
CachyOS base. Existing upstream close, reset and guard-page cleanup behavior and
Natalie Vock's attribution remain intact. No fuzz-based conflict resolution,
protocol change or runtime tuning is part of this refresh. See the per-patch
record and current validation boundaries in [rc5 validation](validation-rc5.json).

## Historical 25 October 2026 kernel package refresh

The user reports `linux-cachyos` from the v4 repository running
`7.3.0-5-cachyos`. The latest public source archive checked here is
`cachyos-7.3.0-4`; [CachyOS's public packaging recipe](https://github.com/CachyOS/linux-cachyos/blob/523baf9cc1e37b7f8079064447f1418702c45154/linux-cachyos/PKGBUILD)
uses that archive and currently lists package release 4. This project's separate
package uses release 5 to produce `7.3.0-5-cachyos-sparse-vm`. The exact build
inputs of the user's installed v4 package were not available for comparison.

The [upstream source comparison](https://github.com/CachyOS/linux/compare/cachyos-7.3.0-3...cachyos-7.3.0-4)
changes six files outside the sparse-VM patch targets. All 14 patched kernel
files are byte-identical between the two source bases after applying the same
nine patches. The new archive and checked-in generic configuration match the
upstream release and packaging checksums. See [rc4 validation](validation-rc4.json).

## Historical rc3 source refresh — 23 October 2026

These were the newest non-RC releases checked on that date. [Kernel.org](https://www.kernel.org/)
lists 7.3 as released mainline and 7.2.12 as the older stable branch's latest
point release. Its [release policy](https://www.kernel.org/category/releases.html)
considers a released mainline kernel stable. CachyOS already publishes a 7.3
release, so the recipe keeps its distro base and pins the latest source release,
`7.3.0-3`. The configuration reference used for rc3 packaged `7.3.0-2`;
the later source release changes an IOMMU revert, not the configuration.
Mesa 26.3.0-rc1 is excluded. Mesa now uses the official release tarball instead
of a mirror snapshot. Package release numbers reset to 1 for the new versions.

The nine kernel and four Mesa patches remain necessary for the private protocol
and accepted mapping behavior. The unpatched 7.3 VM implementation lacks the
series' update context and private batch/profile operations. All 13 affected
kernel files are byte-identical between the official Linux 7.3 archive and the
CachyOS release; both accept the series with zero fuzz.

- Linux commit [`679c6f09d9d58e0debec19ab46bf28e48bf76788`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=679c6f09d9d58e0debec19ab46bf28e48bf76788)
  removes the MES self-test, including `mes_v12_1_map_test_bo`. Patch 1 drops
  its obsolete conversion hunk. The explicit-unmap implementation has not
  thereby landed upstream.
- Patch 1 preserves the new GEM-close fence-detachment logic, the CS error
  return replacing a BUG, and user-queue BO reference handling. It converts
  the new user-queue sparse-PTE reset refresh to an implicit update context,
  finishing that context before either success or error handling.
- Mesa 26.2.5 fixes separately mapped guard-page cleanup. Patches 1 and 3 retain
  both unmaps, apply eligible API-owned synchronization flags to both, and
  record either error while still attempting the main BO unmap. Host tests
  exercise the actual destruction block with padding on/off and either ioctl
  failing. Batching, waits, fallback and profile selection are unchanged.

See [rc3 validation](validation-rc3.json). The earlier source pins and gameplay
evidence remain recorded in [rc2 validation](validation.json) and the benchmark
results; they are not evidence for the refreshed binaries.

Kernel patch 1 explicitly builds on Natalie Vock's **29 May 2026 “Explicit sync
for PRT unmaps” patches 1 and 2**, rebased onto the pinned CachyOS tree. Keep that
attribution when sharing or rebasing. The AMDGPU patch series is GPL-2.0-only;
RADV patches and native test sources carry MIT licensing. Existing upstream
copyright notices remain in their files. License texts are under `LICENSES/`.
New documentation, test glue and the Mesa recipe are provided under MIT; the
kernel recipe follows its GPL-2.0-only upstream packaging reference.

Patch labels preserve the investigation's lineage. They are not claims that
these changes have been accepted by Linux, AMD, Mesa, Valve or Square Enix.
The final activation patches are new source-packaging work and have not yet
inherited the prior configuration's hardware validation.

The original checkpoint remains separate. Its accepted behavior was substantially
better and playable, with occasional minor hiccups rather than a claim of zero
stutter. Captured stability evidence covered 600.698 seconds with no GPU fault,
timeout or reset and zero recorded mapping/cleanup errors. The user reported
another ten minutes without a crash. That evidence applies to the old E28
activation and binaries, not automatically to this new build.
