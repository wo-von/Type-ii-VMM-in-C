# Type-ii-VMM-in-C

A minimal Type-II hypervisor in **~300 lines of C**, written directly against
the Linux KVM ioctl API. There is no QEMU, no libvirt and no library between
this code and `/dev/kvm`. It creates a VM, builds x86-64 page tables by hand,
programs the control registers and segment state for **64-bit long mode**,
loads a freestanding C guest, and then **proves** the guest really ran in
long mode by reading a 64-bit-only register back from the host.

Started from LWN's ["Using the KVM API"](https://lwn.net/Articles/658511/)
(16-bit real mode) and extended to long mode.

```
$ make && ./vmm
rip = 3000
rsp = 400000
rflags = 2
rax = 0
cr0 = 80000003, cr3 = 0, cr4 = 20
efer = 500
cs base=0 limit=FFFFFFFF type=B l=1 db=0
ss base=0 limit=FFFFFFFF type=3 l=1 db=0
PML4[0] = 1003 (RW=1 US=0) -> PDPT @ 1000
  PDPT[0] = 2003 (RW=1 US=0) -> PD @ 2000
    PD[0] = 83 (RW=1 US=0 PS=1) -> 2MiB page @ 0
    PD[1] = 200083 (RW=1 US=0 PS=1) -> 2MiB page @ 200000
Hello from long mode
r15 = 1234605616436508552
assertion passed
KVM_EXIT_HLT
```

> **Known issue:** on newer kernels (seen on Debian 6.12.115) `KVM_SET_SREGS` fails with
> `EINVAL`. See [Known issue: long mode rejected on newer kernels](#known-issue-long-mode-rejected-on-newer-kernels).

## Highlights

- **Raw KVM API.** Every step is a direct ioctl: `KVM_CREATE_VM`,
  `KVM_SET_USER_MEMORY_REGION`, `KVM_CREATE_VCPU`, `KVM_SET_SREGS`/`KVM_SET_REGS`,
  `KVM_RUN`. Every return value is checked.
- **Hand-built 4-level paging.** PML4 → PDPT → PD, with two 2 MiB pages that
  identity-map the guest's 4 MiB of RAM.
- **Long-mode bring-up without a bootloader.** `CR0.PE|PG`, `CR4.PAE`,
  `EFER.LME|LMA` and a 64-bit code segment (`L=1, D=0`) are loaded straight
  into the vCPU. The guest's first instruction already runs in 64-bit mode.
- **Real C guest.** `guest64.c` is compiled freestanding (`-ffreestanding
  -mno-red-zone -nostdlib`), linked at `0x3000`, converted to a flat binary
  with `objcopy`, and loaded at runtime.
- **A host-side proof of 64-bit mode.** See below.
- **Debug tooling.** `dump_state()` prints the registers, the segment
  descriptors, and the guest page tables walked from `CR3`.

## How it works

```
 host process (vmm)                          KVM (kernel)           guest
 ──────────────────                          ────────────           ─────
 open /dev/kvm, KVM_GET_API_VERSION == 12
 KVM_CREATE_VM ───────────────────────────▶  vm fd
 mmap 4 MiB, load guest64.bin at +0x3000
 KVM_SET_USER_MEMORY_REGION (GPA 0 ↔ mmap)
 KVM_CREATE_VCPU, mmap struct kvm_run
 build PML4/PDPT/PD in guest RAM
 KVM_SET_SREGS (CR0/CR3/CR4/EFER, CS/DS…)
 KVM_SET_REGS  (RIP=0x3000, RSP=0x400000)
 loop: KVM_RUN ───────────────────────────▶  VM entry ────────────▶ outb 0x3f8
        ◀── KVM_EXIT_IO (port 0x3f8) ──────  VM exit  ◀──────────── (×21 chars)
        putchar(byte)
       KVM_RUN ───────────────────────────▶                        movq $…, %r15
        ◀── KVM_EXIT_HLT ──────────────────  VM exit  ◀──────────── hlt
 KVM_GET_REGS, assert r15 == 0x1122334455667788
```

### Guest-physical memory layout

Identity-mapped 1:1 onto offsets in the host's `mmap`'d region (`MEM_SIZE` = 4 MiB):

```
0x000000   PML4                    4 KiB
0x001000   PDPT                    4 KiB
0x002000   PD                      4 KiB   (two 2 MiB PS=1 entries)
0x003000   guest code (.text)      <- RIP starts here
0x004000   guest .rodata
...
0x400000   top of RAM              <- RSP starts here, stack grows down
```

### What the r15 assertion proves

Printing a string does not prove 64-bit mode, because the same C would run
in 32-bit protected mode too. `%r15` is one of the registers `r8`–`r15`,
which exist only when the CPU is actually in long mode. The guest `movq`s
`0x1122334455667788` into it and halts. The host then reads it back with
`KVM_GET_REGS` and asserts the value. The round trip succeeds only if the
page tables, `CR0`/`CR3`/`CR4`/`EFER` and the segment descriptors are all
correct.

## Measurements

Measured on 2026-11-13. Host: Intel Core i7-9750H (12 threads), Linux
6.12.115 (Debian 13), EPT and unrestricted guest enabled. Built with `-O0 -g`.

| What | Value | How |
|---|---|---|
| VMM source | 307 lines (`vmm.c`), 12 lines guest (`guest64.c`) | `wc -l` |
| Guest code size | **74 B** `.text` + 22 B `.rodata` | `size -A guest64.elf` |
| Guest image on disk | 4118 B (mostly padding: `.rodata` is page-aligned at `0x4000`) | `ls -l guest64.bin` |
| KVM ioctls per run | 37 total, 22 of them `KVM_RUN` | `strace -e trace=ioctl` |
| VM exits per run | 22 = 21 `KVM_EXIT_IO` (one per character) + 1 `KVM_EXIT_HLT` | same |
| `KVM_RUN` round trip (enter guest → `outb` → exit to userspace) | ~10–50 µs, median 16–44 µs depending on the run | `strace -T`; strace adds overhead, so this is an upper bound |
| `KVM_CREATE_VM` | 0.2–0.5 ms | `strace -T` |
| `KVM_CREATE_VCPU` | 0.5–1.0 ms | `strace -T` |
| CPU time, whole process | **3.7 ms** (±3%) | `perf stat -r 50` (task-clock) |
| Wall time, whole process | **42 ms** (±3%), min 24 ms over 300 runs | `perf stat -r 50`, Python loop |
| Process teardown after `exit_group` | ~40–50 ms | `strace -ttt`: `exit_group` → reaped |

**Takeaways**

- **Teardown dominates wall time.** The process uses less than 4 ms of
  CPU but takes about 42 ms of wall time. Most of the gap comes *after*
  `exit_group`, while the kernel tears down the VM. I haven't profiled the
  exact cause; it needs root to trace, e.g. with `perf trace` or
  `funcgraph`.
- **Every port write costs a full exit to userspace.** Printing 21 bytes
  costs 21 round trips through `KVM_RUN`. That's why real VMMs use
  `rep outsb` (one exit per string), MMIO with coalescing, or virtio rings
  that batch I/O through shared memory.
- The per-exit number above is measured *through strace*. Measuring it
  properly (e.g. `rdtsc` around `KVM_RUN` in a tight loop, without strace)
  is a natural next step.

## Build and run

```sh
make        # builds vmm, guest64.elf, guest64.bin
./vmm       # needs /dev/kvm: be in the `kvm` group, or use sudo
```

Other targets: `make vmm`, `make guest`, `make clean`. Run from the repo root,
because `vmm` loads `./guest64.bin` at startup.

The host is built with `-g -O0 -Wall -Wextra -std=gnu17`. The guest uses a
separate freestanding flag set (`-m64 -ffreestanding -fno-pie -no-pie
-nostdlib -mno-red-zone -fno-stack-protector`, linked with `-Ttext=0x3000`).
See `Makefile` and `CLAUDE.md` for why each flag is there.

## Repository layout

| File | Purpose |
|---|---|
| `vmm.c` | The VMM: VM/vCPU setup, page tables, long-mode register state, run loop |
| `guest64.c` | Freestanding 64-bit guest: prints over port `0x3f8`, sets `r15`, halts |
| `vmm_hello_world.c` | Frozen checkpoint: the LWN article's 16-bit real-mode guest |
| `assets/long_mode_roadmap.md` | Long-mode bring-up guide written against ARMv8 MMU concepts (paging, EPT, GDT, CR*/EFER) |
| `Makefile` | Host and guest builds |

## Known issue: long mode rejected on newer kernels

On Linux 6.12.115 (Debian), `./vmm` stops with:

```
vmm: KVM_SET_SREGS failed: Invalid argument
```

The real-mode `vmm_hello_world.c` still runs on the same kernel, so KVM
itself works. The VMM never calls `KVM_SET_CPUID2`, so the vCPU advertises
an empty CPUID, which means no "long mode supported" bit. The kernel checks
`EFER.LME` against the guest's CPUID and rejects the sregs. A test build that
copies the host's supported CPUID into the vCPU (`KVM_GET_SUPPORTED_CPUID` →
`KVM_SET_CPUID2`) before `KVM_SET_SREGS` boots all the way to the r15
assertion. The measurements above come from that build. The fix belongs in
`vmm.c` and hasn't been applied yet.

## What this does not do

This is a learning project, not a hypervisor for real workloads.

- **No interrupts.** There's no IDT, so any guest exception triple-faults
  (`KVM_EXIT_SHUTDOWN`).
- **One hardcoded device.** `KVM_EXIT_IO` handles only a 1-byte `OUT` to
  `0x3f8`. Anything else is a fatal `errx()`.
- **No MMIO.** `KVM_EXIT_MMIO` falls into the `default` case and aborts.
  Accesses outside the identity-mapped 4 MiB are fatal.
- **Single vCPU, single memory slot.** No SMP and no hot-plug.
- **Flat binary, not a bootable image.** There's no ELF loader, no boot
  protocol, and no way to boot Linux.
- **No virtio, networking or disk. No save/restore, live migration or
  nested virtualization.**

Each of these is a separate piece of a real hypervisor (device models,
interrupt controllers, boot protocols, guest scheduling) that this project
hasn't started yet.

## References

- LWN, [Using the KVM API](https://lwn.net/Articles/658511/)
- Linux kernel, [`Documentation/virt/kvm/api.rst`](https://docs.kernel.org/virt/kvm/api.html)
- AMD64 Architecture Programmer's Manual Vol. 2, ch. 14 (long-mode initialization)
- Intel SDM Vol. 3A, ch. 4 (paging) and §10.8 (initializing IA-32e mode)
