; test.asm — GYGAX64 red-dot demo
;
; Draws a single red pixel in the center of the 64x64 "GYGAX64" framebuffer
; (mem[$0400 .. $0400+4095], one byte per pixel, palette index 1 = red),
; then idles. gygax_coco treats a self-loop (BRA to itself) as "done" and
; renders the final framebuffer.
;
; Assemble with:  ./assemble.sh test.asm -o coco3agi.dsk
; Run with:       ./build/gygax_coco coco3agi.dsk

SCREEN  EQU     $0400
CENTER  EQU     2080            ; row 32, col 32  ->  32*64 + 32

        ORG     $2000           ; keep code out of the $0400-$13FF framebuffer
START   LDB     #1              ; palette index 1 = red
        STB     SCREEN+CENTER
LOOP    BRA     LOOP
        END     START
