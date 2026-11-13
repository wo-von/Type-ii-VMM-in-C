# TODO

## Next

- [ ] **Fix long-mode boot on newer kernels.** `KVM_SET_SREGS` returns `EINVAL` on 6.12.115:
      the vCPU has an empty CPUID, so it doesn't advertise long mode and the kernel rejects
      `EFER.LME`. Read up on `KVM_GET_SUPPORTED_CPUID` / `KVM_SET_CPUID2` in
      `Documentation/virt/kvm/api.rst` and decide where they go in the setup sequence.
      Done when `./vmm` reaches `assertion passed` again.
- [ ] **Update the README after the fix.** Delete the "Known issue" section and the callout
      near the top, then re-run the measurements with your own build and update the table.
- [ ] **Measure per-exit latency properly.** The current 10–50 µs number for one `KVM_RUN`
      round trip comes from `strace -T`, which inflates it. Time `KVM_RUN` with `rdtsc` (or
      `clock_gettime`) in a tight loop, without strace, and report min/median/p99.

## Later

- [ ] Find out why process teardown takes ~40 ms when the run itself uses only ~4 ms of CPU
      (needs root: `perf trace`, or `funcgraph` on the exit path).
