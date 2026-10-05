ENTRY(reset_handler)
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
SECTIONS
{
    .text : { *(.text*) } > FLASH
    .data : { *(.data*) } > RAM
}
