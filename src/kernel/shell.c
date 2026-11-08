#include "amdroid/console.h"
#include "amdroid/vga.h"
#include "amdroid/vfs.h"
#include "amdroid/fat32.h"
#include "amdroid/serial.h"
#include "amdroid/gui.h"
#include "amdroid/types.h"
#include "amdroid/timer.h"
#include "amdroid/pmm.h"
#include "amdroid/keyboard.h"
#include "amdroid/fuss.h"
#include <string.h>

static char linebuf[256];
static int linepos=0;

static void readline(void){
    linepos=0;
    memset(linebuf,0,sizeof(linebuf));
    for(;;){
        /* a VT switch request (Ctrl+Alt+N) abandons the line */
        if(keyboard_peek_requested_vt() != -1) return;
        int c = -1;
        if(serial_has_input()) c = serial_getc();
        else if(keyboard_has_input()) c = keyboard_getchar();
        if(c>=0){
            if(c=='\n' || c=='\r'){
                console_putc('\n');
                linebuf[linepos]=0;
                return;
            } else if(c=='\b'){
                if(linepos>0){ linepos--; console_puts("\b \b"); }
            } else if(c>=32 && c<127){
                if(linepos < 255){ linebuf[linepos++]=c; console_putc(c); }
            }
        } else {
            __asm__ volatile("hlt");
        }
    }
}

static void cmd_help(void){
    console_puts("Commands:\n");
    console_puts("  help        - show this help\n");
    console_puts("  ls [path]   - list directory\n");
    console_puts("  cat <file>  - print file\n");
    console_puts("  echo <text> - print text\n");
    console_puts("  pwd         - print working dir\n");
    console_puts("  free        - memory info\n");
    console_puts("  uptime      - ticks\n");
    console_puts("  clear       - clear screen\n");
    console_puts("  reboot      - reboot\n");
    console_puts("  uname       - OS info\n");
    console_puts("  diskls      - list FAT32 root\n");
    console_puts("  diskcat <f> - read FAT32 file\n");
    console_puts("  gui         - launch graphical UI\n");
    console_puts("  fuss <file> - run a Fuss program (.fas) from the disk\n");
}

static void cmd_ls(const char *arg){
    const char *p = arg && *arg ? arg : "/";
    vfs_node_t *n = vfs_lookup(p);
    if(!n){ console_puts("not found\n"); return; }
    if(!n->is_dir){ console_puts("not a dir\n"); return; }
    vfs_list(n);
}

static void diskls_cb(const char *name, int is_dir, u32 size){
    console_printf("%s%s  (%u bytes)\n", name, is_dir?"/":"", size);
}
static void cmd_diskls(void){ fat32_list_root(diskls_cb); }
static void cmd_diskcat(const char *arg){
    if(!arg||!*arg){ console_puts("usage: diskcat <file>\n"); return; }
    static char buf[65536];
    u32 sz=0;
    if(fat32_read_file(arg, buf, sizeof(buf)-1, &sz)!=0){ console_puts("not found\n"); return; }
    buf[sz]=0;
    console_puts(buf);
    if(sz>0 && buf[sz-1]!='\n') console_putc('\n');
}

static void cmd_cat(const char *arg){
    if(!arg || !*arg){ console_puts("usage: cat <file>\n"); return; }
    vfs_node_t *n = vfs_lookup(arg);
    if(!n){ console_puts("not found\n"); return; }
    if(n->is_dir){ console_puts("is dir\n"); return; }
    char buf[512];
    u64 off=0;
    while(off < n->size){
        int r = vfs_read(n, off, sizeof(buf)-1, buf);
        if(r<=0) break;
        buf[r]=0;
        console_puts(buf);
        off += r;
    }
}

/* Runs the text shell until a VT switch is requested. Returns that VT (1..9). */
int shell_run(void){
    for(;;){
        console_puts("am-droid$ ");
        readline();
        int rv = keyboard_get_requested_vt();
        if(rv != -1) return rv;
        /* parse */
        char *cmd = linebuf;
        while(*cmd==' ') cmd++;
        if(*cmd==0) continue;
        char *arg = cmd;
        while(*arg && *arg!=' ') arg++;
        if(*arg){ *arg=0; arg++; while(*arg==' ') arg++; }
        else arg = cmd+strlen(cmd); /* empty */

        if(strcmp(cmd,"help")==0) cmd_help();
        else if(strcmp(cmd,"ls")==0) cmd_ls(arg);
        else if(strcmp(cmd,"cat")==0) cmd_cat(arg);
        else if(strcmp(cmd,"echo")==0) console_printf("%s\n", arg);
        else if(strcmp(cmd,"pwd")==0) console_puts("/\n");
        else if(strcmp(cmd,"free")==0) console_printf("total pages: %llu free: %llu\n",
            (unsigned long long)pmm_total_pages(), (unsigned long long)pmm_free_pages());
        else if(strcmp(cmd,"uptime")==0) console_printf("ticks: %llu\n", (unsigned long long)timer_ticks());
        else if(strcmp(cmd,"clear")==0) vga_clear();
        else if(strcmp(cmd,"uname")==0) console_puts("am-droid 1.0 x86_64\n");
        else if(strcmp(cmd,"diskls")==0) cmd_diskls();
        else if(strcmp(cmd,"diskcat")==0) cmd_diskcat(arg);
        else if(strcmp(cmd,"fuss")==0){
            if(!arg || !*arg) console_puts("usage: fuss <file.fas>\n");
            else fuss_run_file(arg);
        }
        else if(strcmp(cmd,"gui")==0){
            int vt = gui_run();
            if(vt != 2) keyboard_request_vt(vt);   /* hand the switch back to the VT loop */
        }
        else if(strcmp(cmd,"reboot")==0){ /* triple fault / reboot via keyboard controller */
            __asm__ volatile("cli");
            /* pulse reset */
            unsigned char good=0x02;
            while(good & 0x02) { __asm__ volatile("inb $0x64,%0":"=a"(good)); }
            __asm__ volatile("outb %0,$0x64"::"a"((unsigned char)0xFE));
            for(;;) __asm__ volatile("hlt");
        }
        else console_printf("unknown command: %s\n", cmd);
    }
}
