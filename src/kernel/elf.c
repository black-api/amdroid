/* elf.c — loads a static ELF64 executable (x86-64, ET_EXEC) from the disk
 * into a new address space and starts it as a process. */
#include "amdroid/elf.h"
#include "amdroid/fat32.h"
#include "amdroid/heap.h"
#include "amdroid/console.h"
#include "amdroid/sched.h"
#include "amdroid/vm.h"
#include "amdroid/auth.h"
#include "amdroid/types.h"
#include <string.h>

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_NEEDED 1
#define R_X86_64_RELATIVE 8
#define PIE_BASE (VM_USER_BASE + 0x100000)   /* where position-independent programs are placed */
#define PF_W 2
#define USER_STACK_PAGES 4
#define USER_STACK_TOP (VM_USER_BASE + 0x40000)

struct elf64_hdr {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

static int fail(const char *msg){
    console_printf("[elf] %s\n", msg);
    return -1;
}

int elf_load(const char *in_path, u64 *entry_out, u64 *cr3_out){
    /* FAT32 names are upper case: accept "hello.elf" too */
    char path[80];
    size_t n = strlen(in_path);
    int has_dot = strchr(in_path, '.') != 0;
    if(n + 5 >= sizeof path) return fail("path too long");
    for(size_t k = 0; k <= n; k++){
        char c = in_path[k];
        path[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if(!has_dot){                 /* "run cat" means CAT.ELF */
        strcpy(path + n, ".ELF");
    }
    if(!auth_may(path, 0)) return fail("permission denied");
    char *img; u32 size = 0;
    if(fat32_read_path(path, NULL, 0, &size) != 0) return fail("file not found");
    img = kmalloc(size + 1);
    if(!img) return fail("out of memory");
    u32 got = 0;
    if(fat32_read_path(path, img, size, &got) != 0) { kfree(img); return fail("read error"); }

    if(got < sizeof(struct elf64_hdr)) { kfree(img); return fail("file too small"); }
    struct elf64_hdr *h = (struct elf64_hdr*)img;
    if(h->ident[0] != 0x7F || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F'){
        kfree(img); return fail("not an ELF file");
    }
    if(h->ident[4] != 2 || h->ident[5] != 1){ kfree(img); return fail("need a 64-bit little-endian ELF"); }
    if(h->machine != 0x3E || (h->type != 2 && h->type != 3)){
        kfree(img); return fail("need an x86-64 executable (ET_EXEC or ET_DYN)");
    }
    /* position-independent programs (ET_DYN) are placed at PIE_BASE */
    u64 load_bias = h->type == 3 ? PIE_BASE : 0;
    if(h->phentsize != sizeof(struct elf64_phdr) || (u64)h->phoff + (u64)h->phnum * h->phentsize > got){
        kfree(img); return fail("bad program headers");
    }

    u64 cr3 = vm_new_space();
    if(!cr3){ kfree(img); return fail("out of memory"); }

    for(int i = 0; i < h->phnum; i++){
        struct elf64_phdr *ph = (struct elf64_phdr*)(img + h->phoff + (u64)i * h->phentsize);
        if(ph->type != PT_LOAD) continue;
        ph->vaddr += load_bias;
        if(ph->vaddr < VM_USER_BASE || ph->vaddr + ph->memsz > VM_USER_LIMIT || ph->memsz < ph->filesz){
            kfree(img); return fail("segment outside the user region");
        }
        if(ph->offset + ph->filesz > got){ kfree(img); return fail("segment past end of file"); }
        /* map every page the segment touches, then copy the file bytes in */
        u64 start = ph->vaddr & ~0xFFFULL;
        u64 end = (ph->vaddr + ph->memsz + 0xFFF) & ~0xFFFULL;
        for(u64 va = start; va < end; va += 4096){
            if(!vm_lookup(cr3, va)){
                if(!vm_alloc_user_page(cr3, va)){ kfree(img); return fail("out of memory"); }
            }
        }
        for(u64 k = 0; k < ph->filesz; k++){
            u64 va = ph->vaddr + k;
            u64 pa = vm_lookup(cr3, va & ~0xFFFULL) + (va & 0xFFF);
            ((u8*)(unsigned long)pa)[0] = (u8)img[ph->offset + k];
        }
        (void)PF_W;   /* segments are mapped writable for now */
    }

    /* stack: the top pages below USER_STACK_TOP */
    for(int i = 0; i < USER_STACK_PAGES; i++){
        u64 va = USER_STACK_TOP - (u64)(i + 1) * 4096;
        if(!vm_alloc_user_page(cr3, va)){ kfree(img); return fail("out of memory (stack)"); }
    }
    kfree(img);
    /* relocations: only the relative kind, which position-independent code needs
     * when it is loaded at a different address. Shared libraries are refused. */
    if(load_bias){
        for(int i = 0; i < h->phnum; i++){
            struct elf64_phdr *dp = (struct elf64_phdr*)(img + h->phoff + (u64)i * h->phentsize);
            if(dp->type != PT_DYNAMIC) continue;
            u64 rela = 0, relasz = 0, relaent = 24;
            struct { u64 tag, val; } *dyn = (void*)(img + dp->offset);
            for(u64 k = 0; (k + 1) * 16 <= dp->filesz; k++){
                if(dyn[k].tag == DT_NULL) break;
                if(dyn[k].tag == DT_RELA){ rela = dyn[k].val; }
                if(dyn[k].tag == DT_RELASZ) relasz = dyn[k].val;
                if(dyn[k].tag == DT_RELAENT) relaent = dyn[k].val;
                if(dyn[k].tag == DT_NEEDED){
                    kfree(img); return fail("shared libraries are not supported (link statically or as -pie)");
                }
            }
            /* DT_RELA is a virtual address: find its file offset through the segments */
            u64 rela_off = 0; int found = 0;
            for(int j = 0; j < h->phnum && !found; j++){
                struct elf64_phdr *lp = (struct elf64_phdr*)(img + h->phoff + (u64)j * h->phentsize);
                u64 lv = lp->vaddr - load_bias;      /* the segment's own (unbiased) address */
                if(lp->type == PT_LOAD && rela >= lv && rela < lv + lp->filesz){
                    rela_off = lp->offset + (rela - lv); found = 1;
                }
            }
            if(!found || relaent != 24){ kfree(img); return fail("bad relocation table"); }
            for(u64 r = 0; r + 24 <= relasz; r += 24){
                struct { u64 off, info; i64 add; } *e = (void*)(img + rela_off + r);
                if((e->info & 0xFFFFFFFFu) != R_X86_64_RELATIVE){ kfree(img); return fail("unsupported relocation"); }
                u64 target = load_bias ? e->off + load_bias : e->off;
                u64 val = (u64)((i64)load_bias + e->add);
                if(vm_copy_to_user(cr3, target, &val, 8) != 0){ kfree(img); return fail("relocation outside the image"); }
            }
        }
    }
    *entry_out = h->entry + load_bias;
    *cr3_out = cr3;
    return 0;
}

/* Builds argc/argv on the top of a new user stack. Returns the stack pointer
 * to start with; argv_va receives the address of the argv array. */
static u64 build_args(u64 cr3, int argc, char **argv, u64 *argv_va){
    u64 sp = USER_STACK_TOP - 64;
    u64 ptrs[16];
    if(argc > 15) argc = 15;
    for(int i = argc - 1; i >= 0; i--){
        u64 n = strlen(argv[i]) + 1;
        sp -= n;
        vm_copy_to_user(cr3, sp, argv[i], n);
        ptrs[i] = sp;
    }
    sp &= ~7ULL;
    sp -= 8 * (u64)(argc + 1);
    *argv_va = sp;
    for(int i = 0; i < argc; i++) vm_copy_to_user(cr3, sp + 8 * (u64)i, &ptrs[i], 8);
    u64 zero = 0;
    vm_copy_to_user(cr3, sp + 8 * (u64)argc, &zero, 8);
    return sp & ~15ULL;
}

/* Loads and starts a program with arguments, as the current user. */
int elf_spawn_args(const char *path, int argc, char **argv){
    u64 entry, cr3;
    if(elf_load(path, &entry, &cr3) != 0) return -1;
    u64 argv_va = 0;
    u64 sp = build_args(cr3, argc, argv, &argv_va);
    int pid = proc_create_user(path, entry, sp, cr3, (u64)argc);
    if(pid < 0){ fail("no process slot"); return -1; }
    proc_set_args(pid, (u64)argc, argv_va);
    return pid;
}

/* Loads and runs a program from the disk, waiting for it to exit. */
int elf_run_args(const char *path, int argc, char **argv){
    int pid = elf_spawn_args(path, argc, argv);
    if(pid < 0) return -1;
    int code = proc_wait(pid);
    console_printf("[elf] %s exited with %d\n", path, code);
    return code;
}

int elf_run(const char *path){
    char *argv[1] = { (char*)path };
    return elf_run_args(path, 1, argv);
}

/* Loads and starts a program without waiting for it (run <file> &). */
int elf_spawn(const char *path){
    char *argv[1] = { (char*)path };
    int pid = elf_spawn_args(path, 1, argv);
    if(pid >= 0) proc_detach(pid);
    return pid;
}
