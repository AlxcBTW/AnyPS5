# Minecraft Dungeons II navigation tools (lab only, not for upstream)

Scripts from the AlxcBTW lab that drive Minecraft Dungeons II (PPSA16064, 01.100.100) from boot to the first world with
scripted pad input. The gameplay rows on fork issue #56 and on the upstream PRs were measured with them. Windows,
PowerShell 5.1, Python 3 with `opencv-python`, `numpy` and `Pillow`.

| file | what it does |
|---|---|
| `md2-nav.ps1` | the row driver: gates (AC online, at least 12 GiB free RAM, no other `app.exe`), boot, screen-state navigation, cutscene skip, HUD check, walk, measurement windows; help block at the top |
| `md2-shot.py` | screenshot and screen-state classifier (mask of a fixed region, `cv2.matchTemplate`); `learn` makes a state from one of your screenshots, `match` checks one |
| `claude-probe.py` | one `PrintWindow` capture of the game window, used by `md2-shot.py` |
| `md2-fps.py` | per 10 s window: FPS, GPU busy, idle gaps and the `[gputime]` classes from a row's `stderr.log` |

## Layout they expect

`md2-nav.ps1` takes the workspace as two levels above itself, so copy the four scripts into `<workspace>/lab/scripts/`:

- `lab/runs/<run dir>`: a private copy of the dump (`app0`, `shader_cache`, `download0`) relinked to the libs under test;
  `-RunDir` must be under `lab/runs`;
- `lab/saves/<seed>`: save seeds (the `_sd` directory of a finished row);
- `lab/nav/md2/states.json` and the mask PNGs next to it: the screen states;
- results go to `lab/results/<tag>-<stamp>/` (`stderr.log`, `nav.log`, `pad.log`, `machine.csv`, shots, `end.json`).

## Not included, on purpose

The screen-state masks (`lab/nav/md2/*.png`, `states.json`) and our save seeds are made from the game and stay on our
machine. Learn the states from your own screenshots (look at a `shot-*.png`, then
`python -I lab/scripts/md2-shot.py learn <png> <state> <x0> <y0> <x1> <y1> [white|bright|gray|dark] [thr] [min]`).
The states the navigation uses: `title`, `accessibility`, `brightness`, `tos`, `autosave`, `mode` (attributes `_mode_on`,
`_mode_off`), `mainmenu`, `entering`, `loading`, `cutscene`, `hud` (two probes: minimap frame and health/ability bar).

## Input, both on `main`

- `APS5_PAD_INPUT_FILE`: the in-process pad command file (`docs/user/USAGE.md`, "Controller input for test runs";
  format of `tools/pad_input.py`). `md2-nav.ps1` writes it; no OS input is sent.
- `APS5_NO_PAD_INPUT=1`: keyboard and mouse events that reach the game window are not mapped onto the pad. Set it on
  every row, otherwise a stray Enter or click moves the game.

## Path to the first world

From an empty save: title "Press any button" (Cross), the first-run pages (Accessibility, Brightness, Terms, the autosave
notice, the mode select; Cross, Square where a page needs it), main menu, PLAY, loading screen, intro cutscene (Circle
skips it), then "Escape from Squid Coast" with the HUD. Keep the `_sd` of that row with `-SaveAs <name>`; later rows start
from it with `-SaveSeed <name>` and reach the HUD in 80-95 s.

On `main` the world load aborts ("storing storage image results pending over memory outside the write watch ...")
without #6308. Our gameplay numbers have #5372 as well. Budget on 8 GB cards, as measured on 16.10.:

- RTX 4060 8 GB, `main`'s automatic budget: nothing placed in host memory, and about +36 % in gameplay over a forced
  `APS5_TEXTURE_CACHE_MIB=3072`, which placed about 0.9 GB there.
- RTX 5070 Laptop 8 GB, the automatic budget of #6194 with the revision posted there: placements are rare, but in 2 of 11
  gameplay rows the budget's evictions re-created a 256 MiB 3D storage texture in host memory, and a compute pass over
  it ran into a Windows GPU timeout (device lost). With #4472 that happened in 0 of 5 rows.
- At the title, `main`'s automatic budget is too small on 8 GB cards; #6194 with the revision grows it.

So for gameplay rows on 8 GB, leave `APS5_TEXTURE_CACHE_MIB` unset and include #4472.

Example row:

```
powershell -NoProfile -ExecutionPolicy Bypass -File lab/scripts/md2-nav.ps1 -Tag g1 -RunDir lab/runs/md2-x `
  -LibsDir lab/libs-x -Phase Gameplay -SaveSeed md2-gameplay -WalkSeconds 180 -Seconds 420 `
  -ExtraEnv "APS5_NO_PAD_INPUT=1;APS5_PROFILE_DRAW=1;APS5_PROFILE_GPU=1"
python -I lab/scripts/md2-fps.py lab/results/g1-<stamp>
```

`-LibsDir` only records the libs' hash in `end.json`; relink the run dir yourself before the row.
