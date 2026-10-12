.set noreorder
.set noat
.text
.align 4

# i am very new to this, so please forgive me if anything is wrong i am still learning a lot about vu0 and mmi but from what i have studied i believe this is correct

.globl Audio_MixSamples
.ent Audio_MixSamples
Audio_MixSamples:
    andi    $8, $6, 0xffff
    pcpyld  $8, $8, $8
    pcpyh   $8, $8

    la      $9, .LmixConstants
    lq      $10, 0($9)
    lq      $11, 16($9)
    lq      $12, 32($9)
    lq      $13, 48($9)

    slti    $2, $7, 4
    bnez    $2, .LmixDone
    nop

.LmixLoop:
    ld      $14, 0($4)
    ld      $15, 0($5)

    pmulth  $24, $14, $8
    pmflo   $2
    pmfhi   $3
    pcpyld  $24, $3, $2

    pmulth  $25, $15, $10
    pmflo   $2
    pmfhi   $3
    pcpyld  $25, $3, $2

    paddw   $24, $24, $25
    paddw   $24, $24, $11
    psraw   $24, $24, 15
    pmaxw   $24, $24, $12
    pminw   $24, $24, $13
    ppach   $14, $0, $24
    sd      $14, 0($5)

    addiu   $4, $4, 8
    addiu   $5, $5, 8
    addiu   $7, $7, -4
    slti    $2, $7, 4
    beqz    $2, .LmixLoop
    nop

.LmixDone:
    jr      $31
    nop
.end Audio_MixSamples

.section .rodata
.align 4
.LmixConstants:
    .half 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff
    .word 0x00004000, 0x00004000, 0x00004000, 0x00004000
    .word 0xffff8000, 0xffff8000, 0xffff8000, 0xffff8000
    .word 0x00007fff, 0x00007fff, 0x00007fff, 0x00007fff

.text
.align 4
.globl Audio_MixEnvelopeBlock8
.ent Audio_MixEnvelopeBlock8
Audio_MixEnvelopeBlock8:
    lw      $8, 0($4)
    lw      $9, 4($4)
    lw      $10, 8($4)
    lw      $11, 12($4)
    lw      $12, 16($4)

    lhu     $13, 20($4)
    pcpyld  $13, $13, $13
    pcpyh   $13, $13
    lhu     $14, 22($4)
    pcpyld  $14, $14, $14
    pcpyh   $14, $14
    lhu     $15, 24($4)
    pcpyld  $15, $15, $15
    pcpyh   $15, $15
    lhu     $25, 26($4)
    li      $24, 2

.LenvLoop:
    ld      $6, 0($8)

    pmulth  $2, $6, $13
    pmflo   $2
    pmfhi   $3
    pcpyld  $2, $3, $2
    psraw   $2, $2, 16
    ppach   $7, $0, $2

    pmulth  $2, $6, $14
    pmflo   $2
    pmfhi   $3
    pcpyld  $2, $3, $2
    psraw   $2, $2, 16
    ppach   $4, $0, $2

    pmulth  $2, $7, $15
    pmflo   $2
    pmfhi   $3
    pcpyld  $2, $3, $2
    psraw   $2, $2, 16
    ppach   $5, $0, $2

    pmulth  $2, $4, $15
    pmflo   $2
    pmfhi   $3
    pcpyld  $2, $3, $2
    psraw   $2, $2, 16
    ppach   $6, $0, $2

    ld      $2, 0($9)
    paddsh  $2, $2, $7
    sd      $2, 0($9)
    ld      $2, 0($10)
    paddsh  $2, $2, $4
    sd      $2, 0($10)

    bnez    $25, .LenvSwap
    nop
    ld      $2, 0($11)
    paddsh  $2, $2, $5
    sd      $2, 0($11)
    ld      $2, 0($12)
    paddsh  $2, $2, $6
    sd      $2, 0($12)
    b       .LenvAdvance
    nop

.LenvSwap:
    ld      $2, 0($11)
    paddsh  $2, $2, $6
    sd      $2, 0($11)
    ld      $2, 0($12)
    paddsh  $2, $2, $5
    sd      $2, 0($12)

.LenvAdvance:
    addiu   $8, $8, 8
    addiu   $9, $9, 8
    addiu   $10, $10, 8
    addiu   $11, $11, 8
    addiu   $12, $12, 8
    addiu   $24, $24, -1
    bgtz    $24, .LenvLoop
    nop

    jr      $31
    nop
.end Audio_MixEnvelopeBlock8
