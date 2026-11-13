/* syscall.c — the system call interface for user programs.
 *
 * Files: open() reads the whole file into a kernel buffer (for reading) or
 * starts an empty buffer (for writing). Writes go into the buffer and are
 * saved to the disk by close(), so a crash mid-program leaves the old file.
 * Every open is checked against the calling process's user ID. */
#include "amdroid/syscall.h"
#include "amdroid/console.h"
#include "amdroid/sched.h"
#include "amdroid/timer.h"
#include "amdroid/vm.h"
#include "amdroid/fat32.h"
#include "amdroid/auth.h"
#include "amdroid/heap.h"
#include "amdroid/keyboard.h"
#include "amdroid/net.h"
#include "amdroid/serial.h"
#include "amdroid/types.h"
#include <string.h>

#define FD_MAX 8

typedef struct {
    int used, write;
    char path[80];
    u8 *buf;
    u32 size, cap, pos;
} fd_t;

static fd_t fds[MAX_PROCS][FD_MAX];

/* User pointers must lie in the user region; kernel memory is never readable
 * through a system call. */
static int user_range(u64 p, u64 n){
    return p >= VM_USER_BASE && n <= VM_USER_LIMIT && p + n <= VM_USER_LIMIT && p + n >= p;
}

/* Copies a NUL-terminated path from the caller, upper-cased (FAT names). */
static int user_path(u64 va, char *out, int max){
    u64 cr3 = vm_read_cr3();
    for(int i = 0; i < max; i++){
        if(!user_range(va + i, 1)) return -1;
        char c;
        if(vm_copy_from_user(cr3, va + i, &c, 1) != 0) return -1;
        if(c == 0){ out[i] = 0; return i; }
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    return -1;
}

static fd_t *fd_get(int pid, u64 fd){
    if(fd < 3 || fd >= 3 + FD_MAX) return 0;
    fd_t *f = &fds[pid][fd - 3];
    return f->used ? f : 0;
}

static void fd_free(fd_t *f){
    if(f->buf) kfree(f->buf);
    memset(f, 0, sizeof *f);
}

/* Saves a file opened for writing. Returns 0 on success. */
static int fd_flush(int pid, fd_t *f){
    if(!f->write) return 0;
    int rc = fat32_write_path(f->path, f->buf, f->size);
    if(rc == 0) auth_claim_as(proc_uid(pid), f->path);
    return rc;
}

void syscall_release(int pid){
    for(int i = 0; i < FD_MAX; i++){
        fd_t *f = &fds[pid][i];
        if(f->used){ fd_flush(pid, f); fd_free(f); }
    }
}

static u64 do_open(int pid, u64 path_va, u64 flags){
    char path[80];
    if(user_path(path_va, path, sizeof path) < 0) return (u64)-1;
    int write = flags != 0;
    int uid = proc_uid(pid);
    if(!auth_may_as(uid, path, write)) return (u64)-1;
    int slot = -1;
    for(int i = 0; i < FD_MAX; i++) if(!fds[pid][i].used){ slot = i; break; }
    if(slot < 0) return (u64)-1;
    fd_t *f = &fds[pid][slot];
    memset(f, 0, sizeof *f);
    strcpy(f->path, path);
    f->used = 1;
    f->write = write;
    if(write){
        f->cap = 512;
        f->buf = kmalloc(f->cap);
        if(!f->buf){ memset(f, 0, sizeof *f); return (u64)-1; }
    } else {
        u32 size = 0;
        if(fat32_read_path(path, NULL, 0, &size) != 0){ memset(f, 0, sizeof *f); return (u64)-1; }
        f->buf = kmalloc(size + 1);
        if(!f->buf){ memset(f, 0, sizeof *f); return (u64)-1; }
        u32 got = 0;
        if(fat32_read_path(path, f->buf, size, &got) != 0){ fd_free(f); return (u64)-1; }
        f->size = got;
    }
    return (u64)(slot + 3);
}

static u64 do_write(int pid, u64 fd, u64 va, u64 len){
    if(fd == 1 || fd == 2){
        if(!user_range(va, len)) return (u64)-1;
        u64 cr3 = vm_read_cr3();
        for(u64 i = 0; i < len; i++){
            char c;
            vm_copy_from_user(cr3, va + i, &c, 1);
            console_putc(c);
        }
        console_puts("");
        return len;
    }
    fd_t *f = fd_get(pid, fd);
    if(!f || !f->write || !user_range(va, len)) return (u64)-1;
    if(f->size + len > f->cap){
        u32 nc = (f->size + (u32)len) * 2;
        u8 *nb = kmalloc(nc);
        if(!nb) return (u64)-1;
        if(f->buf) memcpy(nb, f->buf, f->size);
        if(f->buf) kfree(f->buf);
        f->buf = nb;
        f->cap = nc;
    }
    if(vm_copy_from_user(vm_read_cr3(), va, f->buf + f->size, len) != 0) return (u64)-1;
    f->size += (u32)len;
    return len;
}

static u64 do_read(int pid, u64 fd, u64 va, u64 len){
    if(!user_range(va, len)) return (u64)-1;
    u64 cr3 = vm_read_cr3();
    if(fd == 0){
        /* wait for at least one key; then take what is there */
        u64 n = 0;
        for(;;){
            int c = -1;
            if(serial_has_input()) c = serial_getc();
            else if(keyboard_has_input()) c = keyboard_getchar();
            if(c < 0){ proc_idle(); if(n) break; continue; }
            char ch = (char)c;
            if(vm_copy_to_user(cr3, va + n, &ch, 1) != 0) break;
            n++;
            if(ch == '\n' || n >= len) break;
        }
        return n;
    }
    fd_t *f = fd_get(pid, fd);
    if(!f || f->write) return (u64)-1;
    u64 avail = f->size - f->pos;
    u64 n = len < avail ? len : avail;
    if(n && vm_copy_to_user(cr3, va, f->buf + f->pos, n) != 0) return (u64)-1;
    f->pos += (u32)n;
    return n;
}

u64 *syscall_dispatch(u64 *frame){
    net_poll();                                         /* drain the NIC so packets are not lost */
    int pid = proc_current();
    u64 num = frame[14];                                   /* rax */
    u64 a1 = frame[9], a2 = frame[10], a3 = frame[11];     /* rdi, rsi, rdx */
    switch(num){
        case SYS_WRITE:
            frame[14] = do_write(pid, a1, a2, a3);
            break;
        case SYS_EXIT:
            return proc_exit(frame, (int)a1);
        case SYS_YIELD:
            frame[14] = 0;
            return sched_switch(frame);
        case SYS_SLEEP:
            proc_sleep_ticks(a1);       /* real sleep: no CPU while waiting */
            frame[14] = 0;
            break;
        case SYS_GETPID:
            frame[14] = (u64)pid;
            break;
        case SYS_TICKS:
            frame[14] = timer_ticks();
            break;
        case SYS_OPEN:
            frame[14] = do_open(pid, a1, a2);
            break;
        case SYS_READ:
            frame[14] = do_read(pid, a1, a2, a3);
            break;
        case SYS_CLOSE: {
            fd_t *f = fd_get(pid, a1);
            if(!f){ frame[14] = (u64)-1; break; }
            int rc = fd_flush(pid, f);
            fd_free(f);
            frame[14] = rc == 0 ? 0 : (u64)-1;
            break;
        }
        case SYS_UNLINK: {
            char path[80];
            if(user_path(a1, path, sizeof path) < 0 || !auth_may_as(proc_uid(pid), path, 1)){
                frame[14] = (u64)-1; break;
            }
            frame[14] = fat32_delete_path(path) == 0 ? 0 : (u64)-1;
            break;
        }
        case SYS_SIZE: {
            fd_t *f = fd_get(pid, a1);
            frame[14] = f ? (u64)(f->write ? f->size : f->size) : (u64)-1;
            break;
        }
        case SYS_GETUID:
            frame[14] = (u64)(u32)proc_uid(pid);
            break;
        case SYS_SIGACTION:
            frame[14] = proc_sig_handler_set((int)a1, a2) == 0 ? 0 : (u64)-1;
            break;
        case SYS_KILL:
            frame[14] = proc_signal((int)a1, (int)a2) == 0 ? 0 : (u64)-1;
            break;
        case SYS_SIGRETURN:
            if(proc_sigreturn(frame) != 0) return proc_exit(frame, -11);
            return frame;                  /* rax comes back from the saved state */
        default:
            console_printf("[syscall] unknown %llu\n", (unsigned long long)num);
            frame[14] = (u64)-1;
    }
    /* a signal may have arrived for this process while it was in the kernel */
    u64 *resume = proc_deliver_current(frame);
    if(!resume) return sched_switch(frame);
    return resume;
}
