/* echo.c — prints its arguments; shows how argc and argv arrive. */
#include "ulib.h"

int main(int argc, char **argv){
    for(int i = 0; i < argc; i++){
        print(argv[i]);
        print(i + 1 < argc ? " " : "\n");
    }
    return argc;
}
