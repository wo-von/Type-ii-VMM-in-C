CC = gcc
CFLAGS = -g -O0 -Wall -Wextra -std=gnu17

GUEST_CFLAGS = -m64 -ffreestanding -fno-pie -no-pie -nostdlib -mno-red-zone \
	-fno-stack-protector -fno-asynchronous-unwind-tables
GUEST_LDFLAGS = -Wl,--build-id=none -Wl,-Ttext=0x3000

vmm: vmm.c
	$(CC) $(CFLAGS) -o $@ $<

guest64.elf: guest64.c
	$(CC) $(GUEST_CFLAGS) $(GUEST_LDFLAGS) -o $@ $<

guest64.bin: guest64.elf
	objcopy -O binary -j .text -j .rodata $< $@

guest: guest64.bin

clean:
	rm -f vmm guest64.elf guest64.bin

.PHONY: guest clean
