minih264e.h: lieff/minih264 (master, 2026-11-05), CC0 1.0 (LICENSE).
Local change: `__3DS__` added to the little-endian platform list (marked in the file).
Local changes for the ARM11 (marked "3ds-screen-recorder" in the file), the plain C path:
- `copy_wh` copies words instead of bytes;
- `sad_block` sums 4 pixels at a time, with the ARMv6 `USADA8` instruction where it exists
  (`__ARM_FEATURE_SIMD32`); `MINIH264_USADA8_C` forces its C twin (bit-exact checks on a PC).
