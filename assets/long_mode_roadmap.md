# Stage 2 — Getting the guest into 64-bit long mode

You already know all of this from ARM. The concepts are identical; only the
names and the historical baggage are different. This document maps the x86
vocabulary onto what you already own, then gives you an ordered checklist with
success indicators and failure symptoms.

---

## Part 1 — The concepts

### 1.1 Why this stage exists at all

On the SAMA5D36 you brought up, the CPU comes out of reset in a defined state.
You write `TTBR0`, `TCR`, `MAIR`, then set `SCTLR.M` and the MMU is on. Clean,
one page of code, done.

x86 comes out of reset **pretending to be an Intel 8086 from 1978**. 16-bit
registers, one megabyte of addressable memory, segment:offset addressing. Your
`Hi!` guest ran in that mode. To reach 64-bit you have to walk the CPU forward
through a historical re-enactment of the entire x86 lineage:

| Mode | Year | Width | Notes |
|---|---|---|---|
| Real mode | 1978 | 16-bit | address = segment × 16 + offset. No protection. |
| Protected mode | 1985 | 32-bit | segments become table indices. Paging optional. |
| Long mode | 2003 | 64-bit | paging **mandatory**. Segmentation mostly vestigial. |

A real bootloader has to execute that sequence instruction by instruction. It
cannot skip steps.

**You are not a bootloader.** You are the machine. `KVM_SET_SREGS` lets you
write the CPU's architectural state directly, so you can construct the exact
register state the guest *would* have ended up in and start it there. The
re-enactment is skipped entirely.

> This asymmetry — the VMM can construct architectural state that the guest
> could only reach through a sequence of instructions — is worth being able to
> say out loud. It is the difference between someone who read a tutorial and
> someone who understands what a hypervisor is.

### 1.2 Paging — identical to ARM, different names

ARM 4 KiB granule, 48-bit VA: `TTBR0` → L0 → L1 → L2 → L3 → 4 KiB page.
Stop early at L2 and you get a 2 MiB block descriptor.

x86-64, 48-bit VA: `CR3` → PML4 → PDPT → PD → PT → 4 KiB page.
Stop early at PD (set the `PS` bit) and you get a 2 MiB page.

Same tree. Same 9 + 9 + 9 + 9 + 12 bit split of the virtual address. Same idea
that a table entry holds the *physical* address of the next table plus some
flags.

The flags you need:

| Bit | Name | ARM equivalent |
|---|---|---|
| 0 | P — present | valid bit |
| 1 | R/W — writable | AP bits |
| 2 | U/S — user accessible | AP bits |
| 7 | PS — page size (at PD level: "this is a 2 MiB page, stop walking") | block descriptor |

**Use 2 MiB pages.** Three levels instead of four, and one PD with 512 entries
covers a full gigabyte. Less code to get wrong.

**Identity map**: make virtual address == physical address. The guest then
doesn't have to know translation is happening at all.

### 1.3 The virtualization twist — two-dimensional paging

This is the concept that matters most for a Nitro conversation, so slow down
here.

The guest builds page tables that map **guest virtual** → **guest physical**.
It thinks guest-physical addresses are real RAM. They are not. Guest physical
address 0 is just an offset into the anonymous `mmap` you made in your VMM
process.

So the hardware performs a **second** translation: guest-physical →
host-physical, walking a second set of page tables called EPT (Intel) or NPT
(AMD). You never write those. The kernel builds them from your
`KVM_SET_USER_MEMORY_REGION` call — that ioctl is precisely "here is the
backing for this range of guest physical address space."

Consequence worth internalising: on a TLB miss, the hardware walks *both*
trees, nested. Each of the 4 levels of the guest walk is itself a
guest-physical address that must be translated through the 4-level EPT. That is
up to **24 memory accesses for a single TLB miss**, against 4 on bare metal.

This is why huge pages matter more inside a VM than outside it, why page-walk
caches are a real architectural feature and not a footnote, and why "why is my
database slower in a VM" usually has this answer. Good thing to bring up
unprompted.

### 1.4 Segmentation — the genuinely weird part

There is no ARM analogue. This is pure x86 archaeology, but you cannot skip it
because the `L` bit lives here.

x86 has six segment registers: `CS` (code), `DS`, `ES`, `SS` (stack), `FS`,
`GS`.

- **Real mode**: `CS = 0x1000` literally means "add 0x10000 to every code
  address."
- **Protected and long mode**: `CS = 0x08` means "index 1 in the GDT."

The **GDT** (Global Descriptor Table) is an array in memory pointed at by the
`GDTR` register. Each entry — a *descriptor* — carries a base address, a limit,
a type (code or data), a privilege ring, and, critically for you, the **`L`
bit**: "this is 64-bit code."

Now the part that confuses everybody:

> When you load a segment register, the CPU copies the descriptor out of the
> GDT into a **hidden cache** attached to that register. Every subsequent
> address translation reads the hidden cache. The GDT in memory is consulted
> *only* at load time.

A real bootloader must build a GDT, execute `LGDT`, then perform a far jump to
force `CS` to reload from it. Fiddly.

**You can write the hidden cache directly.** `struct kvm_segment` inside
`kvm_sregs` *is* the hidden cache. Set `base = 0`, `limit = 0xffffffff`,
`l = 1`, and you are done. **You do not need a GDT in guest memory at all.**

The `selector` field you set is cosmetic at this point — it is what the guest
would read back if it inspected the register. Set it to something plausible
(`1 << 3` for CS, `2 << 3` for data) and move on.

### 1.5 Control registers

| Register | Bit | Meaning | ARM equivalent |
|---|---|---|---|
| `CR0` | 0 — PE | protection enable | — |
| `CR0` | 31 — PG | paging enable | `SCTLR.M` |
| `CR3` | — | physical address of the PML4 | `TTBR0` |
| `CR4` | 5 — PAE | physical address extension, **mandatory** for long mode | part of `TCR` |
| `EFER` | 8 — LME | long mode enable | — |
| `EFER` | 10 — LMA | long mode *active* — normally set by the CPU | — |

`EFER` is a model-specific register, not a control register, but KVM exposes it
in the same `kvm_sregs` struct so in practice you treat it the same way.

Set all of these in one `KVM_SET_SREGS` call. KVM validates the combination and
will reject inconsistent state — which is a feature, because it fails fast
instead of triple-faulting.

---

## Part 2 — The roadmap

### Memory layout

Pick this and write it down as a comment at the top of your VMM. Half of all
bugs in this stage are "the address I told the CPU is not the address I put the
thing at."

```
guest physical
0x000000   PML4      4 KiB
0x001000   PDPT      4 KiB
0x002000   PD        4 KiB
0x003000   guest code blob        <- RIP starts here
...
0x400000   top of RAM             <- RSP starts here, stack grows down
```

`MEM_SIZE` becomes 4 MiB. The PD needs exactly two entries: one 2 MiB page at
0x000000, one at 0x200000. That covers all backed memory and nothing else, so
any stray access lands as a clean `KVM_EXIT_MMIO` instead of silently working.

---

### Step 0 — Build the diagnostics first

**Do this before anything else.** Long mode fails silently and unhelpfully. If
you build the debugger after the bug, you will spend an evening guessing.

Write one function:

```c
static void dump_state(int vcpufd);
```

It calls `KVM_GET_REGS` and `KVM_GET_SREGS` and prints RIP, RSP, RFLAGS, RAX,
CR0, CR3, CR4, EFER, and the base/limit/type/`l`/`db` of CS and SS. Call it
from every unexpected exit branch in your switch.

Add a second function that walks the page tables you built, from the host side,
and prints each level's entry with its flags decoded. Twenty lines. It will pay
for itself the first time you run it.

**Success indicator:** running it right after `KVM_CREATE_VCPU` and before you
touch anything prints the CPU's reset state — CR0 = 0x60000010, RIP = 0xfff0,
CS base = 0xffff0000. If you see that, your plumbing works.

---

### Step 1 — Build the page tables

Write into the guest memory buffer from the host side. Remember: the *values*
you store are guest-physical addresses; the *pointers* you dereference are host
virtual.

```c
#define PDE64_PRESENT  (1U << 0)
#define PDE64_RW       (1U << 1)
#define PDE64_USER     (1U << 2)
#define PDE64_PS       (1U << 7)

uint64_t *pml4 = (void *)((uint8_t *)mem + 0x0000);
uint64_t *pdpt = (void *)((uint8_t *)mem + 0x1000);
uint64_t *pd   = (void *)((uint8_t *)mem + 0x2000);

pml4[0] = PDE64_PRESENT | PDE64_RW | 0x1000;              /* -> PDPT */
pdpt[0] = PDE64_PRESENT | PDE64_RW | 0x2000;              /* -> PD   */
pd[0]   = PDE64_PRESENT | PDE64_RW | PDE64_PS | 0x000000; /* 2 MiB   */
pd[1]   = PDE64_PRESENT | PDE64_RW | PDE64_PS | 0x200000; /* 2 MiB   */
```

**Success indicator:** your table-walking dump function prints two present 2 MiB
pages identity-mapping 0x000000 and 0x200000, and nothing else present.

**Watch for:** the addresses stored in entries must be 4 KiB aligned — the low
12 bits are flags, so a misaligned address silently corrupts them.

---

### Step 2 — Set up `kvm_sregs`

Read the current sregs first, modify, write back. Do not zero-initialise the
struct — there are fields you do not want to clear.

Control registers:

```c
sregs.cr3  = 0x0;                            /* PML4 lives at guest phys 0 */
sregs.cr4  = (1U << 5);                      /* PAE */
sregs.cr0  = (1U << 0) | (1U << 1) | (1U << 31); /* PE | MP | PG */
sregs.efer = (1U << 8) | (1U << 10);         /* LME | LMA */
```

Segments — one code descriptor, five data descriptors:

```c
struct kvm_segment seg = {
    .base     = 0,
    .limit    = 0xffffffff,
    .selector = 1 << 3,
    .present  = 1,
    .type     = 11,   /* code: execute / read / accessed */
    .dpl      = 0,
    .db       = 0,    /* MUST be 0 when l = 1 */
    .s        = 1,    /* code or data, not a system descriptor */
    .l        = 1,    /* 64-bit */
    .g        = 1,    /* limit is in 4 KiB units */
};
sregs.cs = seg;

seg.type     = 3;     /* data: read / write / accessed */
seg.selector = 2 << 3;
sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = seg;
```

**Success indicator:** `KVM_SET_SREGS` returns 0. If it returns `-EINVAL`, KVM
has rejected your combination as architecturally impossible — that is a gift,
read the constraint table below rather than guessing.

**Watch for:** `db = 1` together with `l = 1` is the single most common
mistake. It means "32-bit *and* 64-bit," which is nonsense, and it is what
`-EINVAL` is usually telling you.

---

### Step 3 — Set `kvm_regs`

```c
struct kvm_regs regs = {
    .rip    = 0x3000,     /* where you loaded the blob */
    .rsp    = 0x400000,   /* top of backed RAM, grows down */
    .rflags = 0x2,
};
```

**Watch for:** RIP must match the load address of your blob *and* the `-Ttext`
you passed to the linker. If those three numbers disagree, the CPU executes
whatever bytes happen to be there.

---

### Step 4 — Build and load a 64-bit guest

From the previous message:

```sh
gcc -m64 -ffreestanding -fno-pie -no-pie -nostdlib -mno-red-zone \
    -fno-stack-protector -fno-asynchronous-unwind-tables \
    -Wl,--build-id=none -Wl,-Ttext=0x3000 -o guest64.elf guest64.c
objcopy -O binary -j .text -j .rodata guest64.elf guest64.bin
```

Load `guest64.bin` at offset 0x3000 in your memory buffer.

Guest source — deliberately trivial, you are testing the mode transition, not
the program:

```c
__attribute__((noreturn)) void _start(void)
{
    const char *s = "Hello from long mode\n";
    while (*s)
        __asm__ volatile("outb %0, %1"
                         :: "a"(*s++), "Nd"((unsigned short)0x3f8));
    for (;;)
        __asm__ volatile("hlt");
}
```

**Success indicator:** `Hello from long mode` on stdout, followed by your
`[vmm] guest halted` line.

**Watch for:** `objcopy` output is larger than the code, because `.rodata` gets
page-aligned. That is correct — load the whole blob and the addresses line up.

---

### Step 5 — Prove it is actually in 64-bit mode

Printing a string does not prove long mode; the same C would work in 32-bit.
Add one line to the guest that can only work in 64-bit:

```c
uint64_t x = 0x1122334455667788ULL;
__asm__ volatile("movq %0, %%r15" :: "r"(x));   /* r15 does not exist in 32-bit */
```

Then read it back from the host with `KVM_GET_REGS` at the `hlt` exit. If
`regs.r15 == 0x1122334455667788`, you are unambiguously in long mode with a
64-bit register file.

**This is your stage-2 completion criterion.** Not the string.

---

## Part 3 — When it fails

It will. Here is what each failure actually means.

| Symptom | What it means | Where to look |
|---|---|---|
| `KVM_EXIT_SHUTDOWN` | **Triple fault.** The guest took an exception; you have no IDT so the handler faulted; that fault faulted. Any guest exception whatsoever ends here. | Page table flags; RIP pointing at garbage; stack not mapped |
| `KVM_EXIT_FAIL_ENTRY` + `hardware_entry_failure_reason` | The VMCS/VMCB state you built is architecturally invalid. The CPU refused to even start. | Inconsistent sregs — `db`+`l`, PG without PE, LME without PAE |
| `KVM_EXIT_INTERNAL_ERROR`, suberror 1 | Emulation failure. KVM tried to emulate an instruction and could not decode it. | RIP almost certainly points at data, not code |
| `KVM_EXIT_MMIO` at an address you expected to work | Guest touched memory outside your mapped slot, or outside your PD entries. | `MEM_SIZE`, and whether you mapped both 2 MiB pages |
| Hangs forever, no exit | Guest is in an infinite loop. | Did you actually emit `hlt`? Did RIP land mid-instruction? |
| Prints garbage, then halts | Blob loaded at the wrong offset, or `-Ttext` disagrees with RIP. | `objdump -D -b binary -m i386:x86-64 --adjust-vma=0x3000` |

### Kernel-side tracing

When the host-side dump is not enough:

```sh
sudo mount -t debugfs none /sys/kernel/debug     # if not already mounted
sudo trace-cmd record -e kvm ./vmm
sudo trace-cmd report | less
```

The events that matter: `kvm_entry` and `kvm_exit` bracket each `KVM_RUN`, and
`kvm_exit` carries the exit reason and guest RIP. `kvm_page_fault` on an address
you believed you mapped tells you instantly whether the problem is your guest
tables or your memory slot — which is otherwise very hard to distinguish.

If `trace-cmd` is not installed: `sudo perf stat -e 'kvm:*' ./vmm` gives you
exit counts by type, which is cruder but often enough.

---

## Part 4 — Where to read

**Primary, in order of usefulness for this stage:**

1. `Documentation/virt/kvm/api.rst` — sections on `KVM_SET_SREGS`,
   `KVM_SET_REGS`, `kvm_run`, and `KVM_SET_USER_MEMORY_REGION`. This is the
   contract; everything else is commentary.
2. **AMD64 Architecture Programmer's Manual, Vol. 2**, ch. 5 (page translation)
   and §14.6 (long mode initialisation). AMD's manual is consistently clearer
   than Intel's on exactly this transition, and it is free.
3. **Intel SDM Vol. 3A**, ch. 4 (paging) and §9.8.5 (initialising IA-32e mode).
   Use as cross-reference when AMD and reality disagree.
4. OSDev wiki: *Setting Up Long Mode*, *Paging*, *Global Descriptor Table*.
   Bootloader-oriented, so it does the full re-enactment you are skipping —
   but the flag tables are correct and quick to scan.

**Oracle only, per your own rule:** `github.com/dpw/kvm-hello-world` has
real-mode, protected-mode and long-mode variants of exactly this. Open it after
twenty minutes stuck, to check a struct field — not to start from.

**For the interview, not the code:** the Firecracker NSDI '20 paper's device
model section, and the Nitro security whitepaper. Read those on the train, not
at the keyboard.

---

## Part 5 — Scope discipline

When this stage passes, what you can honestly claim:

> Wrote a type-2 VMM against the raw KVM ioctl interface: guest memory slot
> management, x86-64 page table construction, long mode entry via direct
> architectural state injection, and PIO/MMIO exit handling.

What you cannot claim yet: virtio, kernel boot, SMP, live migration. Those come
later or not at all. The claim above is already enough to turn "no
virtualization experience" into a twenty-minute technical conversation, which
is the entire point of the project.
