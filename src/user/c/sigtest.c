/* sigtest.c — a handler for SIGUSR1 that prints, and ends on SIGTERM.
 * Run it in the background, then: kill -10 PID (handled), kill PID (ends it). */
#include "ulib.h"

static volatile int hits;

static void on_usr1(int sig){
    hits++;
    print("got signal ");
    print_num(sig);
    print("\n");
}

int main(int argc, char **argv){
    (void)argc; (void)argv;
    sigaction(SIGUSR1, on_usr1);
    print("sigtest ready\n");
    for(;;) yield();
}
