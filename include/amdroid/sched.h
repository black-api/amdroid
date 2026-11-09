#ifndef AMDROID_SCHED_H
#define AMDROID_SCHED_H
#define MAX_PROCS 16
#include "amdroid/types.h"

/* Processes. pid 0 is the kernel itself (the shell and GUI run there).
 * User processes are preempted by the timer; kernel code is not, so kernel
 * data structures are never switched out half-updated. */

#define NSIG 32
#define SIGKILL 9
#define SIGTERM 15

void sched_init(void);
int  proc_uid(int pid);
void proc_set_uid(int pid, int uid);
void proc_set_args(int pid, u64 argc, u64 argv_va);   /* before the process first runs */
void proc_idle(void);                                  /* sleep until something happens */
int  proc_create_user(const char *name, u64 entry, u64 user_sp, u64 cr3, u64 arg); /* pid or -1; arg -> rdi */
int  proc_wait(int pid);          /* kernel: blocks until pid exits, returns its exit code */
int  proc_current(void);
void proc_yield(void);            /* kernel: give the CPU to other processes */
u64 *sched_switch(u64 *frame);    /* from the interrupt path: pick the next process */
u64 *proc_exit(u64 *frame, int code);
/* For ps and kill. Calls cb for every live process. */
typedef void (*proc_visit_fn)(int pid, const char *name, const char *state, void *arg);
void proc_visit(proc_visit_fn cb, void *arg);
int  proc_kill(int pid);          /* SIGKILL to another process; 0 on success */
int  proc_signal(int pid, int sig);
int  proc_sig_handler_set(int sig, u64 handler);   /* the caller: 0 default, 1 ignore, else a function */
u64 *proc_deliver_current(u64 *frame);             /* the frame to resume, or NULL if the caller died */
int  proc_sigreturn(u64 *frame);                    /* restores the state a handler interrupted */
void proc_detach(int pid);
/* For the task manager: the processes with their CPU time (TSC cycles) and resident pages. */
typedef struct { int pid, ppid; u32 serial; char name[32]; const char *state; u64 cycles; u64 pages; } proc_info_t;
int  proc_info(proc_info_t *out, int max, u64 *idle_cycles);
u64  sched_tsc(void);
void proc_sleep_ticks(u64 n);     /* wait, giving the CPU to other processes */        /* nobody will wait for it: reap on exit */
u64 *sched_preempt(u64 *frame);   /* timer: switch only if the interrupted code was user mode */

#endif
