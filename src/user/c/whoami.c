/* whoami.c — prints the user ID the program runs as. */
#include "ulib.h"
int main(int argc, char **argv){
    (void)argc; (void)argv;
    print("uid ");
    print_num(getuid());
    print("\n");
    return 0;
}
