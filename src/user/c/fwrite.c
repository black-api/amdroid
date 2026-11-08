/* fwrite.c — writes a file: fwrite NAME TEXT (the text is saved on close). */
#include "ulib.h"

int main(int argc, char **argv){
    if(argc < 3){ print("usage: fwrite NAME TEXT\n"); return 2; }
    i64 fd = open_file(argv[1], 1);
    if(fd < 0){ print("fwrite: permission denied or bad name\n"); return 1; }
    write_fd((int)fd, argv[2], strlen_u(argv[2]));
    write_fd((int)fd, "\n", 1);
    if(close_fd((int)fd) != 0){ print("fwrite: cannot save the file\n"); return 1; }
    print("saved\n");
    return 0;
}
