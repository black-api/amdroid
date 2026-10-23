#include "amdroid/sched.h"
#include "amdroid/heap.h"
#include "amdroid/console.h"
#include "amdroid/types.h"

#define MAX_TASKS 16
#define TASK_STACK_SIZE (16*1024)

typedef enum { TASK_UNUSED, TASK_READY, TASK_RUNNING, TASK_DEAD } task_state_t;

typedef struct task {
    int id;
    char name[32];
    task_state_t state;
    void *stack;
    void *sp;        /* saved stack pointer */
    task_fn_t fn;
} task_t;

static task_t tasks[MAX_TASKS];
static int current = -1;
static int task_count = 0;

/* context switch in C using inline asm? We'll implement yield by longjmp-like.
   Since cooperative, we can use setjmp/longjmp if available. gcc provides __builtin_setjmp? 
   Use manual: save rsp/rbp into task, restore next. */

/* We'll implement switch via inline assembly */
static void switch_context(task_t *from, task_t *to){
    /* save current registers into from->sp */
    void *sp;
    __asm__ volatile(
        "mov %%rsp, %0\n"
        : "=r"(sp)
    );
    from->sp = sp;
    /* restore */
    __asm__ volatile(
        "mov %0, %%rsp\n"
        :
        : "r"(to->sp)
    );
    /* Note: this is a naive switch; function return addresses handled by stack.
       We need each new task to start by calling its function. Handle in spawn. */
}

/* Instead, implement tasks as functions that run and never return, with their own stack.
   Spawn sets up stack with a trampoline. */

static void task_trampoline(void){
    /* current task's fn */
    int id = current;
    tasks[id].fn();
    tasks[id].state = TASK_DEAD;
    sched_yield();
    for(;;) __asm__ volatile("hlt");
}

void sched_init(void){
    for(int i=0;i<MAX_TASKS;i++){ tasks[i].state=TASK_UNUSED; tasks[i].stack=NULL; }
    /* task 0 = idle/main, will be set by sched_start */
}

int sched_spawn(task_fn_t fn, const char *name){
    for(int i=0;i<MAX_TASKS;i++){
        if(tasks[i].state==TASK_UNUSED){
            tasks[i].id=i;
            int j=0; while(name[j] && j<31){ tasks[i].name[j]=name[j]; j++; } tasks[i].name[j]=0;
            tasks[i].fn=fn;
            tasks[i].state=TASK_READY;
            tasks[i].stack = kmalloc(TASK_STACK_SIZE);
            /* set up stack: push trampoline address so ret jumps there */
            u64 *sp = (u64*)((char*)tasks[i].stack + TASK_STACK_SIZE);
            /* align */
            sp = (u64*)(((u64)sp) & ~0xFULL);
            *--sp = (u64)task_trampoline;
            tasks[i].sp = sp;
            task_count++;
            return i;
        }
    }
    return -1;
}

void sched_yield(void){
    int prev = current;
    /* find next ready */
    int next = -1;
    for(int i=1;i<=MAX_TASKS;i++){
        int idx = (current + i) % MAX_TASKS;
        if(tasks[idx].state==TASK_READY){ next=idx; break; }
    }
    if(next<0 || next==current) return;
    if(prev>=0 && tasks[prev].state==TASK_RUNNING) tasks[prev].state=TASK_READY;
    tasks[next].state=TASK_RUNNING;
    current = next;
    if(prev>=0){
        switch_context(&tasks[prev], &tasks[next]);
    } else {
        /* first switch: just load next sp */
        __asm__ volatile("mov %0, %%rsp"::"r"(tasks[next].sp));
        /* now "return" into trampoline */
    }
}

void sched_tick(void){
    /* cooperative: don't preempt; just count. Could set flag. */
}

void sched_start(void){
    /* pick first ready task */
    for(int i=0;i<MAX_TASKS;i++){
        if(tasks[i].state==TASK_READY){ 
            tasks[i].state=TASK_RUNNING; current=i;
            __asm__ volatile("mov %0, %%rsp"::"r"(tasks[i].sp));
            /* jump to trampoline */
            __asm__ volatile("ret");
            break;
        }
    }
}

int sched_task_count(void){ return task_count; }
