# Controller and keyboard

The Windows build reads Xbox-compatible controllers through XInput. The first
controller and the keyboard share player 1; keyboard input adds to the controller.
The emulated Xbox USB controller is enabled by default for the game's XDK driver.
Keyboard events are accepted by the game window; click/focus that window first.

| Keyboard | Xbox control |
| --- | --- |
| Enter / Backspace | Start / Back |
| Arrow keys | D-pad |
| Z / X / C / V | A / B / X / Y |
| WASD | Left stick |
| IJKL | Right stick |
| Q / E | White / Black |
| 1 / 3 | Left / right trigger |
| Shift / Ctrl | Left / right stick click |

These are controller bindings, not claims about which action each button performs
in RedCard. Menu navigation is still being brought up.

## Verification

`build/Debug/redcard_recomp.exe --input-check` exercises press/release,
all keyboard bindings (including keypad alternatives), the host input merge and
the actual 20-byte Xbox USB report encoder. Results go to stdout/stderr. It
does not inject operating-system keyboard events or rumble the controller.

Latest check: 28 passed, zero failures. Windows detected a physical XInput
controller on port 0. Actual controller button presses and menu response remain
to be verified interactively. See `analysis/input-check.log`.

The USB trace completed standard descriptors, address/configuration setup and
the XID controller descriptor. A readback at guest 003BDD0C confirmed the exact
16-byte XID descriptor reached the driver's real buffer. No periodic report
polls were seen during these short intro runs; do not call input integration
complete until the game opens the pad and consumes reports.
