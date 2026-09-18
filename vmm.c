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
#define PML4_ADDR 0x0000
#define PDPT_ADDR 0x1000
#define PD_ADDR 0x2000
#define CODE_ADDR 0x3000

const uint8_t code[] = {
    0xba, 0xf8, 0x03, /* mov $0x3f8, %dx */
    0x00, 0xd8,       /* add %bl, %al */
    0x04, '0',        /* add $'0', %al */
    0xee,             /* out %al, (%dx) */
    0xb0, '\n',       /* mov $'\n', %al */
    0xee,             /* out %al, (%dx) */
    0xf4,             /* hlt */
};

struct PageTable {
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *pd;
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

static void build_page_tables(struct PageTable *pt, void *mem) {
    pt->pml4 = (uint64_t *) ((uint8_t *) mem + PML4_ADDR);
    pt->pml4[0] = PDE64_PRESENT | PDE64_RW | PDPT_ADDR;
    pt->pdpt = (uint64_t *) ((uint8_t *) mem + PDPT_ADDR);
    pt->pdpt[0] = PDE64_PRESENT | PDE64_RW | PD_ADDR;
    pt->pd = (uint64_t *) ((uint8_t *) mem + PD_ADDR);
    pt->pd[0] = PDE64_PRESENT | PDE64_RW | PDE64_PS | 0x000000; // 2MiB
    pt->pd[1] = PDE64_PRESENT | PDE64_RW | PDE64_PS | 0x200000;
}

static void set_regs(int vcpufd) {

    struct kvm_sregs sregs;
    struct kvm_regs regs;
    int ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    if (ret == -1) {
        err(1, "KVM_GET_SREGS failed");
    }
    struct kvm_segment seg = {
        .base = 0,
        .limit = 0xffffffff,
        .selector = 1 << 3,
        .present = 1,
        .type = 11, // execute, read, accessed
        .dpl = 0,
        .db = 0, // must be zero when l = 1
        .s = 1,  // code or data
        .l = 1,  // 64 bit
        .g = 1   // limit is in 4 KiB units
    };

    sregs.cs = seg;
    sregs.cr0 = 1ULL << 31 | 1ULL | 1ULL << 1; // PE | MP | PG
    sregs.cr3 = PML4_ADDR;
    sregs.cr4 = 1ULL << 5; // PAE (mandatory for long mode)
    // long mode enable and active, should be set by cpu, not here, since we are bootstraping
    sregs.efer = 1ULL << 8 | 1ULL << 10;
    seg.type = 3; // data: read, write, accessed
    seg.selector = 2 << 3;
    sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = seg;
    ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    if (ret == -1) {
        err(1, "KVM_SET_SREGS failed");
    }
    // now regs
    ret = ioctl(vcpufd, KVM_GET_REGS, &regs);
    if (ret == -1) {
        err(1, "KVM_GET_REGS failed");
    }
    regs.rip = CODE_ADDR;
    regs.rsp = 0x400000;
    regs.rflags = 0x2;
    ret = ioctl(vcpufd, KVM_SET_REGS, &regs);
    if (ret == -1) {
        err(1, "KVM_SET_REGS failed");
    }
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
    memcpy(mem + CODE_ADDR, code, sizeof(code));

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = PML4_ADDR,
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
    // build page tables
    struct PageTable pt;
    build_page_tables(&pt, mem);

    // read the sregs and set cs to 0
    set_regs(vcpufd);
    dump_state(vcpufd, mem);

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
