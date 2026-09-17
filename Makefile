vmm: vmm.c
	gcc -o2 -Wall -Wextra -std=gnu17 vmm.c -o vmm
clean:
	rm vmm
.PHONY: clean