# Signed Converter — Flipper Zero

Two's complement converter for 8 / 16 / 32 bit.

| Row | Content |
|-----|---------|
| BIN | 2 rows (bits 31..16 / 15..0), LSB right-aligned |
| HEX | hexadecimal + width selector `[ 8b]` `[16b]` `[32b]` |
| UNS | unsigned decimal |
| SGN | signed decimal (two's complement), sign editable like a digit |

## Build

    ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel=dev   # if Momentum
    cd signed_converter
    ufbt launch

## Controls

| Mode   | Up / Down             | Left / Right   | OK         | Back      |
|--------|-----------------------|----------------|------------|-----------|
| SELECT | change field          | -              | enter EDIT | exit app  |
| EDIT   | +/- digit (wrapping)  | move cursor    | exit EDIT  | exit EDIT |

Navigation order: BIN, HEX, width, UNS, SGN.
Long OK (both modes) = clear, with a short vibration.
Long press on the arrows = autorepeat.

## Semantics

- Width change: the signed value is recomputed in the new width
  (sign extension when widening, truncation when narrowing — like a C cast).
- SGN, sign position: Up/Down negate the value; the minimum (-128, -32768, -2^31)
  stays unchanged, like NEG in hardware. On 0 nothing happens (no -0).
- Digits that would take the value out of range (UNS > 2^N-1, SGN > 2^(N-1)-1 or < -2^(N-1))
  are skipped while cycling.
- Zeroing the magnitude of a negative number gives 0 (sign +).
