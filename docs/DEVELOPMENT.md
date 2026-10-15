# Development

How to build Themezer-3DS, try it without a console, and look at its screens without pressing the buttons yourself.
[ARCHITECTURE.md](ARCHITECTURE.md) explains how the code is laid out.

## Building and testing

The README lists what to install. Then:

```sh
make            # out/Themezer.3dsx and out/Themezer.cia
make test       # the unit tests of source/core and source/domain, on your computer
```

The tests need a C++23 compiler and libpng; they don't need devkitARM.

## Trying it in Azahar

[Azahar](https://azahar-emu.org) runs the 3dsx on a computer. With it installed, `make preview` builds the app and
opens it. The emulator's SD card is a folder: on a Mac `~/Library/Application Support/Azahar/sdmc`, on Linux
`~/.local/share/azahar-emu/sdmc`. Put themes in its `Themes` folder as you would on a console.

Azahar isn't a console, and the app looks a little different in it:

- Without the console's own system font, Azahar uses a replacement whose letters are wider and spaced differently.
  Check how text fits on a console.
- Azahar ignores the tint citro2d gives pictures, so the icons are white in it instead of their colours.
- The HOME Menu's theme storage and the badge storage don't exist until a console has used them, so installing
  themes and badges ends in an error message there.

## The autopilot

The autopilot plays a script of button presses and touches, and saves screenshots of both screens as PNGs. It's built
into the app only with `DEVTOOLS=1`, into `build/dev` and `out/dev`, apart from the normal build:

```sh
make DEVTOOLS=1 3dsx
tools/autopilot.sh tools/autopilot/tour.txt
```

`tools/autopilot.sh` puts the script on Azahar's SD card, opens the app, waits for the script to end, and copies the
screenshots to `out/dev/shots`. `tools/autopilot/tour.txt` walks past the main screens without changing anything.

On a console, the app reads the script from `/3ds/Themezer/autopilot.txt` when it starts (send `out/dev/Themezer.3dsx`
with `3dslink`), and saves the screenshots in `/3ds/Themezer/autopilot/`. A script that installs something installs it
for real there.

A script has one command a line; `#` starts a comment. Times are in frames, 60 a second.

| Command | What it does |
| --- | --- |
| `wait 30` | nothing, for 30 frames |
| `press A`, `press L+R` | the buttons for a frame, then a frame without them: A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT |
| `hold A 60` | the buttons held for 60 frames |
| `touch 160 120 [frames]` | a tap on the touch screen at (160, 120), one frame long unless frames says |
| `drag 160 200 160 60 [frames]` | a touch moving from the first point to the second, in 20 frames unless frames says |
| `shot name` | both screens as they are now, in `name.png` |
| `log text` | a line in the app's log |
| `glyphs` | a page of the console font's symbols, U+E000 to U+E0FF (B closes it) |
| `dumpfont` | the console's system font, as `shared_font.bin` |
| `savelog` | the app's last log lines, as `log.txt` |
| `quit` | closes the app |

When the script ends, the app writes an empty `done` file next to the screenshots, and goes on as usual.
