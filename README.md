# FFVII Rebirth: sparse VM improvements for AMD GPUs

Experimental Linux kernel and RADV patches to reduce stutter in Final Fantasy
VII Rebirth. They reduce the GPU memory-mapping overhead of sparse texture
updates. Two PKGBUILDs build a separate CachyOS kernel and a replacement 64-bit
RADV Vulkan driver.

After installing both packages and booting the patched kernel, enable the
feature in the game's Steam launch options:

```text
RADV_EXPERIMENTAL=sparse_vm %command%
```

The feature is disabled by default. `sparse_vm` is an option added by this
project, **not an upstream Mesa option**.

**Status: 0.1.0-rc2, experimental.** The underlying patches substantially
improved gameplay on a Radeon RX 7900 XTX. The public packages and activation
interface now have six completed off/on gameplay captures on that GPU.
Long-term stability and the packaged native GPU suite remain unverified.
Other eligible AMD GPUs are open for testing; equivalent performance and
stability are not established. See [validation](docs/validation.md).

## Measured performance

On an **RX 7900 XTX / Ryzen 9 9950X3D**, three 40-second Tower camera-rotation
captures per mode show substantially shorter long frames. Both modes use the
same packaged kernel and RADV, with `sparse_vm` off or on. Every recorded frame
is included.

| Metric | Feature off | Feature on | Change |
| --- | ---: | ---: | ---: |
| Average FPS | 64.9 | 74.9 | 15.4% higher |
| P95 frame time | 27.17 ms | 17.76 ms | 34.6% lower |
| P99 frame time | 38.47 ms | 27.50 ms | 28.5% lower |
| P99.9 frame time | 49.80 ms | 30.42 ms | 38.9% lower |
| Frames longer than 33.333 ms | 189 | 1 | Totals over ~120 s per mode |

FPS and percentiles are medians of the three per-run results. The last row
counts long frames across all three captures, not individual stutter events.

![All six off/on frame-time traces](benchmarks/results/2026-09-30-rx7900xtx/graphs/frame-times.svg)

![Frame-time percentiles with all runs and their range](benchmarks/results/2026-09-30-rx7900xtx/graphs/frame-time-percentiles.svg)

Reported settings were 3840×2160 output, 100% scaling, high textures and
`t.MaxFPS 0`; saved window dimensions were 3840×2134 in every run. The existing
6000/5000 MB streaming-pool INI overrides were retained in both modes.
Native Wine Wayland and HDR were enabled. Manual rotations are not
camera-aligned, and MangoHud does not measure physical panel scanout.

All six captures completed, and the tester reported no noticed crashes or
gameplay interruptions. These results cover one scene and one GPU; long-term
stability remains unverified. See the [full results, settings, source data and
reproduction command](benchmarks/results/2026-09-30-rx7900xtx/README.md).

## Requirements and scope

- Arch Linux or CachyOS on x86-64, using AMDGPU and RADV.
- A native discrete AMD GPU with normal RADV sparse support. There is no GPU
  model allowlist; the driver and kernel check the required capabilities.
- SDMA page-table updates. Leave `amdgpu.vm_update_mode` at its default (`-1`);
  configurations that use CPU page-table updates are unsupported.
- APUs, VirtIO and SR-IOV virtual functions are excluded from this candidate.
- The packages use pinned CachyOS 7.2.8-1 and Mesa 26.2.3 sources. Compatibility
  with newer source versions has not been verified.

Use your existing Proton installation. No custom Wine or vkd3d build is
required. Development testing used native Wine Wayland; other display paths
have not been validated by this project. Keep your display and game settings
unchanged for the initial comparison.

The kernel installs alongside your normal kernel. The RADV package replaces
`vulkan-radeon`; Mesa OpenGL and `lib32-vulkan-radeon` remain unchanged. Enable
the feature only for the game. Selection applies to its GPU address space, but
a GPU fault or reset can affect the desktop and other applications. These
experimental patches may cause rendering errors, freezes or performance
regressions. Keep a working kernel available for rollback.

## Build and install

Run as your normal user. `makepkg -s` installs missing build dependencies through
pacman; do not run `makepkg` as root. A full kernel build needs substantial disk
space and time. You can set `MAKEFLAGS` in your makepkg configuration to control
build parallelism.

```sh
sudo pacman -S --needed base-devel git
git clone https://github.com/potatoattack/ff7-rebirth-sparse-vm.git
cd ff7-rebirth-sparse-vm/kernel
makepkg -sri
cd ../mesa
makepkg -sri
```

The kernel recipe builds `linux-cachyos-sparse-vm` and matching headers for DKMS.
The driver package is `vulkan-radeon-sparse-vm`; pacman will ask to replace the
conflicting `vulkan-radeon` package.

Let your initramfs and bootloader hooks finish successfully. Ensure your boot
configuration has an entry for the new kernel, then reboot into it. Custom UKI
and Secure Boot setups need their usual image-generation and signing steps.
These packages do not configure your bootloader or change its default entry.

Check the booted release:

```sh
uname -r
```

For this revision it should be `7.2.8-2-cachyos-sparse-vm`.

## Enable for Rebirth

In Steam, open **Final Fantasy VII Rebirth → Properties → General → Launch
Options** and add:

```text
RADV_EXPERIMENTAL=sparse_vm %command%
```

If you already use launch options, incorporate this environment variable into
the existing command and keep a single `%command%`. If `RADV_EXPERIMENTAL` is
already set, add `sparse_vm` to its comma-separated list. Keep the setting local
to the game rather than exporting it for Steam or your desktop session.

The patched driver prints a diagnostic beginning with this text when activated:

```text
radv: sparse_vm v1 enabled for this device VM
```

This appears on stderr, not in the HUD. An unsupported configuration or a
missing/mismatched kernel interface fails device creation with a diagnostic.
Unpatched Mesa may ignore the option, so both packages are required.

Compare the same save, camera movement and settings with the option present and
absent, restarting the game between runs. Use the resolution and frame limit you
intend to play at; the patches do not require a particular frame cap.

## Disable or uninstall

To disable the feature, remove `sparse_vm` from the game's launch options and
restart the game. It cannot be toggled inside a running game.

To restore the distribution's RADV driver:

```sh
sudo pacman -S vulkan-radeon
```

Boot your normal kernel. After confirming it works, you can remove the
experimental kernel and headers:

```sh
sudo pacman -R linux-cachyos-sparse-vm-headers linux-cachyos-sparse-vm
```

## Testing and reporting

For a repeatable frame-time comparison, see the
[benchmark capture and graph instructions](benchmarks/README.md). They compare
the same packaged kernel and driver with the feature off and on, using automatic
MangoHud logging. The [first packaged-feature comparison](benchmarks/results/2026-09-30-rx7900xtx/README.md)
includes all six runs and their source data.

For a report, include your GPU model and PCI ID, repository revision, kernel and
package versions, Proton version, display backend and launch options. Note
whether the activation diagnostic appeared, how the same scene compares with
the option disabled, and whether you saw rendering errors, freezes or GPU resets.
Relevant kernel error messages help; avoid posting unrelated logs or your full
environment.

The [optional native tests](tests/README.md) check mapping data, ordering and
concurrent access. They are useful for developers and first reports from new
GPUs, but are not required to install the packages or play. Passing them does
not establish long-term game stability.

## Development

The repository contains source, readable patches and build recipes. There are
no prebuilt kernels or drivers. Historical experiment labels remain in patch
names for traceability; they are not installation steps or runtime settings.

- [Design and kernel/driver interface](docs/design.md)
- [Source provenance and attribution](docs/provenance.md)
- [Native and host tests](tests/README.md)
- [Validation details](docs/validation.md)
