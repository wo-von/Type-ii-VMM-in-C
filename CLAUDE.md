# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A minimal Type-II VMM (hypervisor) written directly against the Linux KVM API, built incrementally by following the LWN article ["Using the KVM API"](https://lwn.net/Articles/658511/). The active work lives in `vmm.c`. `vmm_hello_world.c` is a frozen checkpoint of the earlier 16-bit real-mode stage (the article's original example) kept for reference/comparison — it is not being extended further. See `README.md` for the full picture (memory layout, build/run, what's proven, what's explicitly not implemented).

**Stage 2 (bringing the guest up into 64-bit long mode, per `assets/long_mode_roadmap.md`) is complete.** `vmm.c` has: identity-mapped page table construction (`build_page_tables`), control-register/segment-descriptor setup for long mode (`set_regs`), a `dump_state`/`dump_page_tables` debugging pair, a runtime guest-image loader (`load_guest_image`, reads a compiled binary off disk into guest memory — there is no compiled-in guest array any more), and a `KVM_RUN` exit-handling loop covering `KVM_EXIT_HLT`, `KVM_EXIT_IO`, `KVM_EXIT_FAIL_ENTRY`, `KVM_EXIT_INTERNAL_ERROR`. On halt, `prove_longmode()` reads `%r15` back via `KVM_GET_REGS` and asserts it matches a 64-bit constant the guest (`guest64.c`) `movq`'d into it — the actual proof of genuine 64-bit execution, not just the printed string.

### Memory layout (`vmm.c`)

Guest-physical layout, identity-mapped 1:1 onto offsets into the host `mmap`'d region (`MEM_SIZE` = 4 MiB):

```
0x000000   PML4
0x001000   PDPT
0x002000   PD
0x003000   guest code blob   <- RIP starts here
...
0x400000   top of RAM        <- RSP starts here, stack grows down
```

Per `assets/long_mode_roadmap.md`, most bugs at this stage are the address told to the CPU (in a page table entry, `sregs`, or `regs`) disagreeing with the address something was actually loaded/mapped at — check this layout against the code first when debugging a triple fault or `KVM_EXIT_FAIL_ENTRY`.

### `assets/long_mode_roadmap.md`

A self-contained guide for the long-mode stage (now complete, see above), written to map x86 long-mode bring-up onto ARM/AArch64 MMU concepts the user already knows: paging (PML4/PDPT/PD/PT vs ARM translation tables), two-dimensional paging (EPT/NPT vs guest page tables), x86 segmentation and the GDT "hidden cache" trick, and the CR0/CR3/CR4/EFER control registers. It also has an ordered implementation checklist with success indicators, a table mapping KVM exit reasons to likely causes, and pointers into `Documentation/virt/kvm/api.rst` and the AMD64/Intel SDM manuals — still worth pointing the user at the relevant section for any long-mode concept that needs re-deriving, and useful background for whatever stage comes next.

## Build

```sh
make        # builds vmm, guest64.elf, guest64.bin (default goal: `all`)
make vmm    # just the VMM
make guest  # just the guest image (guest64.c -> guest64.elf -> guest64.bin)
make clean
```

Equivalent to `make vmm`: `gcc -g -O0 -Wall -Wextra -std=gnu17 -o vmm vmm.c`.

- Always keep `-Wall -Wextra` on.
- Use `-std=gnu17` (GNU dialect), not plain `c17`/`c99`. Strict ISO mode hides POSIX/GNU-only declarations behind glibc feature-test macros (e.g. `O_CLOEXEC` from `<fcntl.h>` disappears under `-std=c17`), even though it compiles fine under gcc's actual default dialect.
- `compile_flags.txt` (for clangd) and `.vscode/c_cpp_properties.json` (for the Microsoft C/C++ extension) both pin `gnu17` so editor diagnostics match what `gcc` actually accepts — keep them in sync if the build flags change.
- `guest64.c` is compiled freestanding for `-m64` with a very different flag set (`-ffreestanding -fno-pie -no-pie -nostdlib -mno-red-zone -fno-stack-protector -fno-asynchronous-unwind-tables`, linked with `-Wl,-Ttext=0x3000` to match `CODE_ADDR`/`rip` in `vmm.c`) — see `Makefile`'s `GUEST_CFLAGS`/`GUEST_LDFLAGS`. It's a hosted-vs-freestanding distinction, not a style choice; don't merge the two flag sets.
- `vmm.c` loads the guest at runtime via `load_guest_image()` (`open`/`fstat`/`read` straight into guest memory) from `guest64.bin` in the current working directory — `make`/`./vmm` both assume you're running from the repo root.

VS Code: press F5 (`.vscode/launch.json` + `.vscode/tasks.json` build with `-g -O0` and launch gdb automatically).

Formatting is clang-format on save in VS Code (`.vscode/settings.json` + `.clang-format`: LLVM base, 4-space indent, 100-column limit) — keep new code consistent with that style even when editing outside VS Code.

## Run

```sh
sudo ./vmm
```

Requires access to `/dev/kvm` — run with `sudo` or be in the `kvm` group. Every KVM ioctl return value must be checked (`-1` = error); the existing code uses `err()`/`perror()` from `<err.h>` for this, which is the established pattern to follow when adding new ioctl calls.

## Headers

New KVM constants/ioctls (`KVM_*`) come from `<linux/kvm.h>` — this header is required and must not be dropped as the file grows.

## Working style — this is a learning project

The user is following the LWN article to learn the KVM API hands-on and wants to write the code themselves. Do not write or fix code for them by default. Instead:

- When something is broken, point at the symptom (compiler error, wrong output, which line/ioctl is involved) and ask guiding questions rather than supplying the fix. Only write the actual patch if they explicitly ask you to.
- When they hit an unfamiliar *API* (an ioctl, a struct, a flag), point them at where to find it themselves first — `man 2 <syscall>`, `man ioctl_list`, the kernel's `Documentation/virt/kvm/api.rst`, `/usr/include/linux/kvm.h` — rather than just stating the answer.
- When a step bundles several unfamiliar *concepts* at once (e.g. "walk the page table" when registers/paging/segmentation aren't yet solid), don't just point at docs and leave it there — a pile of unrelated-seeming unknowns causes the user to freeze up before starting. Proactively ground the specific concepts the step actually needs, concretely, in terms of this codebase's own addresses/layout, then hand the implementation back. Depth should be just enough to unblock the concrete next step, not a full treatise — and stay scoped to what's relevant: if something adjacent is unfamiliar but not needed for the current step, just name that it's out of scope rather than explaining it, since chasing irrelevant unknowns is as distracting as too much unrelated depth.
- It's fine to run diagnostic commands (compiling to surface the exact error, grepping headers) to ground your hints in fact — just don't turn the diagnosis into an unsolicited fix.
- Config/tooling scaffolding (`.vscode/*`, `compile_flags.txt`, `CLAUDE.md` itself) is fair game to edit directly — this preference is about the VMM code and the KVM learning content, not editor plumbing.
