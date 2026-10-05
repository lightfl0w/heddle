static volatile unsigned short *vga = (volatile unsigned short *)0xb8000;

void kmain(void) {
    const char *msg = "heddle kernel v4";

    for (int i = 0; msg[i]; i++)
        vga[i] = (unsigned short)((0x0f << 8) | msg[i]);

    for (;;)
        ;
}
