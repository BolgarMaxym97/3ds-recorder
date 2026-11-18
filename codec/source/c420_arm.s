@ C420 encoder (codec.h) from a BGR8 source (bytes B, G, R) in ARM assembly for the 3DS's ARM11
@ (ARMv6K SIMD). The same output as codec_c420_encode_888(src, cols, rows, stride, 3, out), byte for
@ byte (tests/arm: checked under ARM emulation). About twice as fast: the C one spends ~120 cycles a
@ block on byte loads and spilled registers (O3DS bench, 2026-11-12: 10.8-12.2 ms a top frame).
@
@   size_t codec_c420_encode_bgr8_arm(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride,
@                                     uint8_t *out);
@   rows % 4 == 0, src and stride multiples of 4, out 4-byte aligned (the caller checks: codec.h).
@
@ Two blocks per iteration in 16-bit lanes: low lane = rows r, r+1, high lane = rows r+2, r+3.
@ 4 rows of a column = 12 bytes = 3 words w0 w1 w2:
@   pkhbt(w0, w1) = B0 G0 | B2 G2,  pkhbt(w2, w0) = R2 B3 | R0 B1,  pkhbt(w1, w2) = G1 R1 | G3 R3
@ uxtb16 (with ror) takes bytes 0 and 2 into the lanes.
@
@ Registers in the loop: r0 column a, r1 column b, r2 out, r3 end of column a, r11 0x07FF07FF;
@ r7 sum R, r8 sum B, r9 sum G, r10 y0 | y1 << 5 | y2 << 10; r4-r6, r12, lr work.

    .arm
    .syntax unified
    .section .text.codec_c420_encode_bgr8_arm, "ax", %progbits
    .global codec_c420_encode_bgr8_arm
    .type codec_c420_encode_bgr8_arm, %function
    .align 2
codec_c420_encode_bgr8_arm:
    push    {r4-r11, lr}
    sub     sp, sp, #24
    ldr     r12, [sp, #60]          @ out
    str     r12, [sp, #0]
    mov     r4, r2                  @ rows
    mov     r2, r12
    lsrs    r1, r1, #1              @ column pairs
    beq     .Lend
    add     r4, r4, r4, lsl #1      @ bytes per column
    str     r4, [sp, #16]
    str     r3, [sp, #8]
    ldr     r11, =0x07FF07FF
.Lpair:
    str     r1, [sp, #4]
    str     r0, [sp, #12]
    ldr     r3, [sp, #16]
    add     r3, r0, r3
    ldr     r1, [sp, #8]
    add     r1, r0, r1
.Lblk:
    @ column a: y0, y1, the sums start
    ldmia   r0!, {r4, r5, r6}
    pld     [r0, #96]
    pkhbt   r12, r4, r5
    pkhbt   lr, r6, r4
    pkhbt   r4, r5, r6
    uxtb16  r5, r12                 @ B0
    uxtb16  r12, r12, ror #8        @ G0
    uxtb16  r6, lr, ror #16         @ R0
    uxtb16  lr, lr, ror #24         @ B1
    add     r8, r5, lr              @ sB
    add     r5, r5, r6
    add     r5, r5, r12, lsl #1     @ L0
    and     r5, r11, r5, lsr #5
    usat16  r10, #5, r5             @ y0
    uxtb16  r5, r4                  @ G1
    uxtb16  r4, r4, ror #8          @ R1
    add     r7, r6, r4              @ sR
    add     r9, r12, r5             @ sG
    add     r4, r4, lr
    add     r4, r4, r5, lsl #1      @ L1
    add     r4, r4, #0x10
    add     r4, r4, #0x100000
    and     r4, r11, r4, lsr #5
    usat16  r4, #5, r4              @ y1
    orr     r10, r10, r4, lsl #5
    @ column b: y2, y3
    ldmia   r1!, {r4, r5, r6}
    pld     [r1, #96]
    pkhbt   r12, r4, r5
    pkhbt   lr, r6, r4
    pkhbt   r4, r5, r6
    uxtb16  r5, r12                 @ B2
    uxtb16  r12, r12, ror #8        @ G2
    uxtb16  r6, lr, ror #16         @ R2
    uxtb16  lr, lr, ror #24         @ B3
    add     r8, r8, r5
    add     r8, r8, lr
    add     r7, r7, r6
    add     r9, r9, r12
    add     r5, r5, r6
    add     r5, r5, r12, lsl #1     @ L2
    add     r5, r5, #0x10
    add     r5, r5, #0x100000
    and     r5, r11, r5, lsr #5
    usat16  r5, #5, r5              @ y2
    orr     r10, r10, r5, lsl #10
    uxtb16  r5, r4                  @ G3
    uxtb16  r4, r4, ror #8          @ R3
    add     r9, r9, r5
    add     r7, r7, r4
    add     r4, r4, lr
    add     r4, r4, r5, lsl #1      @ L3
    add     r4, r4, #8
    add     r4, r4, #0x80000
    and     r4, r11, r4, lsr #5
    usat16  r4, #5, r4              @ y3
    @ co = sat6((sR - sB + 1040) >> 5)
    add     r12, r7, r8
    add     r7, r7, #0x410
    add     r7, r7, #0x04100000
    sub     r7, r7, r8
    and     r7, r11, r7, lsr #5
    usat16  r7, #6, r7
    @ cg = sat6((2 sG - sR - sB + 2080) >> 6)
    add     r9, r9, #0x410
    add     r9, r9, #0x04100000
    rsb     r12, r12, r9, lsl #1
    lsr     r12, r12, #6
    bic     r12, r12, #0xFC00
    usat16  r12, #6, r12
    orr     r7, r7, r12, lsl #6     @ h = co | cg << 6 (12 bits a lane)
    @ word = y0 | y1 << 5 | y2 << 10 | y3 << 15 | h << 20, low lane then high lane
    pkhbt   r5, r10, r7, lsl #20
    and     r6, r4, #31
    orr     r5, r5, r6, lsl #15
    lsl     r7, r7, #4
    pkhtb   r6, r7, r10, asr #16
    lsr     r4, r4, #16
    orr     r6, r6, r4, lsl #15
    stmia   r2!, {r5, r6}
    cmp     r0, r3
    blo     .Lblk
    ldr     r0, [sp, #12]
    ldr     r1, [sp, #8]
    add     r0, r0, r1, lsl #1
    ldr     r1, [sp, #4]
    subs    r1, r1, #1
    bne     .Lpair
.Lend:
    ldr     r0, [sp, #0]
    sub     r0, r2, r0
    add     sp, sp, #24
    pop     {r4-r11, pc}
    .ltorg
    .size codec_c420_encode_bgr8_arm, . - codec_c420_encode_bgr8_arm
