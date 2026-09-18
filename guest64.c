#include <stdint.h>
__attribute__((noreturn)) void _start(void) {
    const char *s = "Hello from long mode\n";
    while (*s)
        __asm__ volatile("outb %0, %1" ::"a"(*s++), "Nd"((unsigned short) 0x3f8));

    // Printing a string does not prove long mode; the same C would work in 32-bit. Add one line to
    // the guest that can only work in 64-bit
    uint64_t x = 0x1122334455667788ULL;
    __asm__ volatile("movq %0, %%r15" ::"r"(x)); /* r15 does not exist in 32-bit */
    for (;;)
        __asm__ volatile("hlt");
}