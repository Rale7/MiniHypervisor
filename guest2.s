# as guest.s -o guest.o
# ld --oformat binary -N -e _start -o guest guest.o

.globl _start
_start:
	mov $0xE9, %dx
    xorw %ax, %ax
loop:
	out %al, (%dx)
	inc %ax
	mov $9, %bx
	cmp %ax, %bx
	jne loop
	hlt
