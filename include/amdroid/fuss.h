#ifndef AMDROID_FUSS_H
#define AMDROID_FUSS_H
#include "amdroid/types.h"

/* Runs a Fuss program stored in the FAT32 root directory (8.3 name, e.g.
   "HELLO.FAS"; case does not matter). Returns 0 on success, 1 after a compile
   or runtime error (the error is already printed). */
int fuss_run_file(const char *name);

#endif
