/* A process that never exits, for testing ps and kill. */
#include "ulib.h"
int main(int argc, char **argv){
    (void)argc; (void)argv;
    for(;;) yield();
}
