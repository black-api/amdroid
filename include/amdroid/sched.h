#ifndef AMDROID_SCHED_H
#define AMDROID_SCHED_H
#include "amdroid/types.h"

typedef void (*task_fn_t)(void);

void sched_init(void);
int  sched_spawn(task_fn_t fn, const char *name);
void sched_yield(void);
void sched_tick(void);   /* called by timer */
void sched_start(void);  /* start scheduling (call last task) */
int  sched_task_count(void);

#endif
