/* Icons, drawn from rectangles so no image files are needed. Each is 32x32. */
#include "amdroid/ui.h"
#include "amdroid/fb.h"

void ui_icon(int which, u32 x, u32 y, u32 c){
    int ix = (int)x, iy = (int)y;
    switch(which){
        case UI_ICO_START:                               /* four rounded tiles */
            ui_rrect(x + 3,  y + 3,  11, 11, 3, c);
            ui_rrect(x + 18, y + 3,  11, 11, 3, c);
            ui_rrect(x + 3,  y + 18, 11, 11, 3, c);
            ui_rrect(x + 18, y + 18, 11, 11, 3, c);
            break;
        case UI_ICO_SEARCH:                              /* lens ring + handle */
            ui_ring(ix + 13, iy + 13, 9, 3, c);
            ui_line(ix + 19, iy + 19, ix + 28, iy + 28, 3, c);
            break;
        case UI_ICO_FILES:                               /* folder */
            ui_rrect(x + 2, y + 6,  13, 7,  2, c);
            ui_rrect(x + 2, y + 10, 28, 18, 3, c);
            break;
        case UI_ICO_TASKS:                               /* bar chart, rounded tops */
            ui_rrect(x + 4,  y + 18, 6, 10, 2, c);
            ui_rrect(x + 13, y + 10, 6, 18, 2, c);
            ui_rrect(x + 22, y + 4,  6, 24, 2, c);
            break;
        case UI_ICO_SETTINGS: {                          /* sliders: tracks + round knobs */
            const int ys[3] = { 8, 16, 24 }, kx[3] = { 10, 21, 14 };
            for(int i = 0; i < 3; i++){
                ui_line(ix + 3, iy + ys[i], ix + 28, iy + ys[i], 2, c);
                ui_disc(ix + kx[i], iy + ys[i], 4, c);
            }
            break;
        }
        case UI_ICO_ABOUT:                               /* info badge */
            ui_ring(ix + 16, iy + 16, 12, 2, c);
            ui_disc(ix + 16, iy + 10, 2, c);
            ui_line(ix + 16, iy + 14, ix + 16, iy + 22, 3, c);
            break;
        case UI_ICO_CALC:                                /* rounded body, screen, keys */
            ui_rrect(x + 5, y + 2, 22, 28, 4, c);
            ui_rrect(x + 8, y + 6, 16, 5, 2, 0x000000);
            for(int r = 0; r < 2; r++)
                for(int k = 0; k < 3; k++)
                    ui_disc(ix + 11 + k * 6, iy + 18 + r * 6, 2, 0x000000);
            break;
        case UI_ICO_CLOCK:                               /* clock face + hands */
            ui_ring(ix + 16, iy + 16, 13, 2, c);
            ui_line(ix + 16, iy + 16, ix + 16, iy + 8,  2, c);   /* minute */
            ui_line(ix + 16, iy + 16, ix + 22, iy + 16, 2, c);   /* hour   */
            ui_disc(ix + 16, iy + 16, 2, c);
            break;
        case UI_ICO_NOTES:                               /* page with lines */
            ui_rrect(x + 6, y + 2, 20, 28, 3, c);
            for(int r = 0; r < 4; r++)
                ui_rrect(x + 10, y + 8 + r * 5, 12, 2, 1, 0x000000);
            break;
        case UI_ICO_NET:                                 /* signal bars */
            ui_rrect(x + 4,  y + 22, 4, 6,  1, c);
            ui_rrect(x + 11, y + 16, 4, 12, 1, c);
            ui_rrect(x + 18, y + 10, 4, 18, 1, c);
            ui_rrect(x + 25, y + 4,  4, 24, 1, c);
            break;
        case UI_ICO_BROWSER:                             /* globe */
            ui_ring(ix + 16, iy + 16, 13, 2, c);
            ui_line(ix + 3,  iy + 16, ix + 29, iy + 16, 2, c);   /* equator  */
            ui_line(ix + 16, iy + 3,  ix + 16, iy + 29, 2, c);   /* meridian */
            break;
        default:                                         /* terminal */
            ui_rrect(x + 2, y + 4, 28, 24, 4, c);
            ui_text(ix + 7, iy + 9, ">_", 0x000000, 1);
            break;
    }
}

/* The mouse pointer. */
#define CUR_W 12
#define CUR_H 16
static const char cursor_bitmap[CUR_H][CUR_W+1] = {
    "X           ", "XX          ", "X.X         ", "X..X        ",
    "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
    "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
    "X.X X..X    ", "XX  X..X    ", "     X..X   ", "      XX    ",
};

void ui_cursor(int x, int y){
    for(int row = 0; row < CUR_H; row++)
        for(int col = 0; col < CUR_W; col++){
            char ch = cursor_bitmap[row][col];
            if(ch == ' ') continue;
            u32 color = (ch == 'X') ? 0x000000 : C_TEXT;
            int px = x + col, py = y + row;
            if(px >= 0 && py >= 0) fb_putpixel((u32)px, (u32)py, color);
        }
}

