NUMBERS = 1 2 3 4

all: guest.img mini_hypervisor

mini_hypervisor: mini_hypervisor.c
	gcc $^ -o $@ -pthread -g

# Pattern rule for building guest.img files
guest%.img: guest%.o
	ld -T guest.ld $^ -o $@

# Pattern rule for building guest.o files
guest%.o: guest.c
	$(CC) -m64 -ffreestanding -fno-pic -c -o $@ $^ -DPROGRAM=$*

# Define the list of all guest image targets
GUEST_IMAGES = $(addsuffix .img, $(addprefix guest,$(NUMBERS)))

# Build all guest images
guest.img: $(GUEST_IMAGES)
	touch guest.img # This ensures guest.img is always updated

clean:
	rm -f mini_hypervisor $(GUEST_IMAGES) $(GUEST_OBJECTS)
