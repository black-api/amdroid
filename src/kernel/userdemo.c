/* userdemo.c — starts a built-in ring-3 program and waits for it to exit. */
#include "amdroid/console.h"
#include "amdroid/pmm.h"
#include "amdroid/sched.h"
#include "amdroid/vm.h"
#include "amdroid/types.h"
#include <string.h>

extern u8 user_hello_start[], user_hello_end[];
extern u8 user_busy_start[], user_busy_end[];
extern u8 user_fault_start[], user_fault_end[];

/* Creates a process running the blob [start, end) at VM_USER_BASE, in its own
 * address space, with one stack page. Returns the pid or -1. */
static int spawn_blob(const char *name, u8 *start, u8 *end, u64 arg){
    u64 cr3 = vm_new_space();
    if(!cr3) return -1;
    u64 size = (u64)(end - start);
    u64 code = vm_alloc_user_page(cr3, VM_USER_BASE);
    if(!code || size > 4096) return -1;
    memcpy((void*)(unsigned long)code, start, size);
    /* the stack page sits above the code; the stack grows down from its top */
    u64 stack_page = VM_USER_BASE + 0x10000;
    if(!vm_alloc_user_page(cr3, stack_page)) return -1;
    return proc_create_user(name, VM_USER_BASE, stack_page + 4096, cr3, arg);
}

int userdemo_run(void){
    int a = spawn_blob("hello", user_hello_start, user_hello_end, 0);
    if(a < 0){ console_puts("[userdemo] could not start\n"); return -1; }
    console_printf("[userdemo] hello -> exit %d\n", proc_wait(a));

    /* two busy processes: their output interleaves only if they are preempted */
    int b = spawn_blob("busy-A", user_busy_start, user_busy_end, 'A');
    int c = spawn_blob("busy-B", user_busy_start, user_busy_end, 'B');
    if(b < 0 || c < 0){ console_puts("[userdemo] could not start busy\n"); return -1; }
    console_printf("[userdemo] busy A -> exit %d\n", proc_wait(b));
    console_printf("[userdemo] busy B -> exit %d\n", proc_wait(c));

    /* a process that touches kernel memory must be killed */
    int f = spawn_blob("fault", user_fault_start, user_fault_end, 0);
    console_printf("[userdemo] fault -> exit %d\n", proc_wait(f));
    return 0;
}
