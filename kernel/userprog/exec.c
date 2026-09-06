#include "kernel/userprog/exec.h"
#include "arch/cpu.h"
#include "arch/x86/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "kernel/asm/stub.h"
#include "kernel/asmFunc.h"
#include "kernel/assert.h"
#include "kernel/auxv.h"
#include "kernel/fs/fs.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/mm/access.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/process.h"
#include "lib/rand/rand.h"
#include "lib/str/str.h"
#define PF_X 0x1
#define EFLAGS_MBS (1 << 1)
#define EFLAGS_IF_1 (1 << 9)
#define EFLAGS_IOPL_0 0
#define MAX_ARG_NR 16
#define MAX_ARG_STR_LEN 256
#define HEAP_ASLR_PAGES 2048

struct Elf64_Nhdr {
    uint32_t namesz;
    uint32_t descsz;
    uint32_t type;
};

#define NT_GNU_ABI_TAG 1
#define EM_X86_64 62
#define EM_386 3
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;
struct Elf32_Ehdr {
    unsigned char e_ident[16];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
};
struct Elf32_Phdr {
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};
enum segment_type {
    PT_NULL,
    PT_LOAD,
    PT_DYNAMIC,
    PT_INTERP,
    PT_NOTE,
    PT_SHLIB,
    PT_PHDR
};
typedef uint64_t Elf64_Addr, Elf64_Off, Elf64_Xword;
typedef uint32_t Elf64_Word;
typedef uint16_t Elf64_Half;
struct Elf64_Ehdr {
    unsigned char e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};
struct Elf64_Phdr {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};
struct Elf64_Dyn {
    int64_t d_tag;
    uint64_t d_val;
};
struct Elf64_Rela {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    int64_t r_addend;
};
#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_RELRENT 37
#define ELF64_R_TYPE(i) ((i) & 0xffffffffu)
#define R_X86_64_RELATIVE 8

struct wx_range {
    uint32_t base;
    uint32_t pages;
};

static void apply_rx(uint32_t base, uint32_t pages) {
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t pg = base + i * PAGE_SIZE;
        uint64_t *pte = pte_ptr(pg);
        if (pte != NULL && (*pte & PTE_P)) {
            *pte = (*pte & 0x000ffffffffff000ull) | pte_wx(PTE_P | PTE_U, 0, 1);
            __asm__ volatile("invlpg (%0)" : : "r"(pg) : "memory");
        }
    }
}

static void apply_relocs(uint32_t bias, uint32_t dyn_vaddr) {
    if (bias == 0 || dyn_vaddr == 0)
        return;
    struct Elf64_Dyn *d = (struct Elf64_Dyn *)(uintptr_t)(bias + dyn_vaddr);
    uint64_t rela = 0, relasz = 0, relaent = sizeof(struct Elf64_Rela);
    uint64_t relr = 0, relrsz = 0;
    for (int i = 0; d[i].d_tag != DT_NULL; i++) {
        if (d[i].d_tag == DT_RELA)
            rela = d[i].d_val;
        else if (d[i].d_tag == DT_RELASZ)
            relasz = d[i].d_val;
        else if (d[i].d_tag == DT_RELAENT)
            relaent = d[i].d_val;
        else if (d[i].d_tag == DT_RELR)
            relr = d[i].d_val;
        else if (d[i].d_tag == DT_RELRSZ)
            relrsz = d[i].d_val;
    }
    if (rela != 0 && relasz != 0) {
        for (uint64_t off = 0; off < relasz; off += relaent) {
            struct Elf64_Rela *r =
                (struct Elf64_Rela *)(uintptr_t)(bias + rela + off);
            if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE)
                *(uint64_t *)(uintptr_t)(bias + r->r_offset) =
                    bias + r->r_addend;
        }
    }
    if (relr == 0 || relrsz == 0)
        return;
    uint64_t where = 0;
    for (uint64_t off = 0; off < relrsz; off += sizeof(uint64_t)) {
        uint64_t w = *(uint64_t *)(uintptr_t)(bias + relr + off);
        if ((w & 1) == 0) {
            where = w;
            *(uint64_t *)(uintptr_t)(bias + where) += bias;
            where += sizeof(uint64_t);
        } else {
            uint64_t bitmap = w >> 1;
            for (int b = 0; b < 63; b++) {
                if (bitmap & (1ull << b)) {
                    uint64_t a = where + (uint64_t)b * sizeof(uint64_t);
                    *(uint64_t *)(uintptr_t)(bias + a) += bias;
                }
            }
            where += 63 * sizeof(uint64_t);
        }
    }
}

static int brk_page_taken(uint32_t v) {
    uint64_t *pde = pde_ptr(v);
    if (pde == NULL) {
        return 0;
    }
    if (*pde & 0x80) {
        return 1;
    }
    uint64_t *pte = pte_ptr(v);
    return (pte != NULL && (*pte & 1)) ? 1 : 0;
}

static uint32_t pick_brk_base(uint32_t image_end) {
    uint32_t brk_end = (image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (brk_end >= USER_LOW_CEILING) {
        return brk_end;
    }
    uint32_t span = (USER_LOW_CEILING - brk_end) / PAGE_SIZE;
    if (span > HEAP_ASLR_PAGES) {
        span = HEAP_ASLR_PAGES;
    }
    if (span == 0) {
        return brk_end;
    }
    uint32_t gap = (rand_u32() % span) * PAGE_SIZE;
    uint32_t brk_base = brk_end + gap;
    while (brk_base > brk_end && brk_page_taken(brk_base)) {
        brk_base -= PAGE_SIZE;
    }
    return brk_base;
}

static void scan_note_abi(int32_t fd, uint32_t base_off, uint32_t filesz,
                          int *is_linux) {
    sys_lseek(fd, base_off, SEEK_SET);
    uint32_t remaining = filesz;
    while (remaining >= sizeof(struct Elf64_Nhdr)) {
        struct Elf64_Nhdr nh;
        if (read_file(fd, &nh, sizeof(nh)) != sizeof(nh))
            break;
        uint32_t sz = sizeof(nh) + ((nh.namesz + 3) & ~3u) +
                      ((nh.descsz + 3) & ~3u);
        if (sz > remaining)
            break;
        if (nh.type == NT_GNU_ABI_TAG && nh.namesz >= 4) {
            char name[4];
            read_file(fd, name, 4);
            if (memcmp(name, "GNU", 4) == 0 && nh.descsz >= 8) {
                uint8_t desc[8];
                read_file(fd, desc, 8);
                if (desc[0] == 0) {
                    *is_linux = 1;
                    kprintf("[exec] Linux ELF detected (GNU ABI-tag)\n");
                }
            }
        }
        remaining -= sz;
        sys_lseek(fd, base_off + (filesz - remaining), SEEK_SET);
    }
}

static void fill_entry_regs(struct Registers *r, uint32_t entry, int is64,
                            uint32_t rsp, uint32_t argc, uint32_t argv_base) {
    memset(r, 0, sizeof(struct Registers));
    r->rip = entry;
    r->cs = is64 ? SELECTOR_USER64_CODE : SELECTOR_U_CODE;
    r->rflags = EFLAGS_IOPL_0 | EFLAGS_MBS | EFLAGS_IF_1;
    r->user_rsp = rsp;
    r->ss = SELECTOR_U_DATA;
    if (is64) {
        r->rdi = argc;
        r->rsi = argv_base;
    } else {
        r->rbx = argv_base;
        r->rcx = argc;
    }
}

static int32_t segment_load(int32_t fd, uint32_t offset, uint32_t filesz,
                            uint32_t memsz, uint32_t vaddr) {
    uint32_t vaddr_first_page = vaddr & 0xfffff000;
    uint32_t size_in_first_page = PAGE_SIZE - (vaddr & 0x00000fff);
    uint32_t occupy_pages =
        (memsz > size_in_first_page)
            ? DIV_ROUND_UP(memsz - size_in_first_page, PAGE_SIZE) + 1
            : 1;
    uint32_t vaddr_page = vaddr_first_page;
    for (uint32_t i = 0; i < occupy_pages; i++) {
        uint64_t *pde = pde_ptr(vaddr_page);
        uint64_t *pte = pte_ptr(vaddr_page);
        if (pde == NULL || (*pde & 0x80) || pte == NULL || !(*pte & 1)) {
            if (get_a_page(vaddr_page) == 0) {
                return -1;
            }
        }
        vaddr_page += PAGE_SIZE;
    }
    sys_lseek(fd, offset, SEEK_SET);
    read_file(fd, (void *)vaddr, filesz);
    return 0;
}

static int32_t load(const char *pathname, int *is64, int *is_linux,
                    uint32_t *phdr_vaddr, uint32_t *phentsize, uint32_t *phnum,
                    uint32_t *bias_out, uint32_t *brk_base_out) {
    *phdr_vaddr = 0;
    *phentsize = 0;
    *phnum = 0;
    *bias_out = 0;
    *brk_base_out = 0;
    int32_t ret = -1;
    uint32_t image_end = 0;
    unsigned char ident[16];
    int32_t fd = open_file(pathname, O_RDONLY);
    if (fd == -1) {
        return -1;
    }
    if (read_file(fd, ident, sizeof(ident)) != sizeof(ident)) {
        goto done;
    }

    if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' ||
        ident[3] != 'F' || (ident[4] != 1 && ident[4] != 2)) {
        goto done;
    }
    *is64 = (ident[4] == 2);
    *is_linux = 0;
    sys_lseek(fd, 0, SEEK_SET);
    if (*is64) {
        struct Elf64_Ehdr elf64_header;
        memset(&elf64_header, 0, sizeof(elf64_header));
        if (read_file(fd, &elf64_header, sizeof(elf64_header)) !=
            sizeof(elf64_header)) {
            goto done;
        }
        int is_dyn = (elf64_header.e_type == 3);
        if ((elf64_header.e_type != 2 && !is_dyn) ||
            elf64_header.e_machine != EM_X86_64 ||
            elf64_header.e_version != 1 || elf64_header.e_phnum > 1024 ||
            elf64_header.e_phentsize != sizeof(struct Elf64_Phdr)) {
            goto done;
        }

        uint32_t bias = 0;
        uint32_t dyn_vaddr = 0;
        if (is_dyn) {
            uint32_t min_v = 0xffffffffu, max_e = 0;
            Elf64_Off pho = elf64_header.e_phoff;
            for (uint32_t i = 0; i < elf64_header.e_phnum; i++) {
                struct Elf64_Phdr ph;
                sys_lseek(fd, pho, SEEK_SET);
                if (read_file(fd, &ph, sizeof(ph)) != sizeof(ph))
                    goto done;
                pho += elf64_header.e_phentsize;
                if (ph.p_type != PT_LOAD)
                    continue;
                if ((uint32_t)ph.p_vaddr < min_v)
                    min_v = (uint32_t)ph.p_vaddr;
                uint32_t e = (uint32_t)(ph.p_vaddr + ph.p_memsz);
                if (e > max_e)
                    max_e = e;
            }
            uint32_t span = max_e - min_v;
            uint32_t avail = USER_LOW_CEILING - USER_VADDR_START;
            if (min_v > max_e || span >= avail)
                goto done;
            uint32_t off = rand_u32() % (avail - span);
            off &= ~(PAGE_SIZE - 1);
            bias = USER_VADDR_START + off;
        }

        struct wx_range wx[16];
        int wxn = 0;
        Elf64_Off prog_header_offset = elf64_header.e_phoff;
        for (uint32_t prog_idx = 0; prog_idx < elf64_header.e_phnum;
             prog_idx++) {
            struct Elf64_Phdr prog64_header;
            memset(&prog64_header, 0, sizeof(prog64_header));
            sys_lseek(fd, prog_header_offset, SEEK_SET);
            if (read_file(fd, &prog64_header, sizeof(prog64_header)) !=
                sizeof(prog64_header)) {
                goto done;
            }
            if (prog64_header.p_type == PT_INTERP) {
                goto done;
            }

            if (prog64_header.p_type == PT_NOTE) {
                scan_note_abi(fd, (uint32_t)prog64_header.p_offset,
                              (uint32_t)prog64_header.p_filesz, is_linux);
                sys_lseek(fd,
                          prog_header_offset +
                              prog_idx * elf64_header.e_phentsize +
                              sizeof(struct Elf64_Phdr),
                          SEEK_SET);
            }

            if (prog64_header.p_type == PT_DYNAMIC)
                dyn_vaddr = (uint32_t)prog64_header.p_vaddr;

            if (elf64_header.e_phoff >= prog64_header.p_offset &&
                elf64_header.e_phoff <
                    prog64_header.p_offset + prog64_header.p_filesz) {
                if ((is_dyn && prog64_header.p_vaddr < USER_STACK3_VADDR) ||
                    (!is_dyn && prog64_header.p_vaddr >= USER_VADDR_START &&
                     prog64_header.p_vaddr < USER_STACK3_VADDR)) {
                    *phdr_vaddr = (uint32_t)(prog64_header.p_vaddr +
                                             (elf64_header.e_phoff -
                                              prog64_header.p_offset));
                    *phentsize = elf64_header.e_phentsize;
                    *phnum = elf64_header.e_phnum;
                }
            }
            if (prog64_header.p_type == PT_LOAD) {
                uint32_t va = (uint32_t)prog64_header.p_vaddr;
                if (!is_dyn &&
                    (va < USER_VADDR_START || va >= USER_STACK3_VADDR)) {
                    prog_header_offset += elf64_header.e_phentsize;
                    continue;
                }
                uint32_t map_at = va + bias;
                if (map_at >= USER_STACK3_VADDR)
                    goto done;
                if (segment_load(fd, (uint32_t)prog64_header.p_offset,
                                 (uint32_t)prog64_header.p_filesz,
                                 (uint32_t)prog64_header.p_memsz,
                                 map_at) == -1) {
                    goto done;
                }
                uint32_t seg_end = map_at + (uint32_t)prog64_header.p_memsz;
                if (seg_end > image_end) {
                    image_end = seg_end;
                }
                if ((prog64_header.p_flags & PF_X) && wxn < 16) {
                    uint32_t first = map_at & ~0xfffu;
                    uint32_t sz_first = PAGE_SIZE - (map_at & 0xfffu);
                    wx[wxn].base = first;
                    wx[wxn].pages =
                        (prog64_header.p_memsz > sz_first)
                            ? DIV_ROUND_UP(prog64_header.p_memsz - sz_first,
                                           PAGE_SIZE) +
                                  1
                            : 1;
                    wxn++;
                }
            }
            prog_header_offset += elf64_header.e_phentsize;
        }
        apply_relocs(bias, dyn_vaddr);
        for (int i = 0; i < wxn; i++)
            apply_rx(wx[i].base, wx[i].pages);
        ret = (int32_t)((uint64_t)elf64_header.e_entry + bias);
        *bias_out = bias;
        if (image_end <= USER_VADDR_START ||
            image_end + PAGE_SIZE >= USER_LOW_CEILING) {
            goto done;
        }
        *brk_base_out = pick_brk_base(image_end);
    } else {
        struct Elf32_Ehdr elf_header;
        struct Elf32_Phdr prog_header;
        memset(&elf_header, 0, sizeof(elf_header));
        if (read_file(fd, &elf_header, sizeof(elf_header)) !=
            sizeof(elf_header)) {
            goto done;
        }
        if (memcmp(elf_header.e_ident, "\177ELF\1\1\1", 7) ||
            elf_header.e_type != 2 || elf_header.e_machine != EM_386 ||
            elf_header.e_version != 1 || elf_header.e_phnum > 1024 ||
            elf_header.e_phentsize != sizeof(struct Elf32_Phdr)) {
            goto done;
        }

        Elf32_Off prog_header_offset = elf_header.e_phoff;
        for (uint32_t prog_idx = 0; prog_idx < elf_header.e_phnum; prog_idx++) {
            memset(&prog_header, 0, sizeof(prog_header));
            sys_lseek(fd, prog_header_offset, SEEK_SET);
            if (read_file(fd, &prog_header, sizeof(prog_header)) !=
                sizeof(prog_header)) {
                goto done;
            }

            if (prog_header.p_type == PT_NOTE) {
                scan_note_abi(fd, prog_header.p_offset, prog_header.p_filesz,
                              is_linux);
                sys_lseek(fd,
                          prog_header_offset +
                              prog_idx * elf_header.e_phentsize +
                              sizeof(struct Elf32_Phdr),
                          SEEK_SET);
            }

            if (elf_header.e_phoff >= prog_header.p_offset &&
                elf_header.e_phoff <
                    prog_header.p_offset + prog_header.p_filesz &&
                prog_header.p_vaddr >= USER_VADDR_START &&
                prog_header.p_vaddr < USER_STACK3_VADDR) {
                *phdr_vaddr =
                    (uint32_t)(prog_header.p_vaddr +
                               (elf_header.e_phoff - prog_header.p_offset));
                *phentsize = elf_header.e_phentsize;
                *phnum = elf_header.e_phnum;
            }
            if (prog_header.p_type == PT_LOAD &&
                prog_header.p_vaddr >= USER_VADDR_START &&
                prog_header.p_vaddr < USER_STACK3_VADDR) {
                if (segment_load(fd, prog_header.p_offset, prog_header.p_filesz,
                                 prog_header.p_memsz,
                                 prog_header.p_vaddr) == -1) {
                    goto done;
                }
                uint32_t seg_end = prog_header.p_vaddr + prog_header.p_memsz;
                if (seg_end > image_end) {
                    image_end = seg_end;
                }
                if (prog_header.p_flags & PF_X) {
                    uint32_t first = prog_header.p_vaddr & ~0xfffu;
                    uint32_t sz_first =
                        PAGE_SIZE - (prog_header.p_vaddr & 0xfffu);
                    uint32_t pages =
                        (prog_header.p_memsz > sz_first)
                            ? DIV_ROUND_UP(prog_header.p_memsz - sz_first,
                                           PAGE_SIZE) +
                                  1
                            : 1;
                    apply_rx(first, pages);
                }
            }
            prog_header_offset += elf_header.e_phentsize;
        }
        ret = elf_header.e_entry;
        if (image_end <= USER_VADDR_START ||
            image_end + PAGE_SIZE >= USER_LOW_CEILING) {
            goto done;
        }
        *brk_base_out = pick_brk_base(image_end);
    }
done:
    close_file(fd);
    return ret;
}
static int count_strs(const char *const *strs, uint32_t *lens, int kcaller) {
    int n = 0;
    if (strs == NULL)
        return 0;
    while (n < MAX_ARG_NR) {
        if (!kcaller && !user_range_readable((uint32_t)(uintptr_t)&strs[n],
                                             sizeof(char *))) {
            return -1;
        }
        if (strs[n] == NULL) {
            break;
        }
        int len = kcaller ? (int)strlen(strs[n])
                          : user_strnlen(strs[n], MAX_ARG_STR_LEN);
        if (len < 0) {
            return -1;
        }
        lens[n] = (uint32_t)len + 1;
        n++;
    }
    return n;
}

int32_t sys_execve(const char *path, const char *argv[], const char *envp[],
                   struct Registers *regs) {
    uint32_t argc;
    int32_t entry_point;
    struct task_struct *cur;
    uint32_t old_pml4_phys;
    uint32_t ustack_ptr;
    uint32_t argv_user_addrs[MAX_ARG_NR];
    uint32_t slens[MAX_ARG_NR];
    uint32_t envlens[MAX_ARG_NR];
    int32_t envc = 0;
    uint32_t argv_user_base;
    int32_t i;
    uint32_t slen;
    struct Registers *ps;
    int is64 = 0;
    int is_linux = 0;
    uint32_t aux_phdr_vaddr = 0;
    uint32_t aux_phentsize = 0;
    uint32_t aux_phnum = 0;
    uint32_t aux_random_addr = 0;
    uint32_t aux_bias = 0;
    uint32_t aux_brk_base = 0;
    cur = current;
    old_pml4_phys = cur->pml4_phys;
    if (cur->pml4_phys == 0) {
        cur->pml4_phys = (uint32_t)create_page_dir();
        process_activate(cur);
    }
    if (cur->userprog_v_addr.vaddr_bitmap.bits != NULL) {
        uint32_t bitmap_bytes =
            cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len;
        uint32_t bitmap_pg_cnt = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint32_t i = 0; i < bitmap_pg_cnt; i++) {
            free_kernel_page((uint32_t)cur->userprog_v_addr.vaddr_bitmap.bits +
                             i * PAGE_SIZE);
        }
        cur->userprog_v_addr.vaddr_bitmap.bits = NULL;
    }
    create_user_vaddr_bitmap(cur);
    {
        int kcaller = (regs != NULL) ? ((regs->cs & 3) == 0) : 1;
        argc = count_strs(argv, slens, kcaller);
        if (argc < 0) {
            return -1;
        }
        if (envp == NULL) {
            envc = 2;
            envlens[0] = (uint32_t)strlen("PATH=/") + 1;
            envlens[1] = (uint32_t)strlen("HOME=/") + 1;
        } else {
            envc = count_strs(envp, envlens, kcaller);
            if (envc < 0) {
                return -1;
            }
        }
    }
    entry_point = load(path, &is64, &is_linux, &aux_phdr_vaddr, &aux_phentsize,
                       &aux_phnum, &aux_bias, &aux_brk_base);
    if (entry_point == -1) {
        if (cur->pml4_phys != old_pml4_phys) {
            cur->pml4_phys = old_pml4_phys;
            page_dir_activate(cur);
        }
        return -1;
    }
    memcpy(cur->name, path, 15);
    cur->name[15] = 0;
    cur->user_brk = 0;
    cur->brk_base = aux_brk_base;
    signal_reset_user(cur);
    for (uint32_t sp = USER_STACK_BOTTOM; sp < USER_STACK_TOP;
         sp += PAGE_SIZE) {
        uint64_t *pde = pde_ptr(sp);
        uint64_t *pte = pte_ptr(sp);
        if (pde == NULL || pte == NULL || !(*pte & 1)) {

            if (get_a_page(sp) == 0) {
                kprintf("[exec] get_a_page for user stack failed\n");
                return -1;
            }
        }
    }
    cur->tls_base = 0;
    cur->tls_selector = 0;
    cur->tls_msr = 0;
    cur->errno = 0;
    cur->compat = is_linux;
    cur->stack_bottom = USER_STACK_BOTTOM;
    if (is_linux) {
        kprintf("[exec] task %d marked as Linux compat (ABI-tag detected)\n",
                cur->pid);
    }

    {
        uint32_t below = rand_u32() % (USER_STACK_PAGES - 3);
        uint32_t sub = (rand_u32() % (PAGE_SIZE / 8)) * 8;
        ustack_ptr = USER_STACK_TOP - below * PAGE_SIZE - sub;
    }
    {
        uint64_t r0 = rand_u64();
        uint64_t r1 = rand_u64();
        ustack_ptr -= 16;
        uint32_t *rnd = (uint32_t *)ustack_ptr;
        rnd[0] = (uint32_t)r0;
        rnd[1] = (uint32_t)(r0 >> 32);
        rnd[2] = (uint32_t)r1;
        rnd[3] = (uint32_t)(r1 >> 32);
        aux_random_addr = ustack_ptr;
    }
    kprintf("[exec] ASLR: base=0x%x brk=0x%x stack=0x%x\n", aux_bias,
            aux_brk_base, ustack_ptr);
    for (i = 0; i < MAX_ARG_NR; ++i) {
        argv_user_addrs[i] = 0;
    }
    if (argc > 0) {
        for (i = (int32_t)argc - 1; i >= 0; --i) {
            slen = slens[i];
            ustack_ptr -= slen;
            ustack_ptr &= ~(is64 ? 0x7u : 0x3u);
            if (ustack_ptr < cur->stack_bottom) {
                kprintf("[exec] argv too large for user stack\n");
                return -1;
            }
            memcpy((void *)ustack_ptr, argv[i], slen);
            argv_user_addrs[i] = ustack_ptr;
        }
    }
    {
        static const char *env_defaults[2] = {"PATH=/", "HOME=/"};
        const char *exefn = path;
        uint32_t envp_addrs[MAX_ARG_NR];
        uint32_t exefn_addr = 0;
        uint32_t aux_dst = 0;
        uint32_t aw = is64 ? 8u : 4u;
        uint32_t amask = is64 ? 0x7u : 0x3u;
        uint64_t aux[32];
        int naw = 0;
        int e;
#define PVAL(p, v)                                                             \
    do {                                                                       \
        if (is64)                                                              \
            *(uint64_t *)(p) = (uint64_t)(v);                                  \
        else                                                                   \
            *(uint32_t *)(p) = (uint32_t)(v);                                  \
    } while (0)
#define PSTACK(sp, v)                                                          \
    do {                                                                       \
        (sp) -= aw;                                                            \
        PVAL(sp, v);                                                           \
    } while (0)
#define A(t, v)                                                                \
    do {                                                                       \
        aux[naw++] = (uint64_t)(t);                                            \
        aux[naw++] = (uint64_t)(v);                                            \
    } while (0)
        A(AT_EXECFN, 0);
        A(AT_PAGESZ, PAGE_SIZE);
        A(AT_CLKTCK, 100);
        A(AT_ENTRY, entry_point);
        A(AT_PHDR, aux_phdr_vaddr + (is64 ? aux_bias : 0));
        A(AT_PHENT, aux_phentsize);
        A(AT_PHNUM, aux_phnum);
        if (is64)
            A(AT_BASE, aux_bias);
        A(AT_FLAGS, 0);
        A(AT_HWCAP, 0);
        A(AT_RANDOM, aux_random_addr);
        A(AT_NULL, 0);
#undef A

        slen = strlen(exefn) + 1;
        ustack_ptr -= slen;
        ustack_ptr &= ~amask;
        if (ustack_ptr < cur->stack_bottom) {
            return -1;
        }
        memcpy((void *)ustack_ptr, exefn, slen);
        exefn_addr = ustack_ptr;

        for (e = envc - 1; e >= 0; --e) {
            slen = envlens[e];
            ustack_ptr -= slen;
            ustack_ptr &= ~amask;
            if (ustack_ptr < cur->stack_bottom) {
                return -1;
            }
            memcpy((void *)ustack_ptr, envp ? envp[e] : env_defaults[e], slen);
            envp_addrs[e] = ustack_ptr;
        }

        ustack_ptr &= ~amask;
        ustack_ptr -= (uint32_t)naw * aw;
        aux_dst = ustack_ptr;
        if (ustack_ptr < cur->stack_bottom) {
            return -1;
        }
        aux[1] = exefn_addr;
        for (int k = 0; k < naw; k++) {
            PVAL(aux_dst + (uint32_t)k * aw, aux[k]);
        }

        PSTACK(ustack_ptr, 0);
        for (e = envc - 1; e >= 0; --e)
            PSTACK(ustack_ptr, envp_addrs[e]);
        PSTACK(ustack_ptr, 0);
        for (i = (int32_t)argc - 1; i >= 0; --i)
            PSTACK(ustack_ptr, argv_user_addrs[i]);
        PSTACK(ustack_ptr, argc);
        argv_user_base = ustack_ptr + aw;
#undef PVAL
#undef PSTACK
    }
    for (int32_t fd = 3; fd < MAX_FILES_OPEN_PER_PROC; ++fd) {
        if (cur->fd_table[fd] != (uint32_t)-1 && (cur->fd_cloexec >> fd) & 1) {
            cur->fd_cloexec &= ~(1ull << fd);
            close_file(fd);
        }
    }
    if (regs != NULL) {
        fill_entry_regs(regs, (uint32_t)entry_point, is64, ustack_ptr, argc,
                        argv_user_base);
        return 0;
    }

    ps =
        (struct Registers *)(cur->kernel_stack_top - THREAD_STACK_SIZE + 0x100);
    fill_entry_regs(ps, (uint32_t)entry_point, is64, ustack_ptr, argc,
                    argv_user_base);

    __asm__ volatile("mov %0, %%ds; mov %0, %%es; mov %0, %%fs;" ::"r"(
                         (uint16_t)SELECTOR_U_DATA)
                     : "memory");
    __asm__ volatile("mov %0, %%rsp; jmp intr_exit" : : "r"(ps) : "memory");
    return 0;
}

int32_t sys_execv(const char *path, const char *argv[],
                  struct Registers *regs) {
    return sys_execve(path, argv, NULL, regs);
}
