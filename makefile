all: guest.img mini_hypervisor

mini_hypervisor: mini_hypervisor.c
	gcc $^ -o $@ -pthread -g

guest.img: guest.o
	ld -T guest.ld guest.o -o guest.img

guest.o: guest.c
	$(CC) -m64 -ffreestanding -fno-pic -c -o $@ $^

clean:
	rm -f kvm_zadatak3 guest.o guest.img