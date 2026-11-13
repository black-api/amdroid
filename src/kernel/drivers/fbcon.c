/* fbcon.c — text console on the framebuffer.
 *
 * Text is stored in a ring of lines (scrollback). The visible screen shows the
 * last `rows` lines, or earlier ones when the view is scrolled back. Drawing
 * is only done while the terminal is active; otherwise output is buffered and
 * shown when the terminal comes back. */
#include "amdroid/fbcon.h"
#include "amdroid/fb.h"
#include "amdroid/heap.h"
#include "amdroid/timer.h"
#include "amdroid/font.h"
#include "amdroid/types.h"
#include <string.h>

#define HIST 1000
#define FG 0xE8E8F0
#define BG 0x0A0A0F
#define CURSOR 0x6C63FF

static int ready, active, dirty;
static int cols, rows;
static char *text;                 /* HIST lines of cols chars */
static u64 abs_line;               /* absolute index of the line being written */
static int cx;                     /* column of the next character */
static u64 view_off;               /* lines scrolled back from the bottom */
static u64 last_flush;
static int cur_drawn_x = -1, cur_drawn_row = -1;

static char *line_of(u64 abs){ return text + (abs % HIST) * cols; }

void fbcon_init(void){
    if(!fb_ready()) return;
    cols = (int)(fb_width() / FONT_W);
    rows = (int)(fb_height() / FONT_H);
    if(cols > 200) cols = 200;
    text = kmalloc((size_t)HIST * cols);
    if(!text) return;
    memset(text, ' ', (size_t)HIST * cols);
    ready = 1;
    active = 0;
    dirty = 1;
}

/* the absolute line shown at the top of the screen */
static u64 top_abs(void){
    u64 bottom = abs_line;
    u64 span = (u64)rows - 1;
    u64 top = bottom > span + view_off ? bottom - span - view_off : 0;
    return top;
}

static void draw_cell(int col, int row, char ch){
    if(col < 0 || col >= cols || row < 0 || row >= rows) return;
    fb_fill_rect((u32)(col * FONT_W), (u32)(row * FONT_H), FONT_W, FONT_H, BG);
    if(ch != ' ') fb_draw_char((u32)(col * FONT_W), (u32)(row * FONT_H), ch, FG, BG);
}

static void draw_cursor(void){
    if(!active || view_off != 0) return;
    int row = (int)(abs_line - top_abs());
    if(row < 0 || row >= rows) return;
    fb_fill_rect((u32)(cx * FONT_W), (u32)(row * FONT_H + FONT_H - 2), FONT_W, 2, CURSOR);
    cur_drawn_x = cx; cur_drawn_row = row;
}

static void erase_cursor(void){
    if(cur_drawn_x < 0) return;
    /* repaint the cell under the cursor from the text buffer */
    u64 abs = top_abs() + (u64)cur_drawn_row;
    char ch = (cur_drawn_x < cols) ? line_of(abs)[cur_drawn_x] : ' ';
    draw_cell(cur_drawn_x, cur_drawn_row, ch);
    cur_drawn_x = cur_drawn_row = -1;
}

void fbcon_redraw(void){
    if(!ready || !active) return;
    erase_cursor();
    fb_clear(BG);
    u64 top = top_abs();
    for(int r = 0; r < rows; r++){
        u64 abs = top + (u64)r;
        if(abs > abs_line) break;
        char *ln = line_of(abs);
        for(int c = 0; c < cols; c++) if(ln[c] != ' ') draw_cell(c, r, ln[c]);
    }
    draw_cursor();
    dirty = 1;
}

/* moves the screen up one line at the bottom edge (incremental, no repaint) */
static void new_line(void){
    abs_line++;
    memset(line_of(abs_line), ' ', (size_t)cols);
    if(view_off == 0 && active){
        erase_cursor();
        fb_scroll_up(FONT_H, BG);
        dirty = 1;
    }
    if(view_off && active) dirty = 1;
}

void fbcon_putc(char c){
    if(!ready) return;
    if(c == '\n'){ cx = 0; new_line(); return; }
    if(c == '\r'){ cx = 0; return; }
    if(c == '\b'){ if(cx > 0) cx--; return; }
    if(c == '\t'){
        int n = (8 - (cx % 8));
        for(int i = 0; i < n; i++) fbcon_putc(' ');
        return;
    }
    if(cx >= cols){ cx = 0; new_line(); }
    char *ln = line_of(abs_line);
    ln[cx] = c;
    if(view_off == 0 && active){
        erase_cursor();
        int row = (int)(abs_line - top_abs());
        draw_cell(cx, row, c);
        dirty = 1;
    }
    cx++;
}

void fbcon_set_active(int on){
    if(!ready) return;
    if(on == active) return;
    active = on;
    cur_drawn_x = cur_drawn_row = -1;
    if(on) fbcon_redraw();
    dirty = 1;
}

void fbcon_flush(int force){
    if(!ready || !active || !dirty) return;
    u64 now = timer_ticks();
    if(!force && now - last_flush < 2) return;     /* at most every 20 ms */
    draw_cursor();
    fb_swap();
    dirty = 0;
    last_flush = now;
}

void fbcon_scroll_view(int lines){
    if(!ready) return;
    u64 maxoff = abs_line >= (u64)rows ? abs_line - (u64)rows + 1 : 0;
    if(maxoff > (u64)(HIST - rows)) maxoff = (u64)(HIST - rows);
    if(lines < 0 && view_off < (u64)(-lines)) view_off = 0;
    else if(lines < 0) view_off -= (u64)(-lines);
    else view_off += (u64)lines;
    if(view_off > maxoff) view_off = maxoff;
    fbcon_redraw();
    fbcon_flush(1);
}
