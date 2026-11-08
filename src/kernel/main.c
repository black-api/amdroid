#include "amdroid/kernel.h"
#include "amdroid/console.h"
#include "amdroid/vga.h"
#include "amdroid/serial.h"
#include "amdroid/types.h"
#include "amdroid/pmm.h"
#include "amdroid/heap.h"
#include "amdroid/idt.h"
#include "amdroid/pic.h"
#include "amdroid/timer.h"
#include "amdroid/keyboard.h"
#include "amdroid/vfs.h"
#include "amdroid/fat32.h"
#include "amdroid/sched.h"
#include "amdroid/fb.h"
#include "amdroid/gui.h"
#include "amdroid/gdt.h"
#include "amdroid/fbcon.h"
#include "amdroid/auth.h"
#include "amdroid/ext2.h"
#include "amdroid/pci.h"
#include "amdroid/nic.h"
#include "amdroid/net.h"
#include "amdroid/vmm.h"

int shell_run(void); /* returns the VT the user switched to */
void shell_login(void);

/* multiboot2 tag parsing for framebuffer */
static void *fb_addr = NULL;
static u32 fb_pitch = 0, fb_w=0, fb_h=0, fb_bpp=0;

static void parse_mb2(u32 magic, u32 mbi){
    if(magic != 0x36D76289) return; /* MB2 magic */
    u8 *p = (u8*)(u64)mbi;
    u32 total = *(u32*)p;
    p += 8;
    u8 *end = (u8*)(u64)mbi + total;
    while(p < end){
        u32 type = *(u32*)p;
        u32 size = *(u32*)(p+4);
        if(size==0) break;
        if(type==8){ /* framebuffer tag */
            fb_addr = (void*)(u64)*(u64*)(p+8);
            fb_pitch = *(u32*)(p+16);
            fb_w = *(u32*)(p+20);
            fb_h = *(u32*)(p+24);
            fb_bpp = *(u8*)(p+28);
        }
        p += (size+7)&~7;
    }
}

void kernel_main(u32 magic, u32 mbi){
    console_init();
    gdt_init();
    console_puts("\n=== am-droid OS ===\n");
    console_printf("multiboot magic: 0x%x\n", magic);

    parse_mb2(magic, mbi);

    /* memory */
    pmm_init(mbi);
    heap_init();
    console_puts("[heap] ready\n");

    /* interrupts */
    idt_init();
    pic_init();
    timer_init(100);
    keyboard_init();
    console_puts("[idt/pic/timer/kbd] ready\n");

    /* enable interrupts */
    __asm__ volatile("sti");

    /* vfs */
    vfs_init();
    console_puts("[vfs] ready\n");
    if(fat32_init()==0) console_puts("[fat32] mounted\n");
    else console_puts("[fat32] no disk\n");

    /* framebuffer */
    console_printf("[fb] addr=%p pitch=%u w=%u h=%u bpp=%u\n", fb_addr, fb_pitch, fb_w, fb_h, fb_bpp);
    if(fb_addr){
        vmm_map_region((u64)fb_addr, (u64)fb_addr, fb_w*fb_h*4 + 0x1000);
        fb_init(fb_addr, fb_pitch, fb_w, fb_h, fb_bpp);
        if(fb_ready()){
            fbcon_init();
            console_printf("[fb] %ux%u %ubpp OK\n", fb_width(), fb_height(), fb_bpp);
        }
        else console_puts("[fb] init failed (bpp?)\n");
    } else {
        console_puts("[fb] no framebuffer (text mode)\n");
    }

    /* scheduler */
    sched_init();
    console_puts("[sched] ready\n");

    console_puts("\nType 'help' for commands.\n");

    if(ext2_mount() == 0) console_puts("[ext2] mounted on the secondary disk\n");
    pci_init();
    extern int xhci_init(void);
    xhci_init();
    if(nic_init() == 0){
        int how = net_init();
        if(how >= 0){
            u32 ip, gw; u8 m[6];
            net_info(&ip, &gw, m);
            console_printf("[net] up at %u.%u.%u.%u (%s)\n", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
                           how == 0 ? "DHCP" : "static");
        }
    }

    /* accounts: first boot creates root; then someone must log in */
    fbcon_set_active(1);
    auth_init();
    shell_login();

    /* VT manager: each UI returns the VT the user asked for (1..9) */
    int current_vt = fb_ready() ? 1 : 2;
    for(;;){
        if(current_vt == 1 && fb_ready()){
            console_puts("Launching GUI (Ctrl+Alt+2 for the text shell)...\n");
            fbcon_set_active(0);
            current_vt = gui_run();
        } else {
            console_printf("Shell on VT%d (Ctrl+Alt+1 for GUI)\n", current_vt);
            fbcon_set_active(1);
            current_vt = shell_run();
        }
        if(current_vt < 1 || current_vt > 9) current_vt = 2;
    }

    for(;;) __asm__ volatile("hlt");
}
