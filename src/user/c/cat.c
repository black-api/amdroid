/* cat.c — prints files: cat FILE... (no arguments: copies stdin to stdout). */
#include "ulib.h"

int main(int argc, char **argv){
    char buf[256];
    if(argc < 2){
        i64 n;
        while((n = read_fd(0, buf, sizeof buf)) > 0) write_fd(1, buf, (u64)n);
        return 0;
    }
    int status = 0;
    for(int i = 1; i < argc; i++){
        i64 fd = open_file(argv[i], 0);
        if(fd < 0){ print("cat: cannot open "); print(argv[i]); print("\n"); status = 1; continue; }
        i64 n;
        while((n = read_fd((int)fd, buf, sizeof buf)) > 0) write_fd(1, buf, (u64)n);
        close_fd((int)fd);
    }
    return status;
}
