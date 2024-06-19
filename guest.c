#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>

#define PROGRAM 4

#define O_RDONLY        0        /* open for reading only */
#define O_WRONLY        1        /* open for writing only */
#define O_RDWR          2        /* open for reading and writing */
#define O_CREAT         64       /* create file if it does not exist */
#define O_EXCL          128      /* error if O_CREAT and the file exists */
#define O_NOCTTY        256      /* do not assign controlling terminal */
#define O_TRUNC         512      /* truncate size to 0 */
#define O_APPEND        1024     /* append on each write */
#define O_NONBLOCK      2048     /* non-blocking mode */
#define O_DSYNC         4096     /* synchronous writes */
#define FASYNC          8192     /* signal-driven I/O */
#define O_DIRECT        16384    /* direct disk access hints */
#define O_LARGEFILE     32768    /* allow large file opens */
#define O_DIRECTORY     65536    /* must be a directory */
#define O_NOFOLLOW      131072   /* do not follow symbolic links */
#define O_NOATIME       262144   /* do not update access times */
#define O_CLOEXEC       524288   /* set close_on_exec */
#define O_PATH          1048576  /* resolve pathname but do not open file */
#define O_TMPFILE       2097152  /* create unnamed temporary file */

#define PARALEL_PORT 0x278
#define OPEN 1
#define CLOSE 2
#define READ 3
#define WRITE 4
#define FINISH 0
#define EOF -1

static inline void exit() {
	for (;;)
		asm("hlt");
}

static void outb(uint16_t port, uint8_t value) {
	asm("outb %0,%1" : /* empty */ : "a" (value), "Nd" (port) : "memory");
}

static void out(uint16_t port, uint32_t value) {
  asm("out %0, %1" : : "a"(value), "Nd" (port) : "memory");
}

static void outq(uint16_t port, uint64_t value) {
  uint32_t val1 = value & ~0;
  uint32_t val2 = (value >> 32) & ~0;

  asm ("out %0, %1" : : "a" (val1), "Nd" (port) : "memory");
  asm ("out %0, %1" : : "a" (val2), "Nd" (port) : "memory");
}

static int in(uint16_t port) {
  int ret;
  asm("in %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static char inb(uint16_t port) {
  char ret;
  asm("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

void
printf(const char *fmt, ...);

static int open(const char* file_name, int flags, int mode) {
  out(PARALEL_PORT, OPEN);
  int i;
  for (i = 0; file_name[i]; i++) {
    outb(PARALEL_PORT, file_name[i]);
  }
  outb(PARALEL_PORT, file_name[i]);

  out(PARALEL_PORT, flags);
  out(PARALEL_PORT, mode);

  int fd = in(PARALEL_PORT);

  return fd;
}

static int close(int fd) {
  
  out(PARALEL_PORT, CLOSE);
  out(PARALEL_PORT, fd);

  int status = in(PARALEL_PORT);
  return status;
}

size_t read(int fd, void* buf, size_t count) {
  out(PARALEL_PORT, READ);
  out(PARALEL_PORT, fd); 
  outq(PARALEL_PORT, (uint64_t) buf);
  outq(PARALEL_PORT, (uint64_t) count);
  
  return in(PARALEL_PORT);
}

size_t write(int fd, void* buf, size_t count) {
  out(PARALEL_PORT, WRITE);
  out(PARALEL_PORT, fd); 
  outq(PARALEL_PORT, (uint64_t) buf);
  outq(PARALEL_PORT, (uint64_t) count);
  
  return in(PARALEL_PORT);
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

#if PROGRAM == 1
    int a = 5;
    int b = 6;
  
    printf("Unesi a: ");
    a = scan_int(); 
    printf("Unesi b: ");
    b = scan_int();

    printf("%d + %d = %d\n", a, b, (a + b));
#elif PROGRAM == 2

    int fd = open("primer.txt", O_RDONLY, 0);
    printf("%d\n", fd);
    if (fd < 0) {
      printf("Greska u otvaranju fajla");
      exit();
    }

    char buf[20];
    size_t size;

    do {
      size = read(fd, buf, 20);
      for (int i = 0; i < size; i++) {
        printf("%c", buf[i]);
      } 
    } while (size == 20);

    close(fd);

#elif PROGRAM == 3

    int fd = open("primer.txt", O_WRONLY | O_TRUNC | O_CREAT, 0777);
    printf("%d\n", fd);
    if (fd < 0) {
      printf("Greska u otvaranju fajla");
      exit();
    }
    
    char tekst[] = "Neki tekst blablabla";

    write(fd, tekst, sizeof(tekst) - 1);

    close(fd);

    fd = open("primer.txt", O_RDONLY, 0);
    printf("%d\n", fd);
    if (fd < 0) {
      printf("Greska u otvaranju fajla");
      exit();
    }

    char buf[20];
    size_t size;

    do {
      size = read(fd, buf, 20);
      for (int i = 0; i < size; i++) {
        printf("%c", buf[i]);
      } 
    } while (size == 20);

    close(fd);
#elif PROGRAM == 4

  int fd1 = open("primer1.txt", O_RDONLY, 0);
  if (fd1 < 0) {
    printf("Greska pri otvaranju fajla primer1.txt\n");
    exit();
  }

  int fd2 = open("primer2.txt", O_WRONLY | O_CREAT | O_TRUNC, 0);
  if (fd2 < 0) {
    printf("Greska pri otvaranju fajla %s\n", "primer.txt");
  }

  char buf[20];
  int size = 0;

  do {
    size = read(fd1, buf, 20);
    write(fd2, buf, size);
  } while (size == 20);

  close(fd1);
  close(fd2);

#endif
  for (;;) {
    asm volatile("hlt");
  }
}
