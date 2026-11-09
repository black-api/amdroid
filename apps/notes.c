/* Notes: a plain text note saved to NOTES.TXT on the disk. Save with the button, or
 * close the window: unsaved changes are written then. */
#include "amdroid/ui.h"
#include "amdroid/fat32.h"

#define NOTE_FILE  "NOTES.TXT"
#define NOTE_MAX   2000
#define LINES_MAX  256

static char text[NOTE_MAX + 1];
static u32 len = 0;
static int loaded = 0;
static int dirty = 0;
static char status[48] = "";
static rect_t last_body;

/* wrapped lines: start offset and length in text */
static u32 ls_start[LINES_MAX], ls_len[LINES_MAX];
static int nlines = 0;

static void load(void){
    u32 size = 0;
    len = 0;
    if(fat32_read_path(NOTE_FILE, text, NOTE_MAX, &size) == 0){
        len = size < NOTE_MAX ? size : NOTE_MAX;
        ui_copy(status, "Loaded NOTES.TXT", sizeof status);
    } else {
        ui_copy(status, "New note", sizeof status);
    }
    text[len] = 0;
    dirty = 0;
    loaded = 1;
}

static void save(void){
    if(fat32_write_path(NOTE_FILE, text, len) == 0){
        dirty = 0;
        ui_copy(status, "Saved", sizeof status);
    } else {
        ui_copy(status, "Save failed (is the disk writable?)", sizeof status);
    }
}

/* Splits the text into lines that fit the width. */
static void wrap(int cols){
    nlines = 0;
    u32 start = 0; int col = 0;
    ls_start[0] = 0;
    for(u32 i = 0; i <= len; i++){
        int at_end = i == len;
        if(at_end || text[i] == '\n' || col == cols){
            if(nlines < LINES_MAX){
                ls_len[nlines] = i - start;
                nlines++;
            }
            if(at_end) break;
            if(nlines >= LINES_MAX) break;
            start = (text[i] == '\n') ? i + 1 : i;
            ls_start[nlines] = start;
            col = 0;
            if(text[i] == '\n') continue;
        }
        col++;
    }
    if(nlines == 0){ ls_start[0] = 0; ls_len[0] = 0; nlines = 1; }
}

static rect_t save_btn(rect_t b){ return ui_button(b, 140); }

static void n_open(void){
    if(!loaded) load();
}
static void n_close(void){
    if(dirty) save();
}

static void n_draw(rect_t b, int focused){
    (void)focused;
    last_body = b;
    if(!loaded) load();

    rect_t sb = save_btn(b);
    ui_fill(sb, dirty ? ui_accent() : C_CARD_HI);
    ui_text(sb.x + 30, sb.y + 4, dirty ? "Save*" : "Save", dirty ? C_BAR : C_TEXT, UI_SCALE);

    u32 cols = (b.w - 16) / UI_CW;
    wrap((int)cols);
    int rows = (int)(b.h - 64 - 28) / UI_CH;
    if(rows < 1) rows = 1;
    int first = nlines > rows ? nlines - rows : 0;      /* keep the end in view */
    for(int i = first; i < nlines; i++){
        char line[80];
        u32 n = ls_len[i];
        if(n > 79) n = 79;
        for(u32 k = 0; k < n; k++) line[k] = text[ls_start[i] + k];
        line[n] = 0;
        ui_text(b.x + 8, b.y + 64 + (u32)(i - first) * UI_CH, line, C_TEXT, UI_SCALE);
    }
    /* the caret at the end of the text */
    int last = nlines - 1 - first;
    if(last >= 0 && last < rows){
        u32 x = b.x + 8 + ls_len[nlines - 1] * UI_CW;
        ui_fill((rect_t){ x, b.y + 64 + (u32)last * UI_CH + 6, 3, UI_CH - 10 }, ui_accent());
    }
    ui_text(b.x, b.y + b.h - 24, status, C_DIM, 1);
}

static void n_click(rect_t b, int x, int y){
    if(ui_contains(save_btn(b), x, y)) save();
}

static int n_key(int c){
    if(c == '\b'){
        if(len > 0){ len--; text[len] = 0; dirty = 1; }
        return 1;
    }
    if(c == '\n' || (c >= 32 && c <= 126)){
        if(len < NOTE_MAX){
            text[len++] = (char)c;
            text[len] = 0;
            dirty = 1;
        }
        return 1;
    }
    return 0;
}

const app_t app_notes = {
    .name = "Notes",
    .icon = UI_ICO_NOTES,
    .pinned = 0,
    .open = n_open,
    .close = n_close,
    .draw = n_draw,
    .key = n_key,
    .click = n_click,
};
