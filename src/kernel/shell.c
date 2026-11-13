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

int userdemo_run(void);
#include "amdroid/husser.h"
#include "amdroid/elf.h"
#include "amdroid/rtc.h"
#include "amdroid/auth.h"
#include "amdroid/ext2.h"
#include "amdroid/usb.h"
#include "amdroid/hda.h"
#include "amdroid/pci.h"
#include "amdroid/hda.h"
#include "amdroid/net.h"
#include "amdroid/nic.h"
#include "amdroid/fat32.h"
#include "amdroid/heap.h"
#include "amdroid/sched.h"
#include "amdroid/fbcon.h"
#include "amdroid/fat32.h"
#include <string.h>

static char linebuf[256];
static int linelen = 0;        /* characters in linebuf */
static int curpos = 0;         /* cursor position inside linebuf */
static int drawn_len = 0;      /* length currently shown on screen */
static int shown_cur = 0;      /* where the cursor is on screen (curpos is where it should be) */

/* history: the last lines entered, newest last */
#define HIST_N 16
static char hist[HIST_N][256];
static int hist_n = 0;
static int hist_idx = 0;       /* while browsing: index into hist, hist_n = the live line */

static const char *cmd_names[] = {
    "help", "ls", "cat", "echo", "pwd", "free", "uptime", "clear", "uname", "reboot",
    "diskls", "diskcat", "fuss", "gui", "userdemo", "run", "ps", "kill", "date", "hussertest", 0
};

/* Redraws the input line after the cursor's old place. Backspace moves the
   cursor left on the console, so this works on the screen and on serial. */
static void redraw_line(void){
    for(int i = 0; i < shown_cur; i++) console_putc('\b');
    for(int i = 0; i < linelen; i++) console_putc(linebuf[i]);
    int extra = drawn_len - linelen;
    if(extra < 0) extra = 0;
    for(int i = 0; i < extra; i++) console_putc(' ');
    int back = linelen + extra - curpos;
    for(int i = 0; i < back; i++) console_putc('\b');
    drawn_len = linelen;
    shown_cur = curpos;
}

static void set_line(const char *text){
    int n = 0;
    while(text[n] && n < 255){ linebuf[n] = text[n]; n++; }
    linebuf[n] = 0;
    linelen = n;
    curpos = n;
    redraw_line();
}

static void insert_at_cursor(const char *text, int n){
    if(linelen + n > 255) return;
    for(int i = linelen; i >= curpos; i--) linebuf[i + n] = linebuf[i];
    for(int i = 0; i < n; i++) linebuf[curpos + i] = text[i];
    linelen += n;
    curpos += n;
    linebuf[linelen] = 0;
    redraw_line();
}

static void delete_range(int at, int n){
    for(int i = at; i + n <= linelen; i++) linebuf[i] = linebuf[i + n];
    linelen -= n;
    linebuf[linelen] = 0;
    if(curpos > at) curpos = curpos - n < at ? at : curpos - n;
    redraw_line();
}

/* root-directory file names collected for tab completion */
static char fnames[64][16];
static int nfnames;
static void collect_name(const char *name, int is_dir, u32 size){
    (void)size;
    if(is_dir || nfnames >= 64) return;
    int k = 0;
    while(name[k] && k < 15){ fnames[nfnames][k] = name[k]; k++; }
    fnames[nfnames][k] = 0;
    nfnames++;
}

static int upper(int c){ return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* Does s start with prefix (case-insensitive)? */
static int starts_with(const char *s, const char *prefix, int plen){
    for(int i = 0; i < plen; i++) if(upper((unsigned char)s[i]) != upper((unsigned char)prefix[i])) return 0;
    return 1;
}

/* Tab: completes the word before the cursor. Command names for the first word,
   root file names after it. */
static void complete(void){
    int ws = curpos;
    while(ws > 0 && linebuf[ws - 1] != ' ') ws--;
    int plen = curpos - ws;
    int first = ws == 0;
    const char *cands[64];
    int nc = 0;
    if(first){
        for(int i = 0; cmd_names[i] && nc < 64; i++)
            if(starts_with(cmd_names[i], linebuf + ws, plen)) cands[nc++] = cmd_names[i];
    } else {
        nfnames = 0;
        fat32_list_root(collect_name);
        for(int i = 0; i < nfnames && nc < 64; i++)
            if(starts_with(fnames[i], linebuf + ws, plen)) cands[nc++] = fnames[i];
    }
    if(nc == 0) return;
    /* longest common prefix of the candidates */
    int common = (int)strlen(cands[0]);
    for(int i = 1; i < nc; i++){
        int k = 0;
        while(k < common && cands[0][k] && cands[i][k] && upper((unsigned char)cands[0][k]) == upper((unsigned char)cands[i][k])) k++;
        common = k;
    }
    if(common > plen){
        insert_at_cursor(cands[0] + plen, common - plen);
    }
    if(nc == 1){
        if(first) insert_at_cursor(" ", 1);
        return;
    }
    if(common == plen){
        /* ambiguous: list the choices, then redraw the line */
        console_putc('\n');
        for(int i = 0; i < nc; i++){ console_puts(cands[i]); console_puts("  "); }
        console_putc('\n');
        console_puts("am-droid$ ");
        drawn_len = -1;   /* force a clean redraw */
        shown_cur = 0;
        redraw_line();
    }
}

static void history_add(const char *line){
    if(line[0] == 0) return;
    if(hist_n > 0 && !strcmp(hist[(hist_n - 1) % HIST_N], line)) return;
    strcpy(hist[hist_n % HIST_N], line);
    hist_n++;
}

static void readline(void){
    linelen = 0; curpos = 0; drawn_len = 0; shown_cur = 0;
    linebuf[0] = 0;
    hist_idx = hist_n;
    for(;;){
        /* a VT switch request (Ctrl+Alt+N) abandons the line */
        if(keyboard_peek_requested_vt() != -1) return;
        int c = -1;
        if(serial_has_input()) c = serial_getc();
        else if(keyboard_has_input()) c = keyboard_getchar();
        if(c < 0){
            /* idle: let other processes run (kernel code is not preempted) */
            fbcon_flush(0);
            net_poll();
            proc_yield();
            continue;
        }
        switch(c){
            case '\n': case '\r':
                console_putc('\n');
                linebuf[linelen] = 0;
                history_add(linebuf);
                fbcon_flush(1);
                return;
            case 3:                          /* Ctrl+C: abandon the line */
                console_puts("^C\n");
                linelen = 0; curpos = 0; shown_cur = 0; linebuf[0] = 0;
                fbcon_flush(1);
                return;
            case '\b': case 127:
                if(curpos > 0) delete_range(curpos - 1, 1);
                break;
            case KEY_DEL:
                if(curpos < linelen) delete_range(curpos, 1);
                break;
            case KEY_LEFT:
                if(curpos > 0){ curpos--; shown_cur--; console_putc('\b'); }
                break;
            case KEY_RIGHT:
                if(curpos < linelen){ console_putc(linebuf[curpos]); curpos++; shown_cur++; }
                break;
            case KEY_HOME: case 1:           /* Ctrl+A */
                while(curpos > 0){ curpos--; shown_cur--; console_putc('\b'); }
                break;
            case KEY_END: case 5:            /* Ctrl+E */
                while(curpos < linelen){ console_putc(linebuf[curpos]); curpos++; shown_cur++; }
                break;
            case 11:                         /* Ctrl+K: delete to end of line */
                delete_range(curpos, linelen - curpos);
                break;
            case 21:                         /* Ctrl+U: delete to start of line */
                delete_range(0, curpos);
                break;
            case KEY_UP:
                if(hist_idx > 0){ hist_idx--; set_line(hist[hist_idx % HIST_N]); }
                break;
            case KEY_DOWN:
                if(hist_idx < hist_n){
                    hist_idx++;
                    if(hist_idx == hist_n) set_line(""); else set_line(hist[hist_idx % HIST_N]);
                }
                break;
            case KEY_PGUP:
                fbcon_scroll_view(10);
                break;
            case KEY_PGDN:
                fbcon_scroll_view(-10);
                break;
            case '\t':
                complete();
                break;
            case 12:                         /* Ctrl+L: clear the screen */
                for(int i = 0; i < 50; i++) console_putc('\n');
                shown_cur = 0;
                drawn_len = -1;
                redraw_line();
                break;
            default:
                if(c >= 32 && c < 127 && linelen < 255){
                    char ch = (char)c;
                    insert_at_cursor(&ch, 1);
                }
                break;
        }
        fbcon_flush(1);
    }
}

/* asks for an account until one logs in */
void shell_login(void){
    for(;;){
        char u[16], p[64];
        console_puts("login: ");
        auth_read_line(u, sizeof u);
        console_puts("password: ");
        auth_read_secret(p, sizeof p);
        if(auth_login(u, p) == 0){
            console_printf("Welcome, %s.\n", auth_name());
            return;
        }
        console_puts("Login incorrect.\n");
    }
}

/* A dotted address or a host name (resolved with DNS). Returns 0 on success. */
static int host_ip(const char *h, u32 *ip){
    if(net_resolve(h, ip, 0, 0) == 0) return 0;
    return -1;
}

/* Copies a token (up to a space) from *p into out. */
static int take_word(const char **p, char *out, int max){
    const char *q = *p;
    while(*q == ' ') q++;
    int n = 0;
    while(*q && *q != ' ' && n < max - 1) out[n++] = *q++;
    out[n] = 0;
    *p = q;
    return n;
}

/* Fetches http://a.b.c.d:port/path into a new buffer (headers removed).
   Returns 0 and the body, or -1. */
static int http_fetch(u32 ip, u16 port, const char *path, u8 **body, u32 *blen){
    int sock = tcp_connect(ip, port);
    if(sock < 0) return -1;
    char req[300];
    int n = 0;
    const char *head = "GET ";
    for(const char *s2 = head; *s2; s2++) req[n++] = *s2;
    for(const char *s2 = path; *s2 && n < 250; s2++) req[n++] = *s2;
    const char *tail = " HTTP/1.0\r\n\r\n";
    for(const char *s2 = tail; *s2; s2++) req[n++] = *s2;
    if(tcp_send(sock, req, (u32)n) != 0){ tcp_close(sock); return -1; }
    u32 cap = 256 * 1024, len = 0;
    u8 *buf = kmalloc(cap);
    if(!buf){ tcp_close(sock); return -1; }
    for(;;){
        if(len + 1024 > cap){ tcp_close(sock); kfree(buf); return -1; }
        int got = tcp_recv(sock, buf + len, 1024, 3000);
        if(got <= 0) break;
        len += (u32)got;
    }
    tcp_close(sock);
    /* skip the header: body starts after CRLFCRLF */
    u32 start = 0;
    for(u32 i = 0; i + 3 < len; i++)
        if(buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n'){ start = i + 4; break; }
    if(start == 0 || buf[9] != '2'){ kfree(buf); return -1; }     /* not a 2xx response */
    memmove(buf, buf + start, len - start);
    *body = buf;
    *blen = len - start;
    return 0;
}

static void show_ext2(const char *name, int is_dir, u32 size){
    if(is_dir) console_printf("  %s/\n", name);
    else console_printf("  %-20s %u\n", name, size);
}

static void show_proc(int pid, const char *name, const char *state, void *arg){
    (void)arg;
    console_printf("  %3d  %-8s %s\n", pid, state, name);
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
    console_puts("  userdemo    - run a built-in ring 3 program\n");
    console_puts("  run <file>  - run a static ELF program from the disk\n");
    console_puts("  ps          - list processes\n");
    console_puts("  kill [-N] <pid> - send a signal (default: terminate)\n");
    console_puts("  date        - show the real-time clock\n");
    console_puts("  whoami      - show the current user\n");
    console_puts("  record <s> <FILE.WAV> - record from the sound card input (root)\n");
    console_puts("  usbdisk     - show a USB storage disk and its first sector\n");
    console_puts("  usbmount    - use the FAT32 file system on the USB disk\n");
    console_puts("  fsck        - free clusters that no file uses (after a crash)\n");
    console_puts("  usbumount   - back to the boot disk\n");
    console_puts("  mkdir <path> - make a directory on the disk\n");
    console_puts("  install a.b.c.d port /path NAME - download a program to the disk (root)\n");
    console_puts("  ifconfig    - show the network interface\n");
    console_puts("  ping <ip>   - ICMP echo\n");
    console_puts("  wget <host> <port> [/path] - HTTP GET over TCP\n");
    console_puts("  dig <name> [server [port]] - DNS lookup\n");
    console_puts("  listen <port> - accept one TCP connection and echo it\n");
    console_puts("  lspci       - list PCI devices\n");
    console_puts("  beep        - play a tone (sound card)\n");
    console_puts("  ext2ls [p]  - list a directory on the ext2 disk (read-only)\n");
    console_puts("  ext2cat <p> - show a file on the ext2 disk\n");
    console_puts("  ext2sum <p> - checksum a whole file on the ext2 disk\n");
    console_puts("  login       - log in as another user\n");
    console_puts("  adduser <n> - add an account (root)\n");
    console_puts("  passwd [n]  - change a password\n");
    console_puts("  chmod <m> <f> - change a file mode (octal)\n");
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
static void cmd_diskls(const char *arg){
    if(!arg || !*arg) fat32_list_root(diskls_cb);
    else if(fat32_list_path(arg, diskls_cb) != 0) console_puts("not a directory\n");
}
static void cmd_diskcat(const char *arg){
    if(!arg||!*arg){ console_puts("usage: diskcat <file>\n"); return; }
    static char buf[65536];
    u32 sz=0;
    char up[16]; int k = 0;
    while(arg[k] && k < 15){ char c = arg[k]; up[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; k++; }
    up[k] = 0;
    if(!auth_may(up, 0)){ console_puts("permission denied\n"); return; }
    if(fat32_read_file(up, buf, sizeof(buf)-1, &sz)!=0){ console_puts("not found\n"); return; }
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
        else if(strcmp(cmd,"diskls")==0) cmd_diskls(arg);
        else if(strcmp(cmd,"diskcat")==0) cmd_diskcat(arg);
        else if(strcmp(cmd,"ext2ls")==0){
            if(!ext2_mounted()) console_puts("no ext2 disk (attach one as the secondary IDE drive)\n");
            else if(ext2_list(arg && *arg ? arg : "/", show_ext2) != 0) console_puts("not a directory\n");
        }
        else if(strcmp(cmd,"ext2sum")==0){
            /* checksum of a whole ext2 file (exercises every block, including indirect ones) */
            static u8 sbuf[1024 * 1024];
            u32 sz = 0;
            if(!ext2_mounted()) console_puts("no ext2 disk\n");
            else if(!arg || !*arg) console_puts("usage: ext2sum <path>\n");
            else if(ext2_read(arg, sbuf, sizeof sbuf, &sz) != 0) console_puts("cannot read (or larger than 1 MB)\n");
            else {
                u32 sum = 0;
                for(u32 i = 0; i < sz; i++) sum = sum * 31 + sbuf[i];
                console_printf("%u bytes  sum %u\n", sz, sum);
            }
        }
        else if(strcmp(cmd,"ext2cat")==0){
            if(!ext2_mounted()) console_puts("no ext2 disk (attach one as the secondary IDE drive)\n");
            else if(!arg || !*arg) console_puts("usage: ext2cat <path>\n");
            else {
                static char ebuf[65536];
                u32 sz = 0;
                if(ext2_read(arg, ebuf, sizeof ebuf - 1, &sz) != 0) console_puts("not found\n");
                else { ebuf[sz < sizeof ebuf - 1 ? sz : sizeof ebuf - 1] = 0; console_puts(ebuf); if(sz && ebuf[sz - 1] != '\n') console_putc('\n'); }
            }
        }
        else if(strcmp(cmd,"lspci")==0){
            int n = pci_count();
            for(int i = 0; i < n; i++){
                pci_dev_t *d = pci_get(i);
                console_printf("%02x:%02x.%d %04x:%04x class %02x.%02x\n", d->bus, d->dev, d->fn,
                               d->vendor, d->device, d->class_code, d->subclass);
            }
        }
        else if(strcmp(cmd,"beep")==0){
            if(hda_play_tone(440, 1000) != 0) console_puts("beep: no sound device\n");
            else console_puts("beep\n");
        }
        else if(strcmp(cmd,"ifconfig")==0){
            if(!net_up()) console_puts("no network card\n");
            else {
                u32 ip, gw; u8 mac[6];
                net_info(&ip, &gw, mac);
                nic_t *n = nic_get();
                console_printf("%s  mac %02x:%02x:%02x:%02x:%02x:%02x\n", n->name,
                               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                u32 dns = net_dns();
                console_printf("  inet %u.%u.%u.%u  gateway %u.%u.%u.%u  dns %u.%u.%u.%u\n",
                               ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
                               gw >> 24, (gw >> 16) & 255, (gw >> 8) & 255, gw & 255,
                               dns >> 24, (dns >> 16) & 255, (dns >> 8) & 255, dns & 255);
            }
        }
        else if(strcmp(cmd,"ping")==0){
            u32 ip = 0;
            char h[64];
            const char *q = arg ? arg : "";
            take_word(&q, h, sizeof h);
            if(!net_up() || !h[0]) console_puts("usage: ping <host>  (needs a network card)\n");
            else if(host_ip(h, &ip) != 0) console_puts("ping: unknown host\n");
            else {
                u32 rtt = 0;
                if(net_ping(ip, 2000, &rtt) == 0) console_printf("reply from %u.%u.%u.%u: time=%u ms\n", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, rtt);
                else console_puts("no reply\n");
            }
        }
        else if(strcmp(cmd,"dig")==0){
            /* dig NAME [server [port]] */
            char h[64], srv[64], prt[16];
            const char *q = arg ? arg : "";
            take_word(&q, h, sizeof h);
            take_word(&q, srv, sizeof srv);
            take_word(&q, prt, sizeof prt);
            u32 server = 0, ip = 0;
            unsigned port = 0;
            for(const char *c = prt; *c >= '0' && *c <= '9'; c++) port = port * 10 + (unsigned)(*c - '0');
            if(srv[0] && host_ip(srv, &server) != 0){ console_puts("dig: bad server\n"); }
            else if(!h[0]) console_puts("usage: dig <name> [server [port]]\n");
            else if(!net_up()) console_puts("no network\n");
            else if(net_resolve(h, &ip, server, (u16)port) != 0) console_puts("dig: no answer\n");
            else console_printf("%s has address %u.%u.%u.%u\n", h, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        }
        else if(strcmp(cmd,"listen")==0){
            /* listen PORT : accepts one TCP connection, echoes what it sends, then closes */
            unsigned port = 0;
            for(const char *c = arg ? arg : ""; *c >= '0' && *c <= '9'; c++) port = port * 10 + (unsigned)(*c - '0');
            if(!net_up() || !port) console_puts("usage: listen <port>\n");
            else {
                int ls = tcp_listen((u16)port);
                if(ls < 0) console_puts("listen: no free socket\n");
                else {
                    console_printf("listening on port %u...\n", port);
                    int c = tcp_accept(ls, 60000);
                    if(c < 0) console_puts("listen: no connection\n");
                    else {
                        console_puts("connected\n");
                        char eb[256];
                        int got;
                        while((got = tcp_recv(c, eb, sizeof eb, 3000)) > 0){
                            if(tcp_send(c, eb, (u32)got) != 0) console_puts("[echo failed]\n");
                            eb[got] = 0;
                            console_puts(eb);
                        }
                        tcp_close(c);
                        console_puts("connection closed\n");
                    }
                    tcp_close(ls);
                }
            }
        }
        else if(strcmp(cmd,"wget")==0){
            /* wget HOST PORT [/path] : a plain HTTP/1.0 GET */
            char h[64], pt[16], path[120];
            const char *q = arg ? arg : "";
            take_word(&q, h, sizeof h);
            take_word(&q, pt, sizeof pt);
            if(!take_word(&q, path, sizeof path)) { path[0] = '/'; path[1] = 0; }
            unsigned port = 0;
            for(const char *c = pt; *c >= '0' && *c <= '9'; c++) port = port * 10 + (unsigned)(*c - '0');
            u32 ip = 0;
            if(!net_up() || !h[0] || !port) console_puts("usage: wget <host> <port> [/path]\n");
            else if(host_ip(h, &ip) != 0) console_puts("wget: unknown host\n");
            else {
                u8 *body = 0; u32 blen = 0;
                if(http_fetch(ip, (u16)port, path, &body, &blen) != 0) console_puts("wget: failed\n");
                else {
                    for(u32 i = 0; i < blen; i++) console_putc((char)body[i]);
                    if(blen && body[blen - 1] != '\n') console_putc('\n');
                    kfree(body);
                }
            }
        }
        else if(strcmp(cmd,"mkdir")==0){
            char p[120];
            const char *q = arg ? arg : "";
            if(!take_word(&q, p, sizeof p)) console_puts("usage: mkdir <path>\n");
            else {
                for(int i = 0; p[i]; i++) if(p[i] >= 'a' && p[i] <= 'z') p[i] = (char)(p[i] - 32);
                if(!auth_may(p, 1)) console_puts("mkdir: permission denied\n");
                else if(fat32_mkdir(p) != 0) console_puts("mkdir: cannot create (exists, bad name or disk full)\n");
                else { auth_claim(p); console_puts("ok\n"); }
            }
        }
        else if(strcmp(cmd,"install")==0){
            /* install HOST PORT /path NAME.EXT : download a program onto the disk (root) */
            char h[64], pt[16], path[120], dest[16];
            const char *q = arg ? arg : "";
            take_word(&q, h, sizeof h);
            take_word(&q, pt, sizeof pt);
            take_word(&q, path, sizeof path);
            take_word(&q, dest, sizeof dest);
            for(int i = 0; dest[i]; i++) if(dest[i] >= 'a' && dest[i] <= 'z') dest[i] = (char)(dest[i] - 32);
            unsigned port = 0;
            for(const char *c = pt; *c >= '0' && *c <= '9'; c++) port = port * 10 + (unsigned)(*c - '0');
            u32 ip = 0;
            if(!net_up() || !h[0] || !port || !path[0] || !dest[0]) console_puts("usage: install <host> <port> /path NAME.EXT\n");
            else if(auth_uid() != 0) console_puts("install: only root can install programs\n");
            else if(host_ip(h, &ip) != 0) console_puts("install: unknown host\n");
            else {
                u8 *body = 0; u32 blen = 0;
                if(http_fetch(ip, (u16)port, path, &body, &blen) != 0)
                    console_puts("install: download failed\n");
                else if(fat32_write_file(dest, body, blen) != 0)
                    console_puts("install: cannot write to the disk\n");
                else {
                    auth_claim(dest);
                    console_printf("installed %s (%u bytes)\n", dest, blen);
                }
                if(body) kfree(body);
            }
        }
        else if(strcmp(cmd,"fsck")==0){
            int n = fat32_recover();
            if(n < 0) console_puts("fsck: the disk could not be read\n");
            else console_printf("fsck: freed %d lost cluster(s)\n", n);
        }
        else if(strcmp(cmd,"usbmount")==0){
            if(fat32_on_usb()) console_puts("already on the USB disk\n");
            else if(fat32_mount_usb() != 0) console_puts("usbmount: no FAT32 file system on the USB disk\n");
            else console_puts("USB disk mounted: files now come from it (accounts stay as loaded)\n");
        }
        else if(strcmp(cmd,"usbumount")==0){
            if(!fat32_on_usb()) console_puts("the boot disk is in use\n");
            else if(fat32_mount_boot() != 0) console_puts("usbumount: boot disk error\n");
            else console_puts("boot disk mounted again\n");
        }
        else if(strcmp(cmd,"record")==0){
            /* record SECONDS FILE.WAV : captures from the sound card's input to a WAV file */
            char sec[8], fname[64];
            const char *q = arg ? arg : "";
            take_word(&q, sec, sizeof sec);
            take_word(&q, fname, sizeof fname);
            int secs = 0;
            for(const char *c = sec; *c >= '0' && *c <= '9'; c++) secs = secs * 10 + (*c - '0');
            if(secs < 1 || secs > 4 || !fname[0]) console_puts("usage: record <1-4 seconds> <FILE.WAV>\n");
            else if(auth_uid() != 0) console_puts("record: only root can write files here\n");
            else {
                for(int i = 0; fname[i]; i++) if(fname[i] >= 'a' && fname[i] <= 'z') fname[i] = (char)(fname[i] - 32);
                u32 nbytes = (u32)secs * 48000u * 4u;             /* stereo 16-bit 48 kHz */
                u8 *wav = kmalloc(44 + nbytes);
                if(!wav) console_puts("record: out of memory\n");
                else {
                    u32 frames = 0;
                    if(hda_record(secs * 1000, (int16_t*)(wav + 44), &frames) != 0){
                        console_puts("record: no audio input\n");
                    } else {
                        int peak = 0;
                        int16_t *pcm = (int16_t*)(wav + 44);
                        for(u32 i = 0; i < frames * 2; i++){ int v = pcm[i] < 0 ? -pcm[i] : pcm[i]; if(v > peak) peak = v; }
                        u32 dbytes = frames * 4;
                        memcpy(wav, "RIFF", 4);
                        u32 riff = 36 + dbytes; wav[4]=(u8)riff; wav[5]=(u8)(riff>>8); wav[6]=(u8)(riff>>16); wav[7]=(u8)(riff>>24);
                        memcpy(wav + 8, "WAVEfmt ", 8);
                        u32 fmtlen = 16; memcpy(wav + 16, &fmtlen, 4);
                        u16 fmt = 1, ch = 2; u32 rate = 48000, brate = 48000 * 4; u16 align = 4, bits = 16;
                        memcpy(wav + 20, &fmt, 2); memcpy(wav + 22, &ch, 2); memcpy(wav + 24, &rate, 4);
                        memcpy(wav + 28, &brate, 4); memcpy(wav + 32, &align, 2); memcpy(wav + 34, &bits, 2);
                        memcpy(wav + 36, "data", 4);
                        memcpy(wav + 40, &dbytes, 4);
                        if(fat32_write_path(fname, wav, 44 + dbytes) != 0) console_puts("record: cannot save the file\n");
                        else {
                            auth_claim(fname);
                            console_printf("recorded %u frames to %s, peak level %d of 32767\n", frames, fname, peak);
                        }
                    }
                    kfree(wav);
                }
            }
        }
        else if(strcmp(cmd,"usbdisk")==0){
            if(!usb_disk_present()) console_puts("no USB disk (attach a usb-storage device)\n");
            else {
                u32 blocks = 0, bsz = 0;
                char vend[9], prod[17];
                usb_disk_info(&blocks, &bsz, vend, prod);
                console_printf("%s %s: %u blocks x %u bytes = %u MB\n", vend, prod, blocks, bsz,
                               (u32)(((u64)blocks * bsz) >> 20));
                static u8 sec[512];
                if(usb_disk_read(0, 1, sec) != 0) console_puts("read failed\n");
                else {
                    console_printf("sector 0: signature %02x%02x%s\n", sec[510], sec[511],
                                   (sec[510] == 0x55 && sec[511] == 0xAA) ? " (boot sector)" : "");
                }
            }
        }
        else if(strcmp(cmd,"whoami")==0){
            if(auth_uid() < 0) console_puts("nobody (not logged in)\n");
            else console_printf("%s (uid %d)\n", auth_name(), auth_uid());
        }
        else if(strcmp(cmd,"login")==0){
            auth_logout();
            shell_login();
        }
        else if(strcmp(cmd,"logout")==0){
            auth_logout();
            shell_login();
        }
        else if(strcmp(cmd,"adduser")==0){
            if(!arg || !*arg) console_puts("usage: adduser <name>\n");
            else if(auth_uid() != 0) console_puts("adduser: only root can add accounts\n");
            else {
                char p1[64], p2[64];
                console_puts("password: "); auth_read_secret(p1, sizeof p1);
                console_puts("repeat:   "); auth_read_secret(p2, sizeof p2);
                if(strcmp(p1, p2) != 0 || !p1[0]) console_puts("passwords do not match\n");
                else if(auth_adduser(arg, p1) != 0) console_puts("adduser: failed (name taken or database full)\n");
                else console_printf("added %s\n", arg);
            }
        }
        else if(strcmp(cmd,"passwd")==0){
            const char *who = arg && *arg ? arg : auth_name();
            char p1[64], p2[64];
            console_puts("new password: "); auth_read_secret(p1, sizeof p1);
            console_puts("repeat:       "); auth_read_secret(p2, sizeof p2);
            if(strcmp(p1, p2) != 0 || !p1[0]) console_puts("passwords do not match\n");
            else if(auth_setpass(who, p1) != 0) console_puts("passwd: not allowed\n");
            else console_puts("password changed\n");
        }
        else if(strcmp(cmd,"chmod")==0){
            int mode = 0; const char *q = arg ? arg : "";
            while(*q >= '0' && *q <= '7'){ mode = mode * 8 + (*q - '0'); q++; }
            while(*q == ' ') q++;
            if(!*q || !*arg) console_puts("usage: chmod <octal mode> <file>\n");
            else if(auth_chmod(q, mode) != 0) console_puts("chmod: not allowed\n");
            else console_puts("ok\n");
        }
        else if(strcmp(cmd,"date")==0){
            rtc_time_t t; rtc_read(&t);
            console_printf("%u-%02u-%02u %02u:%02u:%02u UTC\n", t.year, t.month, t.day, t.hour, t.min, t.sec);
        }
        else if(strcmp(cmd,"ps")==0){
            console_puts("  PID  STATE    NAME\n");
            proc_visit(show_proc, NULL);
        }
        else if(strcmp(cmd,"kill")==0){
            /* kill [-SIG] PID : default signal is TERM */
            int sig = SIGTERM;
            const char *q = arg ? arg : "";
            if(*q == '-'){ q++; sig = 0; while(*q >= '0' && *q <= '9'){ sig = sig * 10 + (*q - '0'); q++; } while(*q == ' ') q++; }
            int pid = 0;
            for(const char *c = q; *c >= '0' && *c <= '9'; c++) pid = pid * 10 + (*c - '0');
            if(pid <= 0 || proc_signal(pid, sig) != 0) console_puts("kill: no such process\n");
        }
        else if(strcmp(cmd,"run")==0){
            /* run NAME [args...] [&] : arguments are split on spaces */
            static char rbuf[256];
            static char *rargv[9];
            int rargc = 0;
            int background = 0;
            if(!arg || !*arg){ console_puts("usage: run <program> [args...] [&]\n"); }
            else {
                int n = 0;
                while(arg[n] && n < 255){ rbuf[n] = arg[n]; n++; }
                rbuf[n] = 0;
                char *q = rbuf;
                while(*q){
                    while(*q == ' ') *q++ = 0;
                    if(!*q) break;
                    if(*q == '&' && q[1] == 0){ background = 1; *q = 0; break; }
                    if(rargc < 8) rargv[rargc++] = q;
                    while(*q && *q != ' ') q++;
                }
                if(rargc == 0) console_puts("usage: run <program> [args...] [&]\n");
                else {
                    rargv[rargc] = 0;
                    if(background){
                        int pid = elf_spawn_args(rargv[0], rargc, rargv);
                        if(pid >= 0){ proc_detach(pid); console_printf("[%d] started %s\n", pid, rargv[0]); }
                    } else {
                        elf_run_args(rargv[0], rargc, rargv);
                    }
                }
            }
        }
        else if(strcmp(cmd,"userdemo")==0){
            userdemo_run();
        }
        else if(strcmp(cmd,"hussertest")==0){
            /* spawn the Husser renderer, hand it a trivial page, and report */
            static const char test_html[] = "<h1>Husser smoke test</h1><p>The renderer is alive.</p>";
            int pid = husser_open(640, 400);
            if(pid < 0){ console_puts("hussertest: could not spawn husser\n"); continue; }
            console_printf("hussertest: husser pid %d\n", pid);
            husser_load(test_html, (u32)sizeof(test_html) - 1, 0);
            for(int i = 0; i < 100; i++){
                husser_pump();
                proc_sleep_ticks(5);
                const volatile husser_shm_t *h = husser_header();
                if(h && h->frame_seq > 0) break;
            }
            const volatile husser_shm_t *h = husser_header();
            if(!h){ console_puts("hussertest: header gone\n"); continue; }
            console_printf("hussertest: state=%u frames=%u size=%ux%u page_h=%u alive=%d\n",
                           h->state, h->frame_seq, h->frame_w, h->frame_h, h->page_h, husser_alive());
            husser_close();
            console_puts("hussertest: done\n");
        }
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
