# Optional developer tests

Players do not need to build or run these to install the packages or launch
Rebirth. Maintainers should run them when changing the kernel/driver interface,
mapping logic or supported hardware. They test correctness and path coverage;
they do not establish game smoothness.

## Native programs

Dependencies: a C compiler, make, pkg-config, Vulkan headers and Vulkan loader.
On Arch:

```sh
sudo pacman -S --needed base-devel vulkan-headers vulkan-icd-loader
make -C tests self-test
```

This builds `lifetimes` and `neighbors` and runs their CPU-only pattern/layout
checks. It does not submit GPU work during `self-test`.

With the packaged kernel and RADV installed, run as your normal user:

```sh
RADV_EXPERIMENTAL=sparse_vm bash tests/run-native.sh
```

The runner creates a separate results directory, performs required-ordering and
lifetime tests, then runs 72 disjoint-neighbor cases. It verifies counter
activation, zero error deltas, expected case counts, full data checks and
GPU-confirmed overlap. The tiny changed-range GPU probes preserve the E22 v3
control that distinguished actual overlap from an asynchronous API return.
The older 16 long-probe rounds are checked for completion and data, and their
overlap observations are reported without requiring them: they had 0/16 pending
observations in the accepted E28 run. E22's tiny probes establish overlap.

The GPU programs use bounded fence waits and test their generated readback data.
They accept any discrete AMD RADV device, with no PCI model restriction, and
require exactly one such device to be visible. On multi-discrete-GPU systems,
use Mesa's Vulkan device selection to expose only the intended test device.
Sparse buffer support and the original separate graphics/two-compute-queue
topology are still required; the programs fail if that coverage is unavailable.
The neighbor test also checks overlap without remapping. Overlap results depend
on workload calibration; incomplete overlap is a test limitation/failure to
review, not evidence of corrupted data. Stop on faults, device loss or failed
checks. Do not infer stability from a successful self-test.

Close other applications using `sparse_vm` while running: kernel counters are
global. Ordinary applications may remain open. These tests require the patched
kernel even for access to its read-only counters; no sudo, bpftrace, tracing
session, module-parameter write or reboot gate is involved.

The original C source names and experiment labels are retained to make review
against the accepted tests straightforward. Public adaptations replace old
environment/global-switch checks with the new request, generalize GPU selection
and remove the old supervisor's stdin handshake. The GPU workloads and data
oracles remain readable.

## Host regression checks

After `makepkg -o` has prepared both source trees:

```sh
bash tests/run-host.sh kernel/src/cachyos-7.2.8-1 \
  mesa/src/mesa-mirror-31e9a6b2e95e30d84bf3177d1f497d063e59b6b2
```

These compile the actual new activation functions against controlled ioctl and
reservation boundaries, test default-off/unsupported/error behavior and protocol
agreement, exhaustively exercise batching fallbacks, check required queue waits,
and check API-owned destruction eligibility. The host checks do not replace
compilation of the driver or native testing on a GPU.
The batch check includes the supplied patched Mesa tree's header directly;
there is no separate copied implementation in the test directory.
