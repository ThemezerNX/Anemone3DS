# quirc

QR code recognition, used by the QR code scanner (`source/services/qr.cpp`). By Daniel Beer, from
https://github.com/dlbeer/quirc, under the ISC license (`LICENSE`, and the notice at the top of each file).

## Version

quirc 1.1 (upstream tag `v1.1`, commit `2e8c4ce`, October 2019): the files of upstream's `lib/`. `quirc_version()`
says "1.0" because upstream didn't change that string for 1.1.

The files are copied unmodified from Anemone3DS's `source/quirc`, which differs from upstream 1.1 in three small ways
(Anemone3DS commits `aa3a00c`, `d358dd1` and `b75d1ac`):

- `identify.c`: `FLOOD_FILL_MAX_DEPTH` is 128 instead of 4096. The flood fill is recursive, and 4096 levels overflowed
  the stack of a 3DS thread.
- `identify.c`: `(void)y;` in `area_count`, and `quirc.c`: no `err >= 0` test in `quirc_strerror` (the enum is never
  negative). Both silence compiler warnings.

The Makefile builds these files with warnings off.

## What 1.1 lacks

- `quirc_extract` doesn't check that a grid fits in `cell_bitmap`, and a misread timing pattern can make a grid bigger
  than any QR code. `qr.cpp` skips such grids before extracting them, which is why it includes `quirc_internal.h`.
- There is no `quirc_flip` for mirrored codes; `qr.cpp` transposes the cells itself.

quirc 1.2 has both, and a flood fill that doesn't recurse, so an update would no longer need the depth change.
