#include <err.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
/*
guest physical
0x000000   PML4      4 KiB page map l4
0x001000   PDPT      4 KiB page directory pointer table
0x002000   PD        4 KiB page directory
0x003000   guest code blob        <- RIP (instruction pointer) starts here
...
0x400000   top of RAM             <- RSP (stack pointer) starts here, stack grows down
*/
#define MEM_SIZE (1UL << 22) // 4MiB

const uint8_t code[] = {
    0xba, 0xf8, 0x03, /* mov $0x3f8, %dx */
    0x00, 0xd8,       /* add %bl, %al */
    0x04, '0',        /* add $'0', %al */
    0xee,             /* out %al, (%dx) */
    0xb0, '\n',       /* mov $'\n', %al */
    0xee,             /* out %al, (%dx) */
    0xf4,             /* hlt */
};

static void dump_state(int vcpufd) {
    struct kvm_regs reg;
    struct kvm_sregs sreg;
    ioctl(vcpufd, KVM_GET_REGS, &reg);
    ioctl(vcpufd, KVM_GET_SREGS, &sreg);
    printf("rip = %llx\n", reg.rip);
    printf("rsp = %llx\n", reg.rsp);
    printf("rflags = %llx\n", reg.rflags);
    printf("rax = %llx\n", reg.rax);
    printf("cr0 = %llx, cr3 = %llx, cr4 = %llx\n", sreg.cr0, sreg.cr3, sreg.cr4);
    printf("efer = %llx\n", sreg.efer);
    printf("cs base=%llx limit=%X type=%X l=%X db=%X", sreg.cs.base, sreg.cs.limit, sreg.cs.type,
           sreg.cs.l, sreg.cs.db);
    printf("ss base=%llx limit=%X type=%X l=%X db=%X", sreg.ss.base, sreg.ss.limit, sreg.ss.type,
           sreg.ss.l, sreg.ss.db);
}
int main() {
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm == -1) {
        err(1, "cannot open /dev/kvm");
    }

    int ret = ioctl(kvm, KVM_GET_API_VERSION, NULL);
    if (ret == -1) {
        err(1, "KVM_GET_API_VERSION ");
    }
    if (ret != 12) {
        err(1, "KVM_GET_API_VERSION expexted 12 got %d\n", ret);
    }

    ret = ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
    if (ret == -1) {
        err(1, "KVM_CHECK_EXTENSION");
    }
    if (!ret) {
        err(1, " required extension KVM_CAP_USER_MEMORY fails");
    }

    int vmfd = ioctl(kvm, KVM_CREATE_VM, (unsigned long) 0);

    void *mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (mem == MAP_FAILED) {
        err(1, "mmap failed");
    }
    memcpy(mem, code, sizeof(code));

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0x1000,
        .memory_size = MEM_SIZE,
        .userspace_addr = (uint64_t) mem,
    };

    ret = ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
    if (ret == -1) {
        err(1, "KVM_USER_MEMORY_REGION ioctl failed");
    }

    int vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, (unsigned long) 0);
    if (vcpufd == -1) {
        err(1, "VCPU KVM_VCPU_CREATE failed");
    }

    int mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL); // to communicate with the userspace
    struct kvm_run *run =
        (struct kvm_run *) mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
    // read the sregs and set cs to 0
    struct kvm_sregs sregs;
    ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    struct kvm_regs regs = {
        .rip = 0x1000,
        .rax = 2,
        .rbx = 2,
        .rflags = 0x2,
    };
    ioctl(vcpufd, KVM_SET_REGS, &regs);
    while (1) {
        ioctl(vcpufd, KVM_RUN, NULL);
        dump_state(vcpufd);
        switch (run->exit_reason) {
        case KVM_EXIT_HLT:
            puts("KVM_EXIT_HLT");
            return 0;

            break;
        case KVM_EXIT_IO:
            if (run->io.direction == KVM_EXIT_IO_OUT && run->io.size == 1 &&
                run->io.port == 0x3f8 && run->io.count == 1)
                putchar(*(((char *) run + run->io.data_offset)));
            else
                errx(1, "unhandled KVM_EXIT_IO");
            break;
        case KVM_EXIT_FAIL_ENTRY:
            errx(1, "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason = 0x%llx",
                 (unsigned long long) run->fail_entry.hardware_entry_failure_reason);
            break;
        case KVM_EXIT_INTERNAL_ERROR:
            errx(1, "KVM_EXIT_INTERNAL_ERROR: suberror = 0x%x", run->internal.suberror);
        }
    }
}
