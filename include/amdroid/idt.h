#ifndef AMDROID_IDT_H
#define AMDROID_IDT_H
#include "amdroid/types.h"

void idt_init(void);
void idt_set_gate(u8 n, u64 handler, u16 sel, u8 flags);

#endif
