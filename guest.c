#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>

static void write(int fd, void* ptr, size_t size) {

}

static void outb(uint16_t port, uint8_t value) {
	asm("outb %0,%1" : /* empty */ : "a" (value), "Nd" (port) : "memory");
}

static char inb(uint16_t port) {
    char ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static char getchar() {
    return inb(0xE9);
}

static char digits[] = "0123456789ABCDEF";

static void
putc(int fd, char c)
{
    if (fd == 1) {
        outb(0xE9, c);
    } else {
        write(fd, &c, 1);
    }
}

static void
printint(int fd, int xx, int base, int sgn)
{
  char buf[16];
  int i, neg;
  uint32_t x;

  neg = 0;
  if(sgn && xx < 0){
    neg = 1;
    x = -xx;
  } else {
    x = xx;
  }

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);
  if(neg)
    buf[i++] = '-';

  while(--i >= 0)
    putc(fd, buf[i]);
}

static void
printptr(int fd, uint64_t x) {
  int i;
  putc(fd, '0');
  putc(fd, 'x');
  for (i = 0; i < (sizeof(uint64_t) * 2); i++, x <<= 4)
    putc(fd, digits[x >> (sizeof(uint64_t) * 8 - 4)]);
}

// Print to the given fd. Only understands %d, %x, %p, %s.
void
vprintf(int fd, const char *fmt, va_list ap)
{
  char *s;
  int c, i, state;

  state = 0;
  for(i = 0; fmt[i]; i++){
    c = fmt[i] & 0xff;
    if(state == 0){
      if(c == '%'){
        state = '%';
      } else {
        putc(fd, c);
      }
    } else if(state == '%'){
      if(c == 'd'){
        printint(fd, va_arg(ap, int), 10, 1);
      } else if(c == 'l') {
        printint(fd, va_arg(ap, uint64_t), 10, 0);
      } else if(c == 'x') {
        printint(fd, va_arg(ap, int), 16, 0);
      } else if(c == 'p') {
        printptr(fd, va_arg(ap, uint64_t));
      } else if(c == 's'){
        s = va_arg(ap, char*);
        if(s == 0)
          s = "(null)";
        while(*s != 0){
          putc(fd, *s);
          s++;
        }
      } else if(c == 'c'){
        putc(fd, va_arg(ap, uint32_t));
      } else if(c == '%'){
        putc(fd, c);
      } else {
        // Unknown % sequence.  Print it to draw attention.
        putc(fd, '%');
        putc(fd, c);
      }
      state = 0;
    }
  }
}

void
fprintf(int fd, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fd, fmt, ap);
}

void
printf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(1, fmt, ap);
}

int scan_int() {

  char c;
  int num = 0;

  while ((c = getchar()) != '\n') {
    num *= 10;
    num += c - '0';
  }

  return num;

}

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void) {
	const char *p;
	uint16_t port = 0xE9;


    int a = 5;
    int b = 6;
  
    printf("Unesi a: ");
    a = scan_int(); 
    printf("Unesi b: ");
    b = scan_int();

    printf("%d + %d = %d\n", a, b, (a + b));



	for (;;)
		asm("hlt");
}
