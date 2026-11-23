# Float Converter — Flipper Zero

IEEE 754 binary32 (`f32`), binary16 (`f16`) and bfloat16 (`bf16`).

| Row  | Content |
|------|---------|
| BIN  | 2 rows; small gap between nibbles, wide gap at the sign / exponent / mantissa boundaries |
| HEX  | hexadecimal + format selector `[ f32]` `[ f16]` `[bf16]` |
| info | label = class (NRM, SUB, ZRO, INF, NaN); content = `2^E x significand`, `±0` or NaN payload (quiet/sig) |
| DEC  | scientific notation with the minimum digits for round-trip (f32: 9, f16: 5, bf16: 4) |

- The FPU is single precision only: `double` is emulated in software (the `__aeabi_d*`
  symbols are resolved when linking FAPs, verified). Used only where the precision is needed.

## Build

    ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel=dev   # if Momentum
    cd float_converter
    ufbt launch

## Controls

| Mode   | Up / Down             | Left / Right   | OK         | Back      |
|--------|-----------------------|----------------|------------|-----------|
| SELECT | change field          | -              | enter EDIT | exit app  |
| EDIT   | +/- digit (wrapping)  | move cursor    | exit EDIT  | exit EDIT |

Navigation order: BIN, HEX, format, DEC (the info row is not selectable).
Long OK (both modes) = clear, with a short vibration.
Long press on the arrows = autorepeat.

## Decimal entry

Editing DEC modifies a `±d.dddddddd E±ee` buffer (the cursor starts on the first mantissa digit;
the mantissa sign and the exponent sign are toggled with Up/Down).
On every change the buffer is converted to the nearest float (round to nearest, ties to even)
and BIN/HEX/info update. While editing, DEC shows what you typed, with `~` if the float is
not exactly that value (e.g. 0.1); on leaving edit mode DEC shows the float's actual value
(`+1.00000001E-01`). Overflow -> ±INF, underflow -> subnormal or ±0.

## Semantics

- Format change: like a cast (value preserved and rounded RNE; overflow -> ±INF;
  NaN -> quiet NaN with the payload truncated to the high bits).
- No libm dependency: encode and decode work on the bits of the double.

## Verification

- Encode/decode checked against an exact reference (Python `Fraction`): 1.2 M cases,
  exhaustive decode for f16/bf16, exact-tie cases and cases within ±1 ulp, overflow/subnormal edges.
- f32 encode identical to the hardware `(float)double` cast on 2 M random doubles.
- DEC -> bits round-trip exhaustive on f16/bf16 and on 2 M f32 patterns.
