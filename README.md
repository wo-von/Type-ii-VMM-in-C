# Type-ii-VMM-in-C

A minimal Type-II VMM (hypervisor) written directly against the Linux KVM
ioctl API — no libvirt, no QEMU, no abstraction layer between this code and
`/dev/kvm`. Started by following LWN's ["Using the KVM
API"](https://lwn.net/Articles/658511/) and built up from there: guest memory
mapping, vcpu creation, x86-64 page tables, and enough architectural state
(control registers, segment descriptors) to bring a guest up in full 64-bit
long mode and prove it.

`vmm_hello_world.c` is a frozen checkpoint of the article's original 16-bit
real-mode example (a guest that writes one byte to COM1 and halts) — kept for
comparison, not extended further. The active code is `vmm.c`.

## Memory layout

Guest-physical layout, identity-mapped 1:1 onto offsets into the host's
`mmap`'d backing memory (`MEM_SIZE` = 4 MiB):

```
guest physical
0x000000   PML4      4 KiB page map l4
0x001000   PDPT      4 KiB page directory pointer table
0x002000   PD        4 KiB page directory
0x003000   guest code blob        <- RIP (instruction pointer) starts here
...
0x400000   top of RAM             <- RSP (stack pointer) starts here, stack grows down
```

The guest at `0x003000` is `guest64.bin` — `guest64.c` compiled freestanding
for `-m64`, linked to load at that exact address, then stripped down to raw
`.text`/`.rodata` bytes with `objcopy`. `vmm.c` reads that file into guest
memory at startup; there's no compiled-in guest code any more.

## Build and run

```sh
make        # builds vmm, guest64.elf, and guest64.bin
sudo ./vmm  # needs /dev/kvm access — sudo, or be in the `kvm` group
```

Other Makefile targets: `make vmm`, `make guest` (builds just the guest
image), `make clean`. See `CLAUDE.md` for the exact compiler flags and why
each one is there (`-ffreestanding`, `-mno-red-zone`, `-Wl,-Ttext=0x3000`,
etc.).

## What the r15 assertion proves

On halt, `vmm.c` reads the vcpu's registers back with `KVM_GET_REGS` and
asserts that `%r15` holds exactly `0x1122334455667788` — a value `guest64.c`
`movq`s into that register right before it halts.

Printing a string doesn't prove the guest is in 64-bit mode; the same C would
compile and run under 32-bit protected mode too. `%r15` is different: it's one
of eight general-purpose registers (`r8`–`r15`) that only exist once the CPU
is genuinely in long mode. Round-tripping a specific, arbitrary 64-bit value
through it — set by the guest, read back by the host after real execution —
is the actual, unambiguous proof that every earlier step (page tables, `CR0`/
`CR3`/`CR4`/`EFER`, segment descriptors) landed correctly and the CPU is
running exactly the mode this VMM claims it built.

## What this does not do

This is a from-scratch learning project, not a hypervisor anyone should trust
with real workloads. Specifically, as of this writing:

- **No interrupts.** There's no IDT. Any guest exception — a fault, a
  divide-by-zero, anything — triple-faults immediately (`KVM_EXIT_SHUTDOWN`),
  because there's no handler for it to even attempt to run.
- **No devices beyond one hardcoded serial port.** `KVM_EXIT_IO` handling is
  special-cased to exactly `port == 0x3f8`, `size == 1`, direction `OUT`.
  Anything else is a fatal `errx()`. There is no device model.
- **No MMIO.** `KVM_EXIT_MMIO` isn't handled in the exit-reason switch at
  all — it falls through to the `default` case and aborts the VMM.
  Guest memory accesses outside the identity-mapped 2 MiB pages built by
  `build_page_tables()` are equally fatal, not gracefully faulted.
- **Single vcpu, single memory slot.** No SMP, no hot-plug, no multiple
  memory regions.
- **The guest is a flat binary loaded at a fixed address, not a bootable
  image.** There's no ELF loader, no bootloader, no way to run an actual
  kernel (Linux or otherwise) — `guest64.bin` is raw `.text`/`.rodata` bytes
  `objcopy`'d out of a freestanding ELF and read directly into place.
- **No virtio, no networking, no disk.** Nothing beyond the serial-port
  `outb` this VMM already implements.
- **No save/restore, no live migration, no nested virtualization.**

None of this is accidental — each is a deliberately unexplored piece of a much
larger surface (device models, interrupt controllers, EPT-based introspection,
guest scheduling) that a real hypervisor has to solve and this project
intentionally hasn't touched yet.
