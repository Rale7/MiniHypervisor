#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <getopt.h>
#include <pty.h>
#include <semaphore.h>

#define OPEN 1
#define CLOSE 2
#define READ 3
#define WRITE 4
#define FINISH 0

#define PDE64_PRESENT 1
#define PDE64_RW (1U << 1)
#define PDE64_USER (1U << 2)
#define PDE64_PS (1U << 7)

#define PM5O_MASK ((uint64_t)(((1UL << 9UL) - 1UL) << 48UL))
#define PM4O_MASK ((uint64_t)(((1UL << 9UL) - 1UL) << 39UL))
#define PDPO_MASK ((uint64_t)(((1UL << 9UL) - 1UL) << 30UL))
#define PDO_MASK ((uint64_t)(((1UL << 9UL) - 1UL) << 21UL))
#define PTO_MASK ((uint64_t)(((1UL << 9UL) - 1UL) << 12UL))

#define PAGE4KB_OFFSET(addr) (addr & ((1 << 12) - 1))
#define PAGE2MB_OFFSET(addr) (addr & ((1 << 21) - 1))

#define PM5_ADDR_TO_ENTRY(addr) ((addr & PM5O_MASK) >> 48UL)
#define PM4_ADDR_TO_ENTRY(addr) ((addr & PM4O_MASK) >> 39UL)
#define PDPO_ADDR_TO_ENTRY(addr) ((addr & PDPO_MASK) >> 30UL)
#define PDO_ADDR_TO_ENTRY(addr) ((addr & PDO_MASK) >> 21UL)
#define PTO_ADDR_TO_ENTRY(addr) ((addr & PTO_MASK) >> 12UL)

#define PMT_ENTRY_TO_ADDR(entry) ((((entry >> 12)) & ((1L << 40UL) - 1)) << 12UL)

// CR4
#define CR4_PAE (1U << 5)

// CR0
#define CR0_PE 1u
#define CR0_PG (1U << 31)

#define EFER_LME (1U << 8)
#define EFER_LMA (1U << 10)

#define SIZE2MB (2 * 1024 * 1024)

const char** shared_files;
int shared_file_size = 0;

enum PageSize {MB2, KB4};

//  Struktura koja predstavlja hipervizora
//
//  kvm_fd - fajl deskriptor /dev/kvm
//  kvm_run_mmap_size - velicina run strukture gosta
struct hypervisor {
    int kvm_fd; 
    int kvm_run_mmap_size;
};

//  Inicijalizuje hypervisora sa potrebnim parametrima,
//  koristi sistemski poziv open za otvaranje fajla
//  /dev/kvm i ioctl za dohvatanje velicine run strukture
int init_hypervisor(struct hypervisor* hypervisor) {

    hypervisor->kvm_fd = open("/dev/kvm", O_RDWR);
    if (hypervisor->kvm_fd < 0) {
        perror("GRESKA: Nije moguce otvoriti /dev/kvm fajl\n");
        return -1;
    }

    hypervisor->kvm_run_mmap_size = ioctl(hypervisor->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (hypervisor->kvm_run_mmap_size < 0) {
        perror("GRESKA: Neuspesan ioctl KVM_GET_VCPU_MMAP_SIZE\n");
        fprintf(stderr, "KVM_GET_VPCU_MMAP_SIZE: %s\n", strerror(errno));
        return -1;
    }

    return 0;

}

struct guest;

typedef int (*State) (struct guest*, uint32_t data, void* data_offset);

struct file {
    int fd;
    int flags;
    mode_t mode;
    int cnt;
    uint64_t addr;
    uint64_t size;
    struct file* next;
    char ime[50];
};

//  Struktura koja definise jednog gosta
//
//  vm_fd - fajl deskriptor koji komunicira sa odredjenim vm-om
//  vm_vcp - fajl deskriptor koji predstavlja virtuelni procesor
//  mem - memorija gosta
//  kvm_run - run struktura gosta  
struct guest {
    int vm_fd;
    int vm_vcpu;
    int pty_master;
    int pty_slave;
    int lock;
    int id;
    char* mem;
    struct kvm_run* kvm_run;
    struct file* file_head;
    struct file* current_file;
    State current_file_state;
};

//  Kreira novog gosta i vraca 0 pri uspehu,
//  u slucaju neuspeha ispisuje gresku
//  i vraca -1
int create_guest(struct hypervisor* hypervisor, struct guest* vm) {

    vm->vm_fd = ioctl(hypervisor->kvm_fd, KVM_CREATE_VM, 0);
    if (vm->vm_fd < 0) {
        perror("GRESKA: Neuspesno otvaranje kvm fajla\n");
        fprintf(stderr, "KVM_CREATE_VM: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

//  Alocira prostor za fizicku memoriju gosta
//  i dodaje je u vm strukturu
int create_memory_region(struct guest* vm, size_t mem_size) {
    struct kvm_userspace_memory_region region;

    vm->mem = (char*)mmap(NULL, mem_size, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm->mem == MAP_FAILED) {
        perror("GRESKA: Neuspesan mmap za mapiranje memorije\n");
        return -1;
    }

    region.slot = 0;
    region.flags = 0;
    region.guest_phys_addr = 0;
    region.memory_size = mem_size;
    region.userspace_addr = (unsigned long)vm->mem;

    if (ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("GRESKA: Neuspesan ioctl KVM_SET_USER_MEMORY_REGION\n");
        fprintf(stderr, "KVM_SET_USER_MEMORY_REGION: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

//  Kreira virtuelni procesor i vraca 0
//  u slucaju uspeha u slucaju neuspeha
// vraca -1
int create_vcpu(struct guest* vm) {

    vm->vm_vcpu = ioctl(vm->vm_fd, KVM_CREATE_VCPU, 0);
    if (vm->vm_vcpu < 0) {
        perror("GRESKA: Nesupesan ioctl KVM_CREATE_VCPU\n");
        fprintf(stderr, "KVM_CREATE_VCPU: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

//  Alocira prostor za kvm run strukturu
int create_kvm_run(struct hypervisor* hypervisor, struct guest* vm) {

    vm->kvm_run = mmap(NULL,hypervisor->kvm_run_mmap_size, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_SHARED, vm->vm_vcpu, 0);
    if (vm->kvm_run == MAP_FAILED) {
        perror("GRESKA: Neuspesan mmap za mapiranje kvm_run strukture\n");
        return -1;
    }

    return 0;

}

void setup_64bit_code_segment(struct kvm_sregs* sregs) {

    struct kvm_segment segment = {
        .base = 0,
        .limit = 0xffffffff,
        .present = 1,
        .type = 11,
        .dpl = 0,
        .db = 0,
        .s = 1,
        .l = 1,
        .g = 1
    };

    sregs->cs = segment;
    segment.type = 3;
    sregs->ds = sregs->es = sregs->fs = sregs->gs = sregs->ss = segment;

}

int setup_long_mode(struct guest* vm, size_t mem_size, enum PageSize page_size) {

    struct kvm_sregs sregs;

    if (ioctl(vm->vm_vcpu, KVM_GET_SREGS, &sregs) < 0) {
        perror("GRESKA: Neuspesan ioctl KVM_GET_SREGS\n");
        fprintf(stderr, "KVM_GET_SREGS: %s\n", strerror(errno));
        return -1;
    }

	uint64_t pml4_addr = 0;
	uint64_t *pml4 = (void *)(vm->mem + pml4_addr);

	uint64_t pdpt_addr = 0x1000;
	uint64_t *pdpt = (void *)(vm->mem + pdpt_addr);

	uint64_t pd_addr = 0x2000;
	uint64_t *pd = (void *)(vm->mem + pd_addr);

    uint64_t page = 0x3000;

	pml4[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pdpt_addr;
	pdpt[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pd_addr;

    if (page_size == MB2) {
        page = (page / SIZE2MB + 1) * SIZE2MB;
        uint64_t page_address = page;
        for (int i = 0; i < mem_size / SIZE2MB - 1; i++) {
            pd[i] = PDE64_PRESENT | PDE64_RW | PDE64_USER | PDE64_PS | page_address;
            page_address += SIZE2MB;
        }
    } else {
        for (int i = 0; i < mem_size / SIZE2MB; i++) {
            pd[i] = PDE64_PRESENT | PDE64_RW | PDE64_USER | page;
            page += 0x1000;
        }

        uint64_t page_address = page;
        for (int i = 0; i < mem_size / SIZE2MB; i++) {
            uint64_t pt_addr = pd[i] & ~0xFFFUL;

            uint64_t* pt = (void*) vm->mem + pt_addr;

            for (int j = 0; j < 512; j++) {
                if (page_address > mem_size) break;
                pt[j] = page_address | PDE64_PRESENT | PDE64_RW | PDE64_USER;
                page_address += 0x1000;
            }
        }
    }

    sregs.cr3 = pml4_addr;
    sregs.cr4 = CR4_PAE;
    sregs.cr0 = CR0_PE | CR0_PG;
    sregs.efer = EFER_LMA | EFER_LME;

    setup_64bit_code_segment(&sregs);

    if (ioctl(vm->vm_vcpu, KVM_SET_SREGS, &sregs) < 0) {
        perror("GRESKA: Neuspesan ioctl KVM_SET_SREGS\n");
        fprintf(stderr, "KVM_SET_SREGS: %s\n", strerror(errno));
        return -1;
    }

    return page;
}

int setup_registers(struct guest* vm) {
    struct kvm_regs regs;

    if (ioctl(vm->vm_vcpu, KVM_GET_REGS, &regs) < 0) {
        perror("GRESKA: Neuspesan ioctl KVM_GET_REGS\n");
        fprintf(stderr, "KVM_GET_REGS %s\n", strerror(errno));
        return -1;
    }

    memset(&regs, 0, sizeof(regs));

    regs.rflags = 2;
    regs.rip = 0;
    regs.rsp = 1 << 19;

    if (ioctl(vm->vm_vcpu, KVM_SET_REGS, &regs) < 0) {
        perror("GRESKA: Nesupesan ioctl KVM_SET_REGS\n");
        fprintf(stderr, "KVM_SET_REGS %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

int setup_terminal(struct guest* vm) {
    if (openpty(&vm->pty_master, &vm->pty_slave, NULL, NULL, NULL) != -1) {
        perror("GRESKA: Neuspesno otvaranje pseudoterminala\n");
        fprintf(stderr, "open_pty %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

int exit_halt(struct guest* vm) {
    printf("KVM_EXIT_HLT\n");
    return 1;
}

sem_t file_mutex;

int start_file_operation(struct guest*, uint32_t, void*);

struct file* init_file() {

    struct file* new_file = (struct file*) malloc(sizeof(struct file));
    if (new_file == NULL) {
        perror("GRESKA: U alokaciji novog fajla\n");
        exit(EXIT_FAILURE);
    }

    new_file->cnt = 0;
    new_file->next = NULL;
    new_file->flags = -1;
    new_file->mode = -1;
    new_file->fd = -1;
    new_file->addr = 0;
    new_file->size = 0;

    return new_file; 
}

void* virtual_to_physical_add(struct guest* vm, uint64_t addr) {

    uint64_t* pm4 = (uint64_t*) vm->mem;
    uint64_t entry2 = PM4_ADDR_TO_ENTRY(addr);

    if (!(pm4[entry2] & PDE64_PRESENT)) {
        return NULL;
    }

    uint64_t* pdp = (uint64_t*) (vm->mem + PMT_ENTRY_TO_ADDR(pm4[entry2]));
    uint64_t entry3 = PDPO_ADDR_TO_ENTRY(addr);

    if (!(pdp[entry3] & PDE64_PRESENT)) {
        return NULL;
    }

    uint64_t* pd = (uint64_t*) (vm->mem + PMT_ENTRY_TO_ADDR(pdp[entry3]));
    uint64_t entry4 = PDO_ADDR_TO_ENTRY(addr);

    if (!(pd[entry4] & PDE64_PRESENT)) {
        return NULL;
    }

    if (pd[entry4] & PDE64_PS) {
        return vm->mem + (PMT_ENTRY_TO_ADDR(pd[entry4])) + PAGE2MB_OFFSET(addr);
    }

    uint64_t* pt = (uint64_t*) (vm->mem + PMT_ENTRY_TO_ADDR(pd[entry4]));
    uint64_t entry5 = PTO_ADDR_TO_ENTRY(addr);

    if (!(pt[entry5] & PDE64_PRESENT)) {
        return NULL;
    }

    return vm->mem + PMT_ENTRY_TO_ADDR(pt[entry5]) + PAGE4KB_OFFSET(addr); 

}

int get_file_descriptor(struct guest* vm, int data) {

    for (struct file* current = vm->file_head; current; current = current->next) {

        if (current->fd == data) {
            vm->current_file = current;
            break;
        }
    }

    return 0;
}

int end_file_operation(struct guest* vm) {
    vm->current_file_state = &start_file_operation;
    vm->current_file = NULL;
    return 0;
}

int return_fd_to_vm(struct guest* vm, uint32_t data, void* data_offset) {
    if (vm->kvm_run->io.direction != KVM_EXIT_IO_IN || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    *((int*) data_offset) = vm->current_file->fd;
    return end_file_operation(vm);
}

int is_shared_file(const char* file_name) {

    for (int i = 0; i < shared_file_size; i++) {
        if (strcmp(shared_files[i], file_name) == 0) {
            return 1;
        }
    }

    return 0;
}

void create_local_copy(struct guest* vm) {

    char path[200];
    sprintf(path, "vm%d_", vm->id);
    strcat(path, vm->current_file->ime);
    int fd = open(path, O_CREAT | O_WRONLY, 0777);

    if (is_shared_file(vm->current_file->ime)) {
        int shared_fd = open(vm->current_file->ime, O_RDONLY);
        if (shared_fd < 0) {
            fprintf(stderr, "GRESKA: Nepostojeci deljeni fajl %s\n", vm->current_file->ime);
        }

        char buffer[1024];
        int size = 0;

        do {
            size = read(shared_fd, buffer, 1024);
            write(fd, buffer, size);
        } while (size == 1024);
        close(shared_fd);
    }

    close(fd);
}

int return_local_file(struct guest* vm) {

    char path[200];
    sprintf(path, "vm%d_", vm->id);    
    strcat(path, vm->current_file->ime);
    if (access(path, F_OK) != 0 && vm->current_file->flags & O_CREAT) {
        create_local_copy(vm);
    }
    return open(path, vm->current_file->flags, vm->current_file->mode);
}

int check_path_exists(struct guest* vm) {
    char path[200];
    sprintf(path, "vm%d_", vm->id);    
    strcat(path, vm->current_file->ime);
    return access(path, F_OK) == 0;
}

int wait_for_mode(struct guest* vm, uint32_t data, void* data_offset) {
    if (vm->kvm_run->io.direction != KVM_EXIT_IO_OUT || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }
   
    vm->current_file->mode = data;

    if (check_path_exists(vm)) {
        vm->current_file->fd = return_local_file(vm);
    } else if (is_shared_file(vm->current_file->ime) && !(vm->current_file->mode & (O_WRONLY | O_RDWR | O_APPEND | O_CREAT))) {
        vm->current_file->fd = open(vm->current_file->ime, vm->current_file->flags, vm->current_file->mode);
    } else {
        vm->current_file->fd = return_local_file(vm);
    }

    vm->current_file_state = &return_fd_to_vm;
    return 0;
}

int wait_for_flag(struct guest* vm, uint32_t data, void* data_offset) {
    if (vm->kvm_run->io.direction != KVM_EXIT_IO_OUT || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    vm->current_file->flags = data;
    vm->current_file_state = &wait_for_mode;

    return 0;
}

int reading_name(struct guest* vm, uint32_t data, void* data_offset) {

    if (vm->kvm_run->io.direction != KVM_EXIT_IO_OUT || vm->kvm_run->io.size != sizeof(uint8_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    char c = (char) (data & 0xFF);

    vm->current_file->ime[vm->current_file->cnt++] = c;

    if (c != '\0') {
        vm->current_file_state = &reading_name;
    } else {
        vm->current_file_state = &wait_for_flag;
    }

    return 0;
}

int wait_for_read_status(struct guest* vm, uint32_t data, void* data_offset) {
    if (vm->kvm_run->io.direction != KVM_EXIT_IO_IN || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    void* addr = virtual_to_physical_add(vm, vm->current_file->addr);
    int status = read(vm->current_file->fd, addr, vm->current_file->size);
    *((int*) data_offset) = status; 
    return end_file_operation(vm);

}

int wait_for_write_status(struct guest* vm, u_int32_t data, void* data_offset) {
    if (vm->kvm_run->io.direction != KVM_EXIT_IO_IN || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    void* addr = virtual_to_physical_add(vm, vm->current_file->addr);
    int status = write(vm->current_file->fd, addr, vm->current_file->size);
    *((int*) data_offset) = status;
    return end_file_operation(vm);
}

int wait_for_second_size_half(struct guest* vm, uint32_t data, void* data_offset) {
    if (vm->kvm_run->io.direction != KVM_EXIT_IO_OUT || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    vm->current_file->size |= ((uint64_t) data << 32);
    if (vm->lock == READ) {
        vm->current_file_state = &wait_for_read_status;
    } else {
        vm->current_file_state = &wait_for_write_status;
    }
    return 0;
}

int wait_for_first_size_half(struct guest* vm, uint32_t data, void* data_offset) {
    if (vm->kvm_run->io.direction != KVM_EXIT_IO_OUT || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    vm->current_file->size = 0;
    vm->current_file->size = data;
    vm->current_file_state = &wait_for_second_size_half;
    return 0;
}

int wait_for_second_addr_half(struct guest* vm, uint32_t data, void* data_offset) {
    if (vm->kvm_run->io.direction != KVM_EXIT_IO_OUT || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    vm->current_file->addr |= ((uint64_t)data << 32);
    vm->current_file_state = &wait_for_first_size_half;
    return 0;
}

int wait_for_first_addr_half(struct guest* vm, uint32_t data, void* data_offset) {
    if (vm->kvm_run->io.direction != KVM_EXIT_IO_OUT || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    vm->current_file->addr = 0;
    vm->current_file->addr = data;
    vm->current_file_state = &wait_for_second_addr_half;
    return 0;
}

int wait_for_close_status(struct guest* vm, uint32_t data, void* data_offset) {

    if (vm->kvm_run->io.direction != KVM_EXIT_IO_IN|| vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    int status = close(vm->current_file->fd);
    *((int*) data_offset) = status;

    for (struct file** indirect = &vm->file_head; *indirect; indirect = &(*indirect)->next) {
        if (*indirect == vm->current_file) {
            *indirect = vm->current_file->next;
            break;
        }
    }

    free(vm->current_file);

    return end_file_operation(vm);
}

int wait_for_fd(struct guest* vm, uint32_t data, void* data_offset) {

    if (vm->kvm_run->io.direction != KVM_EXIT_IO_OUT || vm->kvm_run->io.size != sizeof(uint32_t)) {
        perror("GRESKA: Vm nije ispostovan protokol\n");
        return -1;
    }

    get_file_descriptor(vm, data);

    if (vm->current_file == NULL) {
        perror("GRESKA: Vm: nepostoji file deskriptor\n");
        return -1;
    }

    if (vm->lock == READ || vm->lock == WRITE) {
        vm->current_file_state = &wait_for_first_addr_half;
    } else if (vm->lock == CLOSE) {
        vm->current_file_state = &wait_for_close_status;
    }

    return 0;
}

int start_file_operation(struct guest* vm, uint32_t operation, void* data_offset) {
    vm->lock = operation;

    if (operation == OPEN) {
        struct file* new_file = init_file(); 
        new_file->next = vm->file_head;
        vm->file_head = new_file; 
        vm->current_file = new_file;
        vm->current_file_state = &reading_name;
    } else {
        vm->current_file_state = &wait_for_fd; 
    }

    return 0;
}

int handle_file(struct guest* vm) {

    void* data_offset = (char*)vm->kvm_run + vm->kvm_run->io.data_offset;
    int data = *((int*) data_offset);

    return vm->current_file_state(vm, data, data_offset);
}

int exit_io(struct guest* vm) {
    if (vm->kvm_run->io.direction == KVM_EXIT_IO_OUT && vm->kvm_run->io.port == 0xE9) {
        char c = *((char*)vm->kvm_run + vm->kvm_run->io.data_offset);
        write(vm->pty_master, &c, vm->kvm_run->io.size);
        return 0;
    } else if (vm->kvm_run->io.direction == KVM_EXIT_IO_IN && vm->kvm_run->io.port == 0xE9) {
        char c;
        read(vm->pty_master, &c, sizeof(char));
        *((char*)vm->kvm_run + vm->kvm_run->io.data_offset) = c;
        return 0;
    } else if (vm->kvm_run->io.port == 0x278) {
        handle_file(vm);
    } else {
        fprintf(stderr, "Invalid port %d\n", vm->kvm_run->io.port);
        return -1;
    }
}

int exit_internal_error(struct guest* vm) {
    printf("GRESKA: Interna greska: podgreska = 0x%x\n", vm->kvm_run->internal.suberror);
    return -1;
}

int exit_shutdown(struct guest* vm) {
    printf("Shutdown\n");
    return 1;
}

typedef int (*Handler)(struct guest* vm);

static Handler handlers[] = {
    NULL, NULL, &exit_io, NULL, NULL, &exit_halt,
    NULL, NULL, &exit_shutdown, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL,
    &exit_internal_error
};

void* run_guest(void* par) {

    struct guest* vm = (struct guest*) par;
    int stop = 0;
    int ret;

    while (stop == 0) {

        ret = ioctl(vm->vm_vcpu, KVM_RUN, 0);
        if (ret < 0) {
            perror("GRESKA: Neuspesan ioctl KVM_RUN\n");
            fprintf(stderr, "KVM_RUN: %s\n", strerror(errno));
            return NULL;
        }

        int exit_reason = vm->kvm_run->exit_reason;

        if (handlers[exit_reason]) {
            stop = handlers[exit_reason](vm);
        } else {
            printf("Unknown exit reason %d\n", exit_reason);
            stop = -1;
        }
    }

    return NULL;
} 

pthread_t start_guest(struct guest* vm, FILE* img, int starting_adress) {

    pthread_t handle;

    char* p = vm->mem + starting_adress;
    while (feof(img) == 0) {
        int r = fread(p, 1, 1024, img);
        p += r;
    }

    if (pthread_create(&handle, NULL, &run_guest, vm) == 0) {
        return handle;
    } else {
        return -1;
    }
}

int init_guest(struct hypervisor* hypervisor, struct guest* vm, size_t mem_size, enum PageSize page_size, FILE* img) {

    static int incId = 0;

    int starting_address;

    if (create_guest(hypervisor, vm) < 0) return -1;
    if (create_memory_region(vm, mem_size) < 0) return -1;
    if (create_vcpu(vm) < 0) return -1;
    if (create_kvm_run(hypervisor, vm) < 0) return - 1; 
    if ((starting_address = setup_long_mode(vm, mem_size, page_size)) < 0) return -1;
    if (setup_registers(vm) < 0) return -1;
    vm->lock = 0;
    vm->file_head = NULL;
    vm->current_file = NULL;
    vm->id = incId++;
    vm->current_file_state = &start_file_operation;

    return starting_address;

}

void add_to_files(const char** files, int* size, const char* file) {

    if (*size % 10 == 0) {
        files = realloc(files, sizeof(const char*) * (*size + 10));
        if (files == NULL) {
            printf("GRESKA: alokacija nije uspela\n");
            exit(EXIT_FAILURE);
        }
    }

    files[(*size)++] = file;
}

int main(int argc, char* argv[]) {

    int opt;
    int memory = 0;
    enum PageSize page_size;
    struct hypervisor hypervisor;
    int starting_adress;
    const char** imgs = malloc(sizeof(const char*) * 10) ;
    int img_size = 0;
    shared_files = malloc(sizeof(const char* ) * 10);
    

    struct option long_options[] = {
        {"memory", required_argument, 0, 'm'},
        {"page", required_argument, 0, 'p'},
        {"guest", no_argument, 0, 'g'},
        {"file", no_argument, 0, 'f'},
        {0, 0, 0, 0,}
    };

    while ((opt = getopt_long(argc, argv, "m:p:gf", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm':
                memory = atoi(optarg) * 1024 * 1024;
                break;
            case 'p':
                page_size = (atoi(optarg) == 4) ? KB4 : MB2;
                break;
            case 'g':
                while (optind < argc && argv[optind][0] != '-') {
                    add_to_files(imgs, &img_size, argv[optind++]);
                }
                break;
            case 'f':
                while (optind < argc && argv[optind][0] != '-') {
                    add_to_files(shared_files, &shared_file_size, argv[optind++]);
                }
                break;
        }
    }

    if (init_hypervisor(&hypervisor) < 0) {
        printf("GRESKA: Nije moguce inicijalizovati hipervizora\n");
        exit(EXIT_FAILURE);
    }

    if (page_size == MB2 && memory == SIZE2MB) {
        perror("GRESKA: Ne moze se kreirati vm sa stranicom od 2mb i memorijom od 2mb");
        exit(EXIT_FAILURE);
    }

    int num_of_vms = img_size;
    pthread_t* vms = (pthread_t*) malloc(sizeof(pthread_t) * (num_of_vms));

    if (sem_init(&file_mutex, 0, 1) < 0) {
        perror("GRESKA: Neuspesan sem_init\n");
        fprintf(stderr, "sem_init %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < img_size; i++) {
        FILE* img = fopen(imgs[i], "r");
        if (img == NULL) {
            printf("GRESKA: Nije omoguce otvoriti fajl %s\n", imgs[i]);
            exit(EXIT_FAILURE);
        }

        struct guest* vm = malloc(sizeof(struct guest));
        if (vm == NULL) {
            printf("GRESKA: Alokacija nije uspela\n");
            printf("fopen: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        if ((starting_adress = init_guest(&hypervisor, vm, memory, page_size, img)) < 0) {
            printf("GRESKA: Nije moguce inicijalizovati gosta\n");
            exit(EXIT_FAILURE);
        }

        pthread_t handle = start_guest(vm, img, starting_adress);
        vms[i] = handle;
    }

    for (int i = 0; i < num_of_vms; i++) {
        pthread_join(vms[i], NULL);
    }

}