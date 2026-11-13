/* pie.c — a position-independent program: its pointer table needs relocations,
 * so it runs correctly only if the loader applied them. Exit code 0 on success. */
#include "ulib.h"

static const char *const messages[] = { "pie: first\n", "pie: second\n", 0 };

int main(int argc, char **argv){
    (void)argc; (void)argv;
    for(int i = 0; messages[i]; i++) print(messages[i]);
    return messages[1] != 0 ? 0 : 1;
}
