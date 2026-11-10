@ Q565 encoder (codec.h) in ARM assembly for the 3DS's ARM11 (ARMv6K: no UBFX / MOVW). Same output
@ as codec_q565_encode, byte for byte (tests/arm: checked under ARM emulation on real frames).
@
@   size_t codec_q565_encode_arm(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride,
@                                uint8_t *out, size_t cap);
@
@ Registers in the pixel loop: r0 pixel pointer, r1 the column's end, r2 out, r3 limit (out + cap - 4),
@ r4 the 64-colour table (u16, on the stack), r5 previous pixel, r6 run, r7 hash constant, r8 pixel;
@ r9-r12, lr scratch. Stack: table [sp, #0..127], columns left [sp, #128], column bytes [sp, #132],
@ stride [sp, #136], out start [sp, #140], column start [sp, #144].

    .arm
    .syntax unified
    .section .text.codec_q565_encode_arm, "ax", %progbits
    .global codec_q565_encode_arm
    .type codec_q565_encode_arm, %function
    .align 2
codec_q565_encode_arm:
    push    {r4-r11, lr}
    sub     sp, sp, #152
    ldr     r12, [sp, #188]         @ out
    ldr     lr, [sp, #192]          @ cap
    cmp     lr, #4
    blo     .Lfail
    str     r1, [sp, #128]
    lsl     r2, r2, #1
    str     r2, [sp, #132]
    str     r3, [sp, #136]
    str     r12, [sp, #140]
    str     r0, [sp, #144]
    add     r3, r12, lr
    sub     r3, r3, #4              @ limit
    mov     r2, r12                 @ out
    @ the table, the previous pixel and the run start at 0
    mov     r5, #0
    mov     r6, #0
    mov     r8, #0
    mov     r9, #0
    mov     r10, sp
    add     r11, sp, #128
1:  stmia   r10!, {r5, r6, r8, r9}
    cmp     r10, r11
    blo     1b
    mov     r4, sp
    ldr     r7, =0x9E3779B1
    cmp     r1, #0
    beq     .Ldone
    ldr     r1, [sp, #132]
    add     r1, r0, r1              @ end of the first column

.Lpix:
    cmp     r0, r1
    bhs     .Lcolumn
    ldrh    r8, [r0], #2
    cmp     r8, r5
    beq     .Lsame
    cmp     r2, r3                  @ room for a run flush and the largest op
    bhi     .Lfail
    subs    r12, r6, #1             @ a run before this pixel: flush it
    orrpl   r12, r12, #0xC0
    strbpl  r12, [r2], #1
    movpl   r6, #0
    mul     r9, r8, r7
    lsr     r9, r9, #26             @ hash
    add     r10, r4, r9, lsl #1
    ldrh    r11, [r10]
    cmp     r11, r8
    bne     .Lnew
    strb    r9, [r2], #1            @ INDEX
    mov     r5, r8
    b       .Lpix

.Lnew:
    strh    r8, [r10]
    lsr     r9, r8, #11
    sub     r9, r9, r5, lsr #11     @ dr (wrapped below)
    lsl     r10, r8, #21
    lsl     r11, r5, #21
    lsr     r10, r10, #26
    sub     r10, r10, r11, lsr #26  @ dg
    and     r11, r8, #31
    and     r12, r5, #31
    sub     r11, r11, r12           @ db
    mov     r5, r8                  @ previous pixel: this one
    add     r12, r9, #2
    and     r12, r12, #31           @ ur
    add     lr, r11, #2
    and     lr, lr, #31             @ ub
    add     r8, r10, #2
    and     r8, r8, #63             @ ug
    cmp     r12, #4
    cmplo   lr, #4
    cmplo   r8, #4
    bhs     .Lluma
    orr     r12, lr, r12, lsl #4    @ DIFF
    orr     r12, r12, r8, lsl #2
    orr     r12, r12, #0x40
    strb    r12, [r2], #1
    b       .Lpix

.Lluma:
    add     r12, r10, #32
    and     r12, r12, #63           @ gb = dg + 32
    mov     r8, r12, lsr #1
    sub     r8, r8, #16             @ half = floor(dg / 2)
    sub     r9, r9, r8
    add     r9, r9, #8
    and     r9, r9, #31             @ lr
    sub     r11, r11, r8
    add     r11, r11, #8
    and     r11, r11, #31           @ lb
    cmp     r9, #16
    cmplo   r11, #16
    bhs     .Lraw
    orr     r12, r12, #0x80         @ LUMA
    strb    r12, [r2], #1
    orr     r9, r11, r9, lsl #4
    strb    r9, [r2], #1
    b       .Lpix

.Lraw:
    mov     r12, #0xFE              @ RAW
    strb    r12, [r2], #1
    strb    r5, [r2], #1
    lsr     r12, r5, #8
    strb    r12, [r2], #1
    b       .Lpix

.Lsame:
    add     r6, r6, #1
    cmp     r6, #62
    bne     .Lpix
    add     r12, r3, #4             @ out + cap
    cmp     r2, r12
    bhs     .Lfail
    mov     r12, #0xFD              @ RUN of 62
    strb    r12, [r2], #1
    mov     r6, #0
    b       .Lpix

.Lcolumn:
    ldr     r12, [sp, #128]
    subs    r12, r12, #1
    beq     .Ldone
    str     r12, [sp, #128]
    ldr     r0, [sp, #144]
    ldr     r12, [sp, #136]
    add     r0, r0, r12             @ next column
    str     r0, [sp, #144]
    ldr     r12, [sp, #132]
    add     r1, r0, r12
    b       .Lpix

.Ldone:
    cmp     r6, #0
    beq     2f
    add     r12, r3, #4
    cmp     r2, r12
    bhs     .Lfail
    sub     r6, r6, #1
    orr     r6, r6, #0xC0
    strb    r6, [r2], #1
2:  ldr     r0, [sp, #140]
    sub     r0, r2, r0
    add     sp, sp, #152
    pop     {r4-r11, pc}

.Lfail:
    mov     r0, #0
    add     sp, sp, #152
    pop     {r4-r11, pc}

    .ltorg
    .size codec_q565_encode_arm, . - codec_q565_encode_arm
