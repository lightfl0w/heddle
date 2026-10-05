[bits 32]
global _start
section .text
_start:
    mov esp, 0x90000
    extern kmain
    call kmain
.hang:
    cli
    hlt
    jmp .hang
