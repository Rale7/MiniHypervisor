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

#define PDE64_PRESENT 1
#define PDE64_RW (1U << 1)
#define PDE64_USER (1U << 2)
#define PDE64_PS (1U << 7)

// CR4
#define CR4_PAE (1U << 5)

// CR0
#define CR0_PE 1u
#define CR0_PG (1U << 31)

#define EFER_LME (1U << 8)
#define EFER_LMA (1U << 10)

#define SIZE2MB (2 * 1024 * 1024)

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

//  Struktura koja definise jednog gosta
//
//  vm_fd - fajl deskriptor koji komunicira sa odredjenim vm-om
//  vm_vcp - fajl deskriptor koji predstavlja virtuelni procesor
//  mem - memorija gosta
//  kvm_run - run struktura gosta  
struct guest {
    int vm_fd;
    int vm_vcpu;
    char* mem;
    struct kvm_run* kvm_run;
    int pty_master;
    int pty_slave;
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
    regs.rsp = 1 << 21;

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

    int starting_address;

    if (create_guest(hypervisor, vm) < 0) return -1;
    if (create_memory_region(vm, mem_size) < 0) return -1;
    if (create_vcpu(vm) < 0) return -1;
    if (create_kvm_run(hypervisor, vm) < 0) return - 1; 
    if ((starting_address = setup_long_mode(vm, mem_size, page_size)) < 0) return -1;
    if (setup_registers(vm) < 0) return -1;


    return starting_address;

}

int main(int argc, char* argv[]) {

    int opt;
    int memory = 0;
    enum PageSize page_size;
    struct hypervisor hypervisor;
    int starting_adress;

    struct option long_options[] = {
        {"memory", required_argument, 0, 'm'},
        {"page", required_argument, 0, 'p'},
        {"guest", no_argument, 0, 'g'},
        {0, 0, 0, 0,}
    };

    while ((opt = getopt_long(argc, argv, "m:p:g", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm':
                memory = atoi(optarg) * 1024 * 1024;
                break;
            case 'p':
                page_size = (atoi(optarg) == 4) ? KB4 : MB2;
                break;
            case 'g':
                break;
        }
    }


    if (init_hypervisor(&hypervisor) < 0) {
        printf("GRESKA: Nije moguce inicijalizovati hipervizora\n");
        exit(EXIT_FAILURE);
    }

    int num_of_vms = argc - optind;
    pthread_t* vms = (pthread_t*) malloc(sizeof(pthread_t) * (num_of_vms));

    for (int i = optind; i < argc; i++) {

        FILE* img = fopen(argv[i], "r");
        if (img == NULL) {
            printf("GRESKA: Nije omoguce otvoriti fajl %s\n", argv[i]);
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
        vms[argc - i - 1] = handle;
    }

    for (int i = 0; i < num_of_vms; i++) {
        pthread_join(vms[i], NULL);
    }

}