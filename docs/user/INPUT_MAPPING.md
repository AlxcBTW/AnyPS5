# Controller, keyboard and mouse input

AnyPS5 accepts SDL-mapped game controllers as well as its built-in keyboard and mouse bindings. Xbox-style controllers and PlayStation controllers recognized by SDL use the standard PS button layout; sticks and analog triggers are passed through, and controllers can be connected or disconnected while the game is running. The first recognized controller is used. AnyPS5 uses its built-in keyboard and mouse bindings when no configuration file is present. To change selected keyboard and mouse bindings, create `anyps5-input.ini` beside the generated game executable. Set `ANYPS5_INPUT_CONFIG` to use a file at another path.

For an Xbox-layout controller, A/B/X/Y map to Cross/Circle/Square/Triangle, LB/RB map to L1/R1, and LT/RT map to analog L2/R2. Start maps to Options. D-pad and stick-click buttons are supported. On a PlayStation controller, the touchpad click button is forwarded; finger touch coordinates are not implemented.

Each non-empty line has the form `Action = Type:Value`, or `Setting = number` for the [keyboard stick](#analog-keyboard-sticks) and [mouse](#mouse-as-the-right-stick) settings. Action names are case-insensitive. A `#` or `;` starts a comment. The first line for an action replaces its built-in bindings; later lines for the same action add alternate inputs. Duplicate entries for the same action are ignored. Actions omitted from the file keep their built-in bindings. UTF-8 files with or without a byte-order mark are supported.

Supported input sources are:

- `KEY:Return`, `KEY:Space`, or another key name accepted by SDL.
- `MOUSE:Left`, `MOUSE:Middle`, `MOUSE:Right`, `MOUSE:X1`, or `MOUSE:X2`.
- `WHEEL:Up` or `WHEEL:Down` for pad buttons.

Game-controller bindings are enabled automatically and are not affected by keyboard or mouse overrides in this file.

Supported actions are `Cross`, `Circle`, `Triangle`, `Square`, `L1`, `R1`, `L2`, `R2`, `L3`, `R3`, `Options`, `Up`, `Right`, `Down`, `Left`, `LeftStickLeft`, `LeftStickRight`, `LeftStickUp`, `LeftStickDown`, `RightStickLeft`, `RightStickRight`, `RightStickUp`, `RightStickDown`, `TouchLeft`, `TouchRight`, `StickModifier`, `ToggleMouse`, and `ToggleFullscreen`.

For example, this changes Cross to F or Space, moves the left stick to IJKL, uses the mouse buttons for Square and R2, and keeps all other built-in bindings:

```ini
Cross = KEY:F
Cross = KEY:Space
LeftStickLeft = KEY:J
LeftStickRight = KEY:L
LeftStickUp = KEY:I
LeftStickDown = KEY:K
Square = MOUSE:Left
R2 = MOUSE:Right
ToggleMouse = MOUSE:Middle
```

An invalid line reports the file and line number and stops input initialization. If the configured file does not exist or cannot be read, AnyPS5 reports an error. With no `anyps5-input.ini` and no `ANYPS5_INPUT_CONFIG`, the built-in mapping is used.

## Per-game profiles

To change bindings or settings for one game only, create `anyps5-input.<TITLE ID>.ini` beside the game executable, named after the title ID in the game's `sce_sys/param.json`, for example `anyps5-input.PPSA01234.ini`. AnyPS5 reads `anyps5-input.ini`, or the `ANYPS5_INPUT_CONFIG` file, first and the profile after it, so the profile only needs the lines that differ. The profile's first line for an action replaces that action's bindings, and its settings replace the shared ones. The log lists every input file it read.

## Analog keyboard sticks

A stick key tilts its stick fully. Hold `StickModifier` (Z by default) to tilt only part of the way, for walking or for fine flight control. Each stick axis is independent, so holding two neighbouring keys tilts both axes fully. Holding opposite keys centers that axis, and releasing one of them tilts toward the other.

Settings go in the same file as the bindings, one `Setting = number` per line. Setting names are case-insensitive, and each may appear once in a file.

| Setting | Default | Range | Effect |
| --- | --- | --- | --- |
| `StickModifierScale` | `0.5` | 0.05 to 1 | Tilt while `StickModifier` is held, as a fraction of full tilt. |
| `KeyboardRampMs` | `0` | 0 to 2000 | Milliseconds a stick key takes to tilt from center to full, and to return. 0 tilts at once; a ramp lets a short tap give a small tilt. |

For example, for a flight game where throttle and yaw sit on the left stick:

```ini
KeyboardRampMs = 150
StickModifierScale = 0.35
```

## Mouse as the right stick

`ToggleMouse` (the middle mouse button by default) captures the mouse and drives the right stick from how fast the mouse moves. Capture uses raw relative input, so the pointer speed and acceleration set in the operating system don't apply. Press it again, or switch to another window, to release the mouse.

The stick tilts in the direction the mouse moves, and further the faster it moves. Any movement tilts the stick at least `MouseDeadzoneOffset`, so slow movement isn't lost inside the game's own deadzone. When the mouse stops, the stick eases back and is centered `MouseDecayMs` after the last movement. Speed is measured in counts per second over the time between updates, so the same hand speed gives the same tilt at any frame rate. A mouse that reports only 125 or 250 times per second lands an uneven number of reports in each frame, which makes a steady movement waver; raise `MouseSmoothingMs` to 20 or 25 for such a mouse.

The mouse settings go in the same file:

| Setting | Default | Range | Effect |
| --- | --- | --- | --- |
| `MouseSensitivity` | `1.0` | 0.01 to 100 | At 1.0, moving the mouse 3000 counts per second tilts the stick fully. That is about 5 cm per second at 1600 DPI. |
| `MouseVerticalScale` | `1.0` | 0.1 to 10 | Multiplies vertical mouse speed, for games that pitch slower or faster than they turn. |
| `MouseCurve` | `1.0` | 0.2 to 5 | Response exponent. Above 1 gives finer control at low speed; below 1 reaches large tilts sooner. |
| `MouseDeadzoneOffset` | `0.2` | 0 to 0.9 | Smallest tilt any movement produces, as a fraction of full tilt. Set it just above the game's own deadzone: raise it if slow movement doesn't move the camera, lower it if small movements jump. |
| `MouseMaxSpeed` | `1.0` | 0.1 to 10 | Highest tracked speed, as a multiple of the full-tilt speed. Above 1, a fast flick keeps the stick at full tilt a little longer after the mouse stops. |
| `MouseSmoothingMs` | `10` | 0 to 500 | Smoothing time constant in milliseconds. 0 follows every mouse report directly. |
| `MouseDecayMs` | `30` | 1 to 1000 | Milliseconds without movement after which the stick is centered. |

For example, for a game with a small deadzone that needs fast turns:

```ini
MouseSensitivity = 1.6
MouseDeadzoneOffset = 0.12
MouseCurve = 1.3
```

Earlier versions sampled the mouse every 33 ms and tilted the stick at least halfway for any movement. `MouseDeadzoneOffset = 0.5`, `MouseSensitivity = 0.9`, `MouseSmoothingMs = 0` and `MouseDecayMs = 33` come close to that behaviour.

## Tuning without a game

Builds configured with `-DBUILD_TESTING=ON` include `pad_viz` in `build/tests`. It opens a window that draws the two sticks, the triggers and the buttons as AnyPS5 reports them to a game, computed by the same code, so the keyboard and mouse settings can be tuned by hand. Point it at the game's directory to read the same files a game run reads:

```sh
pad_viz --dir <game directory> --title <TITLE ID> --deadzone 0.12
```

Middle-click captures the mouse, as in a game. Saving one of the `.ini` files updates the window. The shaded disc marks the game's deadzone, given with `--deadzone` (default 0.1), and the dashed ring marks `MouseDeadzoneOffset`. The settings panel lists the files in effect and every setting, shows the mouse speed in counts per second next to the speed the stick tracks as a fraction of full tilt, and names the line of an `.ini` file that has an error.

`--record <file>` writes the input events to a text trace. The `pad_keyboard_mouse_tests` program replays a trace through the same code and prints the pad state as CSV, so settings and frame rates can be compared offline:

```sh
pad_keyboard_mouse_tests --replay <file> --poll 16 --dir <game directory> --title <TITLE ID>
```

`--poll` sets how often, in milliseconds, the replay samples the pad, like a game's frame time. Both programs take `--config <ini>` to read a single file instead of a directory.
