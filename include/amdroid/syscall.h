#ifndef AMDROID_SYSCALL_H
#define AMDROID_SYSCALL_H
#include "amdroid/types.h"
void syscall_init(void);
/* called from isr stub with frame pointer */
void syscall_handler(u64 *frame);
#endif
