#ifndef AMDROID_ELF_H
#define AMDROID_ELF_H
#include "amdroid/types.h"
int elf_load(const char *path, u64 *entry_out, u64 *cr3_out);
int elf_run(const char *path);   /* load, run as a process, wait for its exit code */
int elf_spawn(const char *path); /* load and start in the background; returns the pid or -1 */
int elf_run_args(const char *path, int argc, char **argv);
int elf_spawn_args(const char *path, int argc, char **argv);
#endif
