# flipper-tools

Bench utilities for embedded electronics and hardware hacking, as external
applications (FAP) for the Flipper Zero. Written in C, built with uFBT,
developed and used on [Momentum](https://momentum-fw.dev) firmware (dev channel).

Designed for bench use: a single screen, D-pad only, no nested menus.

## Apps

| App | Description | Version |
|-----|-------------|---------|
| [Base Converter](base_converter/) | 32-bit BIN / OCT / HEX / ASCII / DEC converter with per-digit editing | 1.1 |
| [Float Converter](float_converter/) | IEEE 754 binary32 / binary16 / bfloat16: BIN / HEX / fields / DEC | 1.0.1 |
| [Signed Converter](signed_converter/) | 8/16/32-bit two's complement: BIN / HEX / unsigned / signed DEC | 1.0 |

Each folder has its own README with the controls table and the edge-case semantics.

## Build

```sh
pipx install ufbt
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel=dev   # if Momentum

cd base_converter       # or float_converter / signed_converter
ufbt                    # build -> dist/<app>.fap
ufbt launch             # build + upload via USB + run
```

The apps show up in Apps > Tools.

## Compatibility

A FAP is compatible if the firmware API version has the same major and a minor
greater than or equal to the one the app was built against. On a mismatch,
align SDK and firmware (`ufbt update` / `ufbt flash_usb`).
Releases state the API version used for the build.

## Note on previews

The `preview_*.png` images are approximate renderings produced by a host-side
harness: the font is not the real Flipper profont, so widths may differ
slightly from the real screen.

## License

[MIT](LICENSE) © 2026 Francesco Not
