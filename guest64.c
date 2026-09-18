__attribute__((noreturn)) void _start(void) {
    const char *s = "Hello from long mode\n";
    while (*s)
        __asm__ volatile("outb %0, %1" ::"a"(*s++), "Nd"((unsigned short) 0x3f8));
    for (;;)
        __asm__ volatile("hlt");
}