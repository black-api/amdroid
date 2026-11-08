/* ulib.c — the program entry point: the kernel starts _start with argc and argv. */
#include "ulib.h"

void _start(i64 argc, char **argv){
    exit(main((int)argc, argv));
}
