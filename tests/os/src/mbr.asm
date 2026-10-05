%include "src/mbr.inc"
[bits 16]
[org 0x7c00]
start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    mov si, msg
.print:
    lodsb
    or al, al
    jz .load
    mov ah, 0x0e
    int 0x10
    jmp .print
.load:
    mov ah, 0x02
    mov al, 8
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov bx, 0x1000
    int 0x13
    jmp 0x1000
msg: db "heddle-mbr", 0
times 510-($-$$) db 0
dw 0xaa55
