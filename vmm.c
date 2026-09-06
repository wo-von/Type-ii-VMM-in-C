#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include <linux/kvm.h>

const uint8_t code[] = {
    0xba, 0xf8, 0x03, /* mov $0x3f8, %dx */
	0x00, 0xd8,       /* add %bl, %al */
	0x04, '0',        /* add $'0', %al */
	0xee,             /* out %al, (%dx) */
	0xb0, '\n',       /* mov $'\n', %al */
	0xee,             /* out %al, (%dx) */
	0xf4,             /* hlt */
};

int kvm; 
int main() {
    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm == -1){
        err(1, "cannot open /dev/kvm");        
    }

    int ret = ioctl(kvm, KVM_GET_API_VERSION, NULL);
    if (ret == -1) {
        err(1, "KVM_GET_API_VERSION ");
    }
    if (ret != 12){
        err(1, "KVM_GET_API_VERSION expexted 12 got %d\n", ret);
    }
    
}
