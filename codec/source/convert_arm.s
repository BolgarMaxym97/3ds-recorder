@ One BGR8 column (bytes B, G, R) to RGB565 in ARM assembly for the 3DS's ARM11 (ARMv6K SIMD): the
@ same output as fb_column_to_rgb565(dst, src, FB_BGR8, rows) (convert.h), byte for byte (tests/arm:
@ checked under ARM emulation). The C one loads every byte on its own.
@
@   void fb_column_bgr8_arm(uint16_t *dst, const uint8_t *src, uint32_t rows);
@   rows % 4 == 0, src and dst 4-byte aligned (the caller checks: convert.h).
@
@ Four pixels a step: 12 bytes = 3 words w0 w1 w2, in 16-bit lanes (low lane = pixels 0 and 1, high
@ lane = pixels 2 and 3, as in c420_arm.s):
@   x = pkhbt(w0, w1) = B0 G0 | B2 G2,  z = pkhbt(w2, w0) = R2 B3 | R0 B1,  v = pkhbt(w1, w2) = G1 R1 | G3 R3
@ Each channel comes out of them with one AND of a shifted or rotated word, already in place:
@   b >> 3 = 0x001F001F & (x >> 3) (pixels 0, 2), & (z ror 27) (pixels 1, 3)
@   g & 0xFC = 0x00FC00FC & (x >> 8), & v
@   r & 0xF8 = 0x00F800F8 & (z ror 16), & (v >> 8)
@ pixel = (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3, two pixels a register.
@
@ Registers: r0 dst, r1 src, r2 end of src, r3 0x001F001F, r12 0x00FC00FC, lr 0x00F800F8, r4-r10 work.

    .arm
    .syntax unified
    .section .text.fb_column_bgr8_arm, "ax", %progbits
    .global fb_column_bgr8_arm
    .type fb_column_bgr8_arm, %function
    .align 2
fb_column_bgr8_arm:
    push    {r4-r10, lr}
    add     r2, r2, r2, lsl #1
    add     r2, r1, r2              @ end: src + rows * 3
    cmp     r1, r2
    beq     2f
    ldr     r3, =0x001F001F
    ldr     r12, =0x00FC00FC
    ldr     lr, =0x00F800F8
1:  ldmia   r1!, {r4, r5, r6}
    pld     [r1, #96]
    pkhbt   r7, r4, r5              @ x
    pkhbt   r8, r6, r4              @ z
    pkhbt   r9, r5, r6              @ v
    @ pixels 0 and 2
    and     r4, r3, r7, lsr #3      @ b
    and     r5, r12, r7, lsr #8     @ g
    and     r6, lr, r8, ror #16     @ r
    orr     r4, r4, r5, lsl #3
    orr     r4, r4, r6, lsl #8
    @ pixels 1 and 3
    and     r5, r3, r8, ror #27     @ b
    and     r6, r12, r9             @ g
    and     r10, lr, r9, lsr #8     @ r
    orr     r5, r5, r6, lsl #3
    orr     r5, r5, r10, lsl #8
    @ dst: pixel 0 | pixel 1 << 16, pixel 2 | pixel 3 << 16
    pkhbt   r6, r4, r5, lsl #16
    pkhtb   r7, r5, r4, asr #16
    stmia   r0!, {r6, r7}
    cmp     r1, r2
    blo     1b
2:  pop     {r4-r10, pc}
    .ltorg
    .size fb_column_bgr8_arm, . - fb_column_bgr8_arm
