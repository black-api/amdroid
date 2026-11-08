#ifndef AMDROID_SYSCALL_H
#define AMDROID_SYSCALL_H
#include "amdroid/types.h"

/* System calls: int 0x80 with the number in rax and arguments in rdi, rsi,
 * rdx. The result comes back in rax (-1 on error). File descriptors 0, 1 and 2
 * are stdin, stdout and stderr; files opened with SYS_OPEN start at 3. */
#define SYS_WRITE  1   /* write(fd, buf, len) */
#define SYS_EXIT   2   /* exit(code): does not return */
#define SYS_YIELD  3   /* yield(): let other processes run */
#define SYS_GETPID 4
#define SYS_TICKS  5   /* ticks(): timer ticks since boot (100 Hz) */
#define SYS_OPEN   6   /* open(path, flags): flags 0 = read, 1 = write (create or replace) */
#define SYS_READ   7   /* read(fd, buf, len): stdin blocks until a key is pressed */
#define SYS_CLOSE  8   /* close(fd): a file opened for writing is saved to the disk here */
#define SYS_UNLINK 9   /* unlink(path) */
#define SYS_SIZE   10  /* size(fd) */
#define SYS_GETUID 11
#define SYS_SIGACTION 12  /* sigaction(sig, handler): 0 default, 1 ignore, else a function */
#define SYS_KILL 13       /* kill(pid, sig) */
#define SYS_SIGRETURN 14  /* used by the signal trampoline, not by programs */

void syscall_init(void);
u64 *syscall_dispatch(u64 *frame);   /* returns the frame to resume */
void syscall_release(int pid);       /* a process ended: close its files */
#endif
