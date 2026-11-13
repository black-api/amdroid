/* sched.c — process table and round-robin scheduler.
 *
 * Every process keeps its saved interrupt frame on its own kernel stack. A
 * switch means returning a different frame from the interrupt handler; the
 * stub in isr_stubs.asm restores registers from that frame and iretq's into
 * whichever process it belongs to. */
#include "amdroid/sched.h"
#include "amdroid/heap.h"
#include "amdroid/console.h"
#include "amdroid/gdt.h"
#include "amdroid/vm.h"
#include "amdroid/syscall.h"
#include "amdroid/net.h"
#include "amdroid/types.h"
#include <string.h>
#include "amdroid/timer.h"

static const char *state_name(int s);

#define KSTACK_SIZE (16 * 1024)
#define FRAME_WORDS 22      /* see isr_stubs.asm: 15 GPRs, vector, error, iret frame */

enum { P_FREE, P_READY, P_RUNNING, P_BLOCKED, P_ZOMBIE };

typedef struct proc {
    int pid;
    int state;
    char name[32];
    u64 *krsp;                  /* saved interrupt frame */
    u8 *kstack;
    u64 kstack_top;
    u64 cr3;
    int exit_code;
    int detached;               /* reaped automatically when it exits */
    int uid;                    /* who the process runs as */
    u32 sig_pending;            /* bit n = signal n waiting */
    u64 sig_handler[NSIG];      /* 0 = default, 1 = ignore, else a user function */
    int ppid;                   /* the process that started it */
    u32 serial;                 /* tells a new process from an old one that had the same pid */
    u64 cycles;                 /* CPU time used, in TSC cycles */
    u64 sleep_until;            /* in proc_sleep_ticks: not picked to run before this tick... */
    int wake;                   /* ...unless proc_wake set this */
    u8 fx[512] __attribute__((aligned(16)));   /* saved SSE/x87 state */
} proc_t;

static proc_t procs[MAX_PROCS];
static int cur = 0;
static u64 kernel_cr3;
static u64 loaded_cr3;

/* read by isr_stubs.asm: where the current process's SSE state is kept */
u8 *cur_fx_ptr;

static inline void yield_now(void){ __asm__ volatile("int $0x81"); }

/* ---- CPU accounting: cycles go to whoever ran since the last mark; time in hlt is idle ---- */
static u64 last_tsc = 0, idle_cycles = 0;
static u32 next_serial = 1;
static inline u64 rdtsc(void){ u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
static void charge(void){
    u64 now = rdtsc();
    if(last_tsc) procs[cur].cycles += now - last_tsc;
    last_tsc = now;
}
static void idle_hlt(void){
    charge();
    __asm__ volatile("sti; hlt");
    u64 now = rdtsc();
    idle_cycles += now - last_tsc;
    last_tsc = now;
}
u64 sched_tsc(void){ return rdtsc(); }

void sched_init(void){
    memset(procs, 0, sizeof procs);
    procs[0].pid = 0;
    procs[0].state = P_RUNNING;
    strcpy(procs[0].name, "kernel");
    kernel_cr3 = vm_kernel_cr3();
    procs[0].cr3 = kernel_cr3;
    procs[0].uid = 0;
    loaded_cr3 = kernel_cr3;
    cur = 0;
    cur_fx_ptr = procs[0].fx;
}

int proc_current(void){ return cur; }
int proc_uid(int pid){ return (pid >= 0 && pid < MAX_PROCS) ? procs[pid].uid : -1; }
void proc_set_uid(int pid, int uid){ if(pid >= 0 && pid < MAX_PROCS) procs[pid].uid = uid; }

/* Sets the arguments a new process starts with: argc in rdi, argv in rsi. */
void proc_set_args(int pid, u64 argc, u64 argv_va){
    if(pid <= 0 || pid >= MAX_PROCS || procs[pid].state != P_READY) return;
    u64 *f = procs[pid].krsp;
    f[9] = argc;
    f[10] = argv_va;
}

int proc_create_user(const char *name, u64 entry, u64 user_sp, u64 cr3, u64 arg){
    int pid = -1;
    for(int i = 1; i < MAX_PROCS; i++) if(procs[i].state == P_FREE){ pid = i; break; }
    if(pid < 0) return -1;
    proc_t *p = &procs[pid];
    memset(p, 0, sizeof *p);
    p->pid = pid;
    p->ppid = cur;
    p->serial = next_serial++;
    for(int k = 0; k < (int)sizeof p->name - 1 && name[k]; k++) p->name[k] = name[k];
    p->kstack = kmalloc(KSTACK_SIZE + 16);
    if(!p->kstack) return -1;
    p->kstack_top = ((u64)(unsigned long)p->kstack + KSTACK_SIZE) & ~0xFULL;
    /* initial frame: "return" from an interrupt into ring 3 at entry */
    u64 *f = (u64*)(unsigned long)(p->kstack_top - FRAME_WORDS * 8);
    memset(f, 0, FRAME_WORDS * 8);
    f[9] = arg;                /* rdi */
    f[17] = entry;             /* rip */
    f[18] = SEL_UCODE;         /* cs */
    f[19] = 0x202;            /* rflags: IF set */
    f[20] = user_sp;           /* rsp */
    f[21] = SEL_UDATA;         /* ss */
    p->krsp = f;
    p->cr3 = cr3;
    p->uid = procs[cur].uid;   /* inherits the creator's user */
    vm_map_trampoline(cr3);
    p->state = P_READY;
    /* default FPU/SSE state: FCW 0x037F, MXCSR 0x1F80 (all exceptions masked) */
    memset(p->fx, 0, sizeof p->fx);
    *(u16*)(p->fx + 0) = 0x037F;
    *(u32*)(p->fx + 24) = 0x1F80;
    return pid;
}

/* Frees a dead process's kernel stack and address space. Never the one running. */
static void reap(proc_t *p){
    if(p->kstack){ kfree(p->kstack); p->kstack = NULL; }
    if(p->cr3 && p->cr3 != kernel_cr3 && p->cr3 != loaded_cr3) vm_free_space(p->cr3);
    p->cr3 = 0;
    p->state = P_FREE;
}

/* READY and not asleep: a sleeper costs no CPU until its tick comes or it is woken,
 * so two sleeping processes let the machine halt instead of yielding to each other. */
static int runnable(int i){
    const proc_t *p = &procs[i];
    return p->state == P_READY && (p->wake || timer_ticks() >= p->sleep_until);
}

static int any_other_ready(void){
    for(int i = 0; i < MAX_PROCS; i++) if(i != cur && runnable(i)) return 1;
    return 0;
}

/* Picks the next READY process after the current one and switches to it. */
static void wake_waiters(void);

/* ---------- signals ----------
 * Signals are queued and delivered when the process is about to run. A handler
 * runs on the user stack; the interrupted state is saved below it and restored
 * by sigreturn, which the trampoline page calls when the handler returns. */
#define SIG_BLOCK_WORDS 18

static void terminate(proc_t *p, int code){
    syscall_release((int)(p - procs));
    p->exit_code = code;
    p->state = P_ZOMBIE;
    wake_waiters();
}

/* Delivers one pending signal to p (about to run). Returns 1 if p died. */
static int deliver(proc_t *p){
    if(!p->sig_pending) return 0;
    for(int sig = 1; sig < NSIG; sig++){
        if(!(p->sig_pending & (1u << sig))) continue;
        u64 h = p->sig_handler[sig];
        p->sig_pending &= ~(1u << sig);
        if(h == 1 && sig != SIGKILL) continue;               /* ignored */
        if(h == 0 || sig == SIGKILL){ terminate(p, 128 + sig); return 1; }
        /* a handler: save the state below the user stack, then run it */
        u64 *f = p->krsp;
        u64 blk[SIG_BLOCK_WORDS];
        memcpy(blk, f, 15 * 8);
        blk[15] = f[17]; blk[16] = f[19]; blk[17] = f[20];
        u64 b = ((f[20] - 256) & ~0xFULL) - 144;             /* 16-aligned, 144 bytes */
        u64 ret = b - 8;                                     /* the handler returns to the trampoline */
        u64 tramp = VM_SIG_TRAMPOLINE;
        if(vm_copy_to_user(p->cr3, b, blk, sizeof blk) != 0 ||
           vm_copy_to_user(p->cr3, ret, &tramp, 8) != 0){
            terminate(p, 128 + sig);
            return 1;
        }
        f[20] = ret;
        f[17] = h;
        f[9] = (u64)sig;                                     /* rdi: the signal number */
        return 0;
    }
    return 0;
}

int proc_signal(int pid, int sig){
    if(pid <= 0 || pid >= MAX_PROCS || sig < 1 || sig >= NSIG) return -1;
    proc_t *p = &procs[pid];
    if(p->state == P_FREE || p->state == P_ZOMBIE) return -1;
    if(sig == SIGKILL){
        if(pid == cur){ p->sig_pending |= 1u << sig; return 0; }   /* delivered on the way out */
        terminate(p, -9);
        return 0;
    }
    p->sig_pending |= 1u << sig;
    if(p->state == P_BLOCKED) p->state = P_READY;
    return 0;
}

int proc_sig_handler_set(int sig, u64 handler){
    if(sig < 1 || sig >= NSIG || sig == SIGKILL) return -1;
    procs[cur].sig_handler[sig] = handler;
    return 0;
}

/* For the current process at a system call return. NULL if it died. */
u64 *proc_deliver_current(u64 *frame){
    proc_t *p = &procs[cur];
    p->krsp = frame;
    if(deliver(p)) return 0;
    return p->krsp;
}

/* sigreturn: restores the state a handler was interrupted in. */
int proc_sigreturn(u64 *frame){
    proc_t *p = &procs[cur];
    u64 blk[SIG_BLOCK_WORDS];
    if(vm_copy_from_user(p->cr3, frame[20], blk, sizeof blk) != 0) return -1;
    memcpy(frame, blk, 15 * 8);
    frame[17] = blk[15];
    frame[19] = blk[16];
    frame[20] = blk[17];
    return 0;
}

u64 *sched_switch(u64 *frame){
    charge();
    proc_t *from = &procs[cur];
    from->krsp = frame;
    if(from->state == P_RUNNING) from->state = P_READY;

    proc_t *to = 0;
    for(;;){
        int next = -1;
        for(int k = 1; k <= MAX_PROCS; k++){
            int i = (cur + k) % MAX_PROCS;
            if(runnable(i)){ next = i; break; }
        }
        if(next < 0 && from->state != P_READY){
            /* the caller cannot go on (it exited or blocked) and the rest are asleep:
               run a sleeper, which halts in its own wait loop */
            for(int k = 1; k <= MAX_PROCS; k++){
                int i = (cur + k) % MAX_PROCS;
                if(procs[i].state == P_READY){ next = i; break; }
            }
        }
        if(next < 0){
            /* nothing else can run: keep the caller */
            from->state = from->state == P_READY ? P_RUNNING : from->state;
            return frame;
        }
        cur = next;
        to = &procs[cur];
        to->state = P_RUNNING;
        if(!deliver(to)) break;      /* the process died from a signal: pick again */
    }
    if(to->cr3 != loaded_cr3){ vm_switch(to->cr3); loaded_cr3 = to->cr3; }
    /* now that we are off the dead process's tables, reclaim the finished ones */
    for(int i = 1; i < MAX_PROCS; i++)
        if(i != cur && procs[i].state == P_ZOMBIE && procs[i].detached) reap(&procs[i]);
    if(to->kstack_top) gdt_set_rsp0(to->kstack_top);
    cur_fx_ptr = to->fx;
    return to->krsp;
}

/* Called by the timer. Only user-mode code is preempted. */
u64 *sched_preempt(u64 *frame){
    if((frame[18] & 3) != 3) return frame;
    return sched_switch(frame);
}

static void wake_waiters(void){
    for(int i = 0; i < MAX_PROCS; i++) if(procs[i].state == P_BLOCKED) procs[i].state = P_READY;
}

u64 *proc_exit(u64 *frame, int code){
    proc_t *p = &procs[cur];
    syscall_release(cur);            /* close its files (flushes writes) */
    p->exit_code = code;
    p->state = P_ZOMBIE;
    wake_waiters();
    return sched_switch(frame);
}

void proc_yield(void){ yield_now(); }

/* Idle: give the CPU away if someone else can run, otherwise sleep until the
 * next interrupt (the timer, a key, a packet). Keeps the CPU from spinning. */
void proc_idle(void){
    net_poll();                       /* keep the network moving while we wait */
    if(any_other_ready()){ yield_now(); return; }
    idle_hlt();
    __asm__ volatile("cli");
}

/* Waits n timer ticks, letting other processes run (the GUI uses this between frames).
 * proc_wake on the sleeper ends the wait early. */
void proc_sleep_ticks(u64 n){
    int me = cur;
    u64 target = timer_ticks() + n;
    procs[me].sleep_until = target;
    while(timer_ticks() < target && !procs[me].wake){
        if(any_other_ready()) yield_now();
        else idle_hlt();
    }
    procs[me].sleep_until = 0;
    procs[me].wake = 0;
}

void proc_wake(int pid){
    if(pid >= 0 && pid < MAX_PROCS && procs[pid].state != P_FREE) procs[pid].wake = 1;
}

/* 1 while pid can still run (it has not exited or been killed). */
int proc_alive(int pid){
    if(pid < 0 || pid >= MAX_PROCS) return 0;
    return procs[pid].state != P_FREE && procs[pid].state != P_ZOMBIE;
}

/* A snapshot of the processes for the task manager. */
int proc_info(proc_info_t *out, int max, u64 *idle){
    charge();
    int n = 0;
    for(int i = 0; i < MAX_PROCS && n < max; i++){
        proc_t *p = &procs[i];
        if(p->state == P_FREE) continue;
        proc_info_t *o = &out[n++];
        o->pid = p->pid;
        o->ppid = i == 0 ? -1 : p->ppid;
        o->serial = i == 0 ? 0 : p->serial;
        for(int k = 0; k < 32; k++) o->name[k] = p->name[k];
        o->state = state_name(p->state);
        o->cycles = p->cycles;
        o->pages = i == 0 ? 0 : vm_user_pages(p->cr3);
    }
    if(idle) *idle = idle_cycles;
    return n;
}

static const char *state_name(int s){
    switch(s){
        case P_READY: return "ready";
        case P_RUNNING: return "running";
        case P_BLOCKED: return "blocked";
        case P_ZOMBIE: return "zombie";
        default: return "free";
    }
}

void proc_visit(proc_visit_fn cb, void *arg){
    for(int i = 0; i < MAX_PROCS; i++)
        if(procs[i].state != P_FREE) cb(procs[i].pid, procs[i].name, state_name(procs[i].state), arg);
}

void proc_detach(int pid){
    if(pid > 0 && pid < MAX_PROCS) procs[pid].detached = 1;
}

/* Kills another process. Its waiter (if any) sees the exit code -9. */
int proc_kill(int pid){
    if(pid <= 0 || pid >= MAX_PROCS || pid == cur) return -1;
    int r = proc_signal(pid, SIGKILL);
    /* nobody will wait for a detached process: reclaim it now rather than
       waiting for the next context switch, which may not come while idle */
    proc_t *p = &procs[pid];
    if(r == 0 && p->state == P_ZOMBIE && p->detached && p->cr3 != loaded_cr3) reap(p);
    return r;
}

int proc_wait(int pid){
    if(pid <= 0 || pid >= MAX_PROCS) return -1;
    for(;;){
        proc_t *p = &procs[pid];
        if(p->state == P_ZOMBIE){
            int code = p->exit_code;
            reap(p);
            return code;
        }
        if(p->state == P_FREE) return -1;
        procs[cur].state = P_BLOCKED;
        proc_idle();
    }
}
