# Base Converter — Flipper Zero

Unsigned 32-bit BIN / OCT / HEX / ASCII / DEC converter with per-digit editing.

The ASCII field (4 bytes, MSB on the left) sits in the box on the HEX row.
Navigation order: BIN, OCT, HEX, ASCII, DEC.

## Build

    pipx install ufbt          # or: pip install --user ufbt
    cd base_converter
    ufbt                       # build -> dist/base_converter.fap
    ufbt launch                # build + upload via USB + run

For non-release firmware: `ufbt update --channel=dev` (or `--index-url` for Momentum/Unleashed).
The FAP ends up in Apps > Tools.

## Controls

| Mode   | Up / Down               | Left / Right      | OK         | Back       |
|--------|-------------------------|-------------------|------------|------------|
| SELECT | change base             | -                 | enter EDIT | exit app   |
| EDIT   | +/- digit (wrapping)    | move cursor       | exit EDIT  | exit EDIT  |

Long press on the arrows = autorepeat. EDIT is entered with the cursor on the LSB.
Digits that would exceed UINT32_MAX (MSB of OCT and DEC) are skipped.
In ASCII, Up/Down cycle only 0x20..0x7E (from a non-printable byte: Up -> ' ', Down -> '~');
non-printable bytes are shown as an empty box.
