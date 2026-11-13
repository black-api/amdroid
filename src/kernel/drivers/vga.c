#include "amdroid/vga.h"
#include "amdroid/types.h"

#define VGA_MEM ((volatile u16*)0xB8000)
#define VGA_W 80
#define VGA_H 25

static u8 vga_color = 0x0F; /* white on black */
static int vga_cx = 0, vga_cy = 0;

void vga_init(void){
    vga_clear();
}

static void vga_scroll(void){
    for(int y=1;y<VGA_H;y++)
        for(int x=0;x<VGA_W;x++)
            VGA_MEM[(y-1)*VGA_W+x] = VGA_MEM[y*VGA_W+x];
    for(int x=0;x<VGA_W;x++)
        VGA_MEM[(VGA_H-1)*VGA_W+x] = (u16)' ' | ((u16)vga_color<<8);
}

void vga_putc(char c){
    if(c=='\n'){ vga_cx=0; vga_cy++; }
    else if(c=='\r'){ vga_cx=0; }
    else if(c=='\b'){ if(vga_cx>0) vga_cx--; }
    else if(c=='\t'){ vga_cx = (vga_cx+8)&~7; }
    else {
        VGA_MEM[vga_cy*VGA_W+vga_cx] = (u16)c | ((u16)vga_color<<8);
        vga_cx++;
    }
    if(vga_cx>=VGA_W){ vga_cx=0; vga_cy++; }
    if(vga_cy>=VGA_H){ vga_scroll(); vga_cy=VGA_H-1; }
}

void vga_clear(void){
    for(int i=0;i<VGA_W*VGA_H;i++)
        VGA_MEM[i] = (u16)' ' | ((u16)vga_color<<8);
    vga_cx=0; vga_cy=0;
}
