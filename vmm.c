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

#define PDE64_PRESENT (1U << 0)
#define PDE64_RW (1U << 1)
#define PDE64_USER (1U << 2)
#define PDE64_PS (1U << 7)

/* Walks the guest's page tables from the host side. `mem` is the host
 * pointer backing guest-physical memory; `cr3` is the guest-physical
 * address of the PML4, read out of the vcpu's sregs. Assumes an identity
 * mapping between guest-physical addresses and offsets into `mem`, which
 * is what this VMM's memory layout uses. */
static void dump_page_tables(void *mem, uint64_t cr3) {
    uint64_t *pml4 = (uint64_t *) ((uint8_t *) mem + cr3);
    for (int i = 0; i < 512; i++) {
        if (!(pml4[i] & PDE64_PRESENT))
            continue;
        printf("PML4[%d] = %llX (RW=%d US=%d) -> PDPT @ %llX\n", i, (unsigned long long) pml4[i],
               !!(pml4[i] & PDE64_RW), !!(pml4[i] & PDE64_USER),
               (unsigned long long) (pml4[i] & ~0xFFFULL));

        uint64_t *pdpt = (uint64_t *) ((uint8_t *) mem + (pml4[i] & ~0xFFFULL));
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PDE64_PRESENT))
                continue;
            if (pdpt[j] & PDE64_PS) {
                printf("  PDPT[%d] = %llX (RW=%d US=%d PS=1) -> 1GiB page @ %llX\n", j,
                       (unsigned long long) pdpt[j], !!(pdpt[j] & PDE64_RW),
                       !!(pdpt[j] & PDE64_USER), (unsigned long long) (pdpt[j] & ~0xFFFULL));
                continue;
            }
            printf("  PDPT[%d] = %llX (RW=%d US=%d) -> PD @ %llX\n", j,
                   (unsigned long long) pdpt[j], !!(pdpt[j] & PDE64_RW), !!(pdpt[j] & PDE64_USER),
                   (unsigned long long) (pdpt[j] & ~0xFFFULL));

            uint64_t *pd = (uint64_t *) ((uint8_t *) mem + (pdpt[j] & ~0xFFFULL));
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PDE64_PRESENT))
                    continue;
                if (pd[k] & PDE64_PS) {
                    printf("    PD[%d] = %llX (RW=%d US=%d PS=1) -> 2MiB page @ %llX\n", k,
                           (unsigned long long) pd[k], !!(pd[k] & PDE64_RW), !!(pd[k] & PDE64_USER),
                           (unsigned long long) (pd[k] & ~0xFFFULL));
                    continue;
                }
                printf("    PD[%d] = %llX (RW=%d US=%d) -> PT @ %llX\n", k,
                       (unsigned long long) pd[k], !!(pd[k] & PDE64_RW), !!(pd[k] & PDE64_USER),
                       (unsigned long long) (pd[k] & ~0xFFFULL));

                uint64_t *pt = (uint64_t *) ((uint8_t *) mem + (pd[k] & ~0xFFFULL));
                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PDE64_PRESENT))
                        continue;
                    printf("      PT[%d] = %llX (RW=%d US=%d) -> 4KiB page @ %llX\n", l,
                           (unsigned long long) pt[l], !!(pt[l] & PDE64_RW), !!(pt[l] & PDE64_USER),
                           (unsigned long long) (pt[l] & ~0xFFFULL));
                }
            }
        }
    }
}

static void dump_state(int vcpufd, void *mem) {
    struct kvm_regs reg;
    struct kvm_sregs sreg;
    int ret = ioctl(vcpufd, KVM_GET_REGS, &reg);
    if (ret == -1) {
        err(1, "KVM_GET_REGS failed");
    }
    ret = ioctl(vcpufd, KVM_GET_SREGS, &sreg);
    if (ret == -1) {
        err(1, "KVM_GET_SREGS failed");
    }
    printf("rip = %llX\n", reg.rip);
    printf("rsp = %llX\n", reg.rsp);
    printf("rflags = %llX\n", reg.rflags);
    printf("rax = %llX\n", reg.rax);
    printf("cr0 = %llX, cr3 = %llX, cr4 = %llX\n", sreg.cr0, sreg.cr3, sreg.cr4);
    printf("efer = %llX\n", sreg.efer);
    printf("cs base=%llX limit=%X type=%X l=%X db=%X\n", sreg.cs.base, sreg.cs.limit, sreg.cs.type,
           sreg.cs.l, sreg.cs.db);
    printf("ss base=%llX limit=%X type=%X l=%X db=%X\n", sreg.ss.base, sreg.ss.limit, sreg.ss.type,
           sreg.ss.l, sreg.ss.db);
    dump_page_tables(mem, sreg.cr3);
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
    if (vmfd == -1) {
        err(1, "KVM_CREATE_VM failed");
    }

    void *mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (mem == MAP_FAILED) {
        err(1, "mmap failed");
    }
    memcpy(mem + 0x3000, code, sizeof(code));

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0x0000,
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
    if (mmap_size <= 0) {
        err(1, "KVM_GET_VCPU_MMAP_SIZE failed");
    }
    struct kvm_run *run =
        (struct kvm_run *) mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);

    if (run == MAP_FAILED) {
        err(1, "mmap for VCPU failed");
    }
    // read the sregs and set cs to 0
    struct kvm_sregs sregs;
    ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    if (ret == -1) {
        err(1, "KVM_GET_SREGS failed");
    }
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    if (ret == -1) {
        err(1, "KVM_SET_SREGS failed");
    }
    struct kvm_regs regs = {
        .rip = 0x3000,
        .rax = 2,
        .rbx = 2,
        .rflags = 0x2,
    };
    ret = ioctl(vcpufd, KVM_SET_REGS, &regs);
    if (ret == -1) {
        err(1, "KVM_SET_REGS failed");
    }
    while (1) {
        ret = ioctl(vcpufd, KVM_RUN, NULL);
        if (ret == -1) {
            dump_state(vcpufd, mem);
            err(1, "KVM_RUN_FAILED");
        }
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
            dump_state(vcpufd, mem);
            errx(1, "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason = 0x%llx",
                 (unsigned long long) run->fail_entry.hardware_entry_failure_reason);
            break;
        case KVM_EXIT_INTERNAL_ERROR:
            dump_state(vcpufd, mem);
            errx(1, "KVM_EXIT_INTERNAL_ERROR: suberror = 0x%x", run->internal.suberror);
            break;
        default:
            dump_state(vcpufd, mem);
            errx(1, "check KVM exit");
        }
    }
}
