# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A minimal Type-II VMM (hypervisor) written directly against the Linux KVM API, built incrementally by following the LWN article ["Using the KVM API"](https://lwn.net/Articles/658511/). The entire implementation currently lives in `vmm.c`, built up step by step in the order the article introduces them: open `/dev/kvm` → `KVM_GET_API_VERSION` → `KVM_CHECK_EXTENSION`(`KVM_CAP_USER_MEMORY`) → `KVM_CREATE_VM` → (upcoming) guest memory mapping, vcpu creation, and the run loop.

`code[]` in `vmm.c` is raw x86 machine code (not C) that will eventually run *inside* the guest VM — it writes a byte to I/O port `0x3f8` (COM1 serial), which is how the guest prints output back to the host in the article's example.

## Build

```sh
gcc -g -O0 -Wall -Wextra -std=gnu17 -o vmm vmm.c
```

- Always keep `-Wall -Wextra` on.
- Use `-std=gnu17` (GNU dialect), not plain `c17`/`c99`. Strict ISO mode hides POSIX/GNU-only declarations behind glibc feature-test macros (e.g. `O_CLOEXEC` from `<fcntl.h>` disappears under `-std=c17`), even though it compiles fine under gcc's actual default dialect.
- `compile_flags.txt` (for clangd) and `.vscode/c_cpp_properties.json` (for the Microsoft C/C++ extension) both pin `gnu17` so editor diagnostics match what `gcc` actually accepts — keep them in sync if the build flags change.

VS Code: press F5 (`.vscode/launch.json` + `.vscode/tasks.json` build with `-g -O0` and launch gdb automatically).

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
