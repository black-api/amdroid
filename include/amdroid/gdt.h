#ifndef AMDROID_GDT_H
#define AMDROID_GDT_H
#include "amdroid/types.h"

/* Selectors. User selectors carry RPL 3. */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x1B   /* 0x18 | 3 */
#define SEL_UDATA 0x23   /* 0x20 | 3 */
#define SEL_TSS   0x28

void gdt_init(void);
void gdt_set_rsp0(u64 rsp0);   /* kernel stack used when an interrupt arrives from user mode */
#endif
