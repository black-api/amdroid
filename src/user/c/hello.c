/* hello.c — prints a line and exits with code 3. */
#include "ulib.h"
int main(int argc, char **argv){
    (void)argc; (void)argv;
    print("hello from an ELF program\n");
    return 3;
}
