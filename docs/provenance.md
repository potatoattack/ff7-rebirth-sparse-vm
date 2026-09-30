# Sources and attribution

| Input | Pin |
| --- | --- |
| Kernel | CachyOS `cachyos-7.2.8-1` release archive |
| Kernel configuration / packaging reference | `CachyOS/linux-cachyos` commit `c7d6ad1ead107761d354a12d6217ddf6840cb177`, `linux-cachyos/config` and `PKGBUILD` |
| RADV | `chaotic-cx/mesa-mirror` commit `31e9a6b2e95e30d84bf3177d1f497d063e59b6b2`, Mesa 26.2.3 |
| Accepted algorithm | E27 kernel's eight patches; E16 RADV's three patches; E28 selection of the guarded direct profile |

Every source and patch is checksummed in its PKGBUILD. The checked-in kernel
configuration is the generic x86-64 CachyOS config, not a copy of the development
machine's config. Distribution training-profile options are disabled by the
recipe; the source version, scheduler patches and accepted AMDGPU changes stay
with the pinned CachyOS release. The new kernel package has a separate name.

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
