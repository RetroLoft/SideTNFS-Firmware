    section text
    xdef _start,_r03_tos,_rload_ticks
    xref _r03_test_main
_start:
    move.l 4(sp),a0
    lea stack_end,sp
    move.l #$100,d0
    add.l $c(a0),d0
    add.l $14(a0),d0
    add.l $1c(a0),d0
    move.l d0,-(sp)
    move.l a0,-(sp)
    clr.w -(sp)
    move.w #$4a,-(sp)
    trap #1
    lea 12(sp),sp
    jsr _r03_test_main
    pea done_message
    move.w #9,-(sp)
    trap #1
    addq.l #6,sp
    move.w #7,-(sp)
    trap #1
    addq.l #2,sp
    clr.w -(sp)
    trap #1
; C ABI: packed GEMDOS argument buffer pointer, byte count (32-bit).
_r03_tos:
    move.l 4(sp),a0
    move.l 8(sp),d0
    movem.l d1-d7/a0-a6,-(sp)
    move.l sp,a6
    suba.l d0,sp
    move.l sp,a1
    subq.w #1,d0
.copy:
    move.b (a0)+,(a1)+
    dbf d0,.copy
    trap #1
    move.l a6,sp
    movem.l (sp)+,d1-d7/a0-a6
    rts
; Read the TOS 200 Hz uptime counter in supervisor mode.
; Preserve C callee-saved registers across the XBIOS call.
_rload_ticks:
    movem.l d1-d7/a0-a6,-(sp)
    pea read_ticks(pc)
    move.w #38,-(sp)
    trap #14
    addq.l #6,sp
    movem.l (sp)+,d1-d7/a0-a6
    rts
read_ticks:
    move.l $4ba.w,d0
    rts
    section data
done_message: dc.b "Press a key to return to desktop.",13,10,0
    even
    section bss
stack: ds.b 8192
stack_end:
