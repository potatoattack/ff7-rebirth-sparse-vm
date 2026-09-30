# Measuring the packaged feature

Compare **the same packaged kernel and RADV with `sparse_vm` off and on**.
This isolates the opt-in feature; it is not a comparison between different
distribution builds. Both modes keep your existing Proton and display backend.

The public package's performance comparison is still pending. Earlier controls
already contained some fixes, and settings changed over the investigation.
They cannot be labelled an unpatched-versus-final benchmark. The
[historical example](examples/e07/README.md) demonstrates the plotting format
with a clearly identified incremental comparison.

## Before recording

1. Build/install both packages and boot the packaged kernel using the main
   README. Finish the first hardware check in `docs/validation.md`.
2. Use the save, viewpoint and graphics settings you intend to play with.
   Keep resolution, both scaling sliders, texture quality, frame limit,
   upscaler/frame generation, HDR/VRR, Proton, INI values and mods identical
   throughout. For this comparison, keep your current 100%/100% scaling and
   intended frame-limit setting, and record the actual setting. Leave texture
   quality and the existing INI values unchanged.
3. Take one settings screenshot and note the displayed cap/scaling settings.
   Saved INI values alone do not establish the live render resolution or cap.
4. Use the same camera motion, preferably a controller stick held consistently,
   from the same starting view. Keep the shader caches warm; do not clear them
   between runs. Close downloads and unrelated heavy workloads.

The capture script requires Python 3 and MangoHud. Matplotlib is needed only on
the computer producing graphs. No sudo or probes are used. It reads the game
process, checks the loaded driver and activation message, saves configuration
hashes, and records every frame with MangoHud. It creates its own logging
configuration without editing your normal MangoHud configuration. It does not
change a frame limit, game setting, INI file or kernel switch.

## Record one run

Replace `/absolute/path/ff7-rebirth-sparse-vm` with your clone's absolute path.
Use this Steam launch option for the first run:

```text
python3 /absolute/path/ff7-rebirth-sparse-vm/benchmarks/capture.py launch off off-1 -- %command%
```

The wrapper selects the mode itself. Use a direct launch through this command;
do not combine it with a private experimental driver launcher. The original
working setup can remain on disk for rollback.

After the save is loaded and you are at the starting view, run in a terminal:

```sh
python3 /absolute/path/ff7-rebirth-sparse-vm/benchmarks/capture.py capture \
  --note 'Swamp camera rotation; record resolution, scaling, textures, cap and controller motion here'
```

At `FF7_READY`, focus the game and rotate through the view continuously until
the terminal says the capture completed. Allow at least 30 seconds of warm-up,
then 40 seconds of automatic logging. No MangoHud hotkey is needed. Logging is
scheduled relative to overlay initialization, so initialization delays can
lengthen warm-up. Return to the terminal only after logging has finished.

The script prints the archive to upload and allows you to close the game.
Launch again for each run. Start with one off/on pair to confirm logging and
activation work, then complete this order:

| Run | Steam launch arguments after `capture.py` |
| --- | --- |
| 1 | `launch off off-1 -- %command%` |
| 2 | `launch on on-1 -- %command%` |
| 3 | `launch on on-2 -- %command%` |
| 4 | `launch off off-2 -- %command%` |
| 5 | `launch off off-3 -- %command%` |
| 6 | `launch on on-3 -- %command%` |

Keep the same capture note and settings throughout. Report freezes, corruption,
unusual interruptions and remaining hitches, including failed runs. Do not
silently replace a slow run. Six runs give three observations per mode; they
do not establish performance on other GPUs or long-term stability.

Results are under `${XDG_STATE_HOME:-$HOME/.local/state}/ff7-sparse-vm-benchmarks`.
If Steam does not launch the game, read `last-error.txt` there. Each successful
launcher also records `launch-output.log` in its own directory. That full log
is excluded from the upload archive. A failed recording retains diagnostics
and requires a fresh launch before retrying.

The logger uses the private-config/inotify trigger previously exercised during
the investigation. This standalone packaged-driver wrapper has host checks but
has not yet been exercised on the target machine. It deliberately fails if the
installed kernel/driver, process, activation or complete log cannot be verified.

## Generate graphs

Install `python-matplotlib` on Arch, or Matplotlib in a Python environment.
The capture command does not need Matplotlib. Create a JSON manifest beside
the CSVs, with all runs listed in acquisition order:

```json
{
  "title": "FFVII Rebirth: sparse_vm off versus on",
  "subtitle": "GPU, resolution/scaling, texture quality, cap, save/view and software revisions",
  "duration_s": 40,
  "notes": "Fresh launches; same settings; at least 30 s warm-up; manual camera rotation.",
  "runs": [
    {"group": "Feature off", "label": "Off 1", "csv": "off-1.csv"},
    {"group": "Feature on", "label": "On 1", "csv": "on-1.csv"}
  ]
}
```

Include the other four captures in a completed comparison. Before publishing,
check the captured driver/game/Proton hashes, settings, activation, failures and
notes across runs. The plotter validates frame data and duration; it does not
establish matched settings or a causal effect by itself.

```sh
python3 benchmarks/plot.py /path/to/manifest.json --output /path/to/graphs
```

Outputs are SVG and PNG frame-time plots, percentile plots and `metrics.json`.
All recorded frames remain in the calculations and plots, with shared axes and
no smoothing or outlier removal. Manual rotations are not time-aligned between
runs. Percentiles use linear interpolation. Average FPS is frame count divided
by the sum of frame intervals, not the arithmetic mean of the CSV's FPS column.
Counts above 25, 33.333 and 50 ms describe long frames, not a claim that every
such frame is a discrete stutter. The 33.333 ms count is also reported per minute.

Each run is calculated separately. Summary markers show the median of the run
metrics, with min/max whiskers showing repeatability, not confidence intervals.
MangoHud measures application-side frame intervals, not physical panel scanout.
Keep raw frame data, the manifest, source hashes and the plotting command with
any graphs added to the main README. Publish only reviewed measurement fields,
not the complete diagnostic archives or process environment.

Host checks (including the real Linux inotify trigger and executable handoff):

```sh
python3 -m unittest discover -s benchmarks -p 'test_*.py' -v
```

[MangoHud logging options](https://github.com/flightlessmango/MangoHud#environment-variables)
document `autostart_log`, `log_duration`, `log_interval`, and `read_cfg`.
