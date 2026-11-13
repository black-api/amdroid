/* ulib.h — the user-program library: system calls and a few helpers.
 * Programs are freestanding (no libc) and are linked at VM_USER_BASE. */
#ifndef ULIB_H
#define ULIB_H

typedef unsigned long u64;
typedef long i64;

static inline i64 syscall3(i64 n, i64 a, i64 b, i64 c){
    i64 r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}
#define SYS_WRITE 1
#define SYS_EXIT 2
#define SYS_YIELD 3
#define SYS_GETPID 4
#define SYS_TICKS 5
#define SYS_OPEN 6
#define SYS_READ 7
#define SYS_CLOSE 8
#define SYS_UNLINK 9
#define SYS_SIZE 10
#define SYS_GETUID 11
#define SYS_SIGACTION 12
#define SYS_KILL 13
#define SYS_SIGRETURN 14
#define SIGKILL 9
#define SIGTERM 15
#define SIGUSR1 10

static inline i64 write_fd(int fd, const void *buf, u64 n){ return syscall3(SYS_WRITE, fd, (i64)buf, (i64)n); }
static inline i64 read_fd(int fd, void *buf, u64 n){ return syscall3(SYS_READ, fd, (i64)buf, (i64)n); }
static inline i64 open_file(const char *path, int write){ return syscall3(SYS_OPEN, (i64)path, write, 0); }
static inline i64 close_fd(int fd){ return syscall3(SYS_CLOSE, fd, 0, 0); }
static inline i64 unlink_file(const char *path){ return syscall3(SYS_UNLINK, (i64)path, 0, 0); }
static inline i64 file_size(int fd){ return syscall3(SYS_SIZE, fd, 0, 0); }
static inline i64 getuid(void){ return syscall3(SYS_GETUID, 0, 0, 0); }
static inline i64 getpid(void){ return syscall3(SYS_GETPID, 0, 0, 0); }
static inline void yield(void){ syscall3(SYS_YIELD, 0, 0, 0); }
/* handler: a function taking the signal number; 0 = default, 1 = ignore */
static inline i64 sigaction(int sig, void (*handler)(int)){ return syscall3(SYS_SIGACTION, sig, (i64)handler, 0); }
static inline i64 kill_pid(i64 pid, int sig){ return syscall3(SYS_KILL, pid, sig, 0); }
static inline void exit(int code){ for(;;) syscall3(SYS_EXIT, code, 0, 0); }

static inline u64 strlen_u(const char *s){ u64 n = 0; while(s[n]) n++; return n; }
static inline void puts_fd(int fd, const char *s){ write_fd(fd, s, strlen_u(s)); }
static inline void print(const char *s){ puts_fd(1, s); }
static inline void print_num(i64 v){
    char b[24]; int n = 0; u64 u;
    if(v < 0){ write_fd(1, "-", 1); u = (u64)(-v); } else u = (u64)v;
    do { b[n++] = (char)('0' + u % 10); u /= 10; } while(u);
    char out[24]; int k = 0;
    while(n) out[k++] = b[--n];
    write_fd(1, out, k);
}

int main(int argc, char **argv);
#endif
