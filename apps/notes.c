/* Notes: a note in Markdown, saved to NOTES.MD on the disk (an older NOTES.TXT is picked up
 * the first time). Edit it as text; Preview (or Tab) shows it formatted: headings, bold,
 * lists, checkboxes, quotes, code and tables, drawn by the shared Markdown engine. Save with
 * the button, or close the window: unsaved changes are written then. */
#include "amdroid/ui.h"
#include "amdroid/fat32.h"
#include "amdroid/keyboard.h"
#include "amdroid/markdown.h"
#include "amdroid/dictation.h"

#define NOTE_FILE  "NOTES.MD"
#define OLD_FILE   "NOTES.TXT"
#define NOTE_MAX   32000
#define LINES_MAX  2048
#define LINE_MAX   160             /* characters across, in the widest window */
#define TOP        64              /* the text starts below the buttons */

static char text[NOTE_MAX + 1];
static u32 len = 0;
static int loaded = 0;
static int dirty = 0;
static int preview = 0;
static int pscroll = 0;           /* preview: pixels scrolled */
static int pheight = 0;           /* preview: the height the note took last time */
static char status[64] = "";
static rect_t last_body;

/* wrapped lines: start offset and length in text */
static u32 ls_start[LINES_MAX], ls_len[LINES_MAX];
static int nlines = 0;

static void load(void){
    u32 size = 0;
    len = 0;
    if(fat32_read_path(NOTE_FILE, text, NOTE_MAX, &size) == 0){
        len = size < NOTE_MAX ? size : NOTE_MAX;
        ui_copy(status, "Loaded NOTES.MD", sizeof status);
    } else if(fat32_read_path(OLD_FILE, text, NOTE_MAX, &size) == 0){
        len = size < NOTE_MAX ? size : NOTE_MAX;
        ui_copy(status, "Loaded NOTES.TXT (saves as NOTES.MD)", sizeof status);
    } else {
        ui_copy(status, "New note - Markdown works: # heading, **bold**, - list", sizeof status);
    }
    text[len] = 0;
    dirty = 0;
    loaded = 1;
}

static void save(void){
    if(fat32_write_path(NOTE_FILE, text, len) == 0){
        dirty = 0;
        ui_copy(status, "Saved NOTES.MD", sizeof status);
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
static rect_t view_btn(rect_t b){ rect_t s = ui_button(b, 140); s.x -= 180; s.w = 172; return s; }
static rect_t mic_btn(rect_t b){ rect_t s = view_btn(b); s.x -= 220; s.w = 212; return s; }

/* ---- dictation: the system service types into the focused window, which is us ---- */
/* ---- the Markdown engine draws through these ---- */
static void md_text(void *ctx, int x, int y, const char *s, u32 color, int scale, int bold){
    (void)ctx;
    if(x < 0 || y < 0) return;
    ui_text((u32)x, (u32)y, s, color, (u32)scale);
    if(bold) ui_text((u32)x + 1, (u32)y, s, color, (u32)scale);
}
static void md_rect(void *ctx, int x, int y, int w, int h, u32 color){
    (void)ctx;
    if(x < 0 || y < 0 || w <= 0 || h <= 0) return;
    ui_fill((rect_t){ (u32)x, (u32)y, (u32)w, (u32)h }, color);
}

/* a voice command's note: a new list item at the end, saved at once */
void notes_add_line(const char *s){
    if(!loaded) load();
    if(len && text[len - 1] != '\n' && len < NOTE_MAX) text[len++] = '\n';
    const char *pre = "- ";
    for(const char *p = pre; *p && len < NOTE_MAX; p++) text[len++] = *p;
    for(const char *p = s; *p && len < NOTE_MAX; p++) text[len++] = *p;
    text[len] = 0;
    dirty = 1;
    save();
}

static void n_open(void){
    if(!loaded) load();
}
static void n_close(void){
    if(dirty) save();
}

static void draw_preview(rect_t b){
    int top = (int)b.y + TOP, bottom = (int)(b.y + b.h) - 28;
    md_draw_t d = {
        .ctx = 0, .text = md_text, .rect = md_rect, .scale = UI_SCALE,
        .fg = C_TEXT, .dim = C_MUTED, .accent = ui_accent(), .code_fg = 0xE6C07B,
        .code_bg = 0x161D22, .rule = C_SEP, .quote = ui_accent(),
        .clip_top = top, .clip_bottom = bottom,
    };
    int maxs = pheight - (bottom - top);
    if(pscroll > maxs) pscroll = maxs;
    if(pscroll < 0) pscroll = 0;
    pheight = md_draw(text, len, (int)b.x + 12, top - pscroll, (int)b.w - 24, &d);
    if(len == 0) ui_text(b.x + 12, (u32)top, "(empty note - press Tab to write)", C_DIM, UI_SCALE);
}

static void n_draw(rect_t b, int focused){
    (void)focused;
    last_body = b;
    if(!loaded) load();

    rect_t sb = save_btn(b);
    ui_fill(sb, dirty ? ui_accent() : C_CARD_HI);
    ui_text(sb.x + 30, sb.y + 4, dirty ? "Save*" : "Save", dirty ? C_BAR : C_TEXT, UI_SCALE);
    rect_t vb = view_btn(b);
    ui_fill(vb, preview ? ui_accent() : C_CARD_HI);
    ui_text(vb.x + 14, vb.y + 4, preview ? "Edit" : "Preview", preview ? C_BAR : C_TEXT, UI_SCALE);
    rect_t mb = mic_btn(b);
    int ds = dict_state();
    ui_fill(mb, ds == DICT_LISTEN ? 0xE5484D : ds == DICT_WRITE ? C_CARD : C_CARD_HI);
    ui_text(mb.x + 14, mb.y + 4, ds == DICT_LISTEN ? "Stop" : ds == DICT_WRITE ? "Writing..." : "Dictate",
            ds == DICT_LISTEN ? 0xFFFFFF : ds == DICT_WRITE ? C_DIM : C_TEXT, UI_SCALE);

    if(preview){
        draw_preview(b);
        ui_text(b.x, b.y + b.h - 24, "Preview - Tab to edit, arrows / Page Up / Page Down to scroll", C_DIM, 1);
        return;
    }

    u32 cols = (b.w - 16) / UI_CW;
    if(cols > LINE_MAX) cols = LINE_MAX;
    wrap((int)cols);
    int rows = (int)(b.h - TOP - 28) / UI_CH;
    if(rows < 1) rows = 1;
    int first = nlines > rows ? nlines - rows : 0;      /* keep the end in view */
    for(int i = first; i < nlines; i++){
        char line[LINE_MAX + 1];
        u32 n = ls_len[i];
        if(n > LINE_MAX) n = LINE_MAX;
        for(u32 k = 0; k < n; k++) line[k] = text[ls_start[i] + k];
        line[n] = 0;
        /* Markdown marks a little dimmer, so the text reads first */
        u32 col = C_TEXT;
        if(n && (line[0] == '#' || line[0] == '>')) col = ui_accent();
        ui_text(b.x + 8, b.y + TOP + (u32)(i - first) * UI_CH, line, col, UI_SCALE);
    }
    /* the caret at the end of the text */
    int last = nlines - 1 - first;
    if(last >= 0 && last < rows){
        u32 x = b.x + 8 + ls_len[nlines - 1] * UI_CW;
        ui_fill((rect_t){ x, b.y + TOP + (u32)last * UI_CH + 6, 3, UI_CH - 10 }, ui_accent());
    }
    ui_text(b.x, b.y + b.h - 24, status, C_DIM, 1);
}

static void n_click(rect_t b, int x, int y){
    if(ui_contains(mic_btn(b), x, y)){
        if(dict_state() != DICT_WRITE){ preview = 0; dict_toggle(); }
    }
    else if(ui_contains(save_btn(b), x, y)) save();
    else if(ui_contains(view_btn(b), x, y)){ preview = !preview; pscroll = 0; }
}

static int n_key(int c){
    if(c == '\t'){ preview = !preview; pscroll = 0; return 1; }
    if(preview){
        int page = (int)last_body.h - TOP - 60;
        if(c == KEY_UP) pscroll -= 48;
        else if(c == KEY_DOWN) pscroll += 48;
        else if(c == KEY_PGUP) pscroll -= page;
        else if(c == KEY_PGDN) pscroll += page;
        else if(c == 27){ preview = 0; }
        else if(c == '\n' || (c >= 32 && c <= 126)){ preview = 0; }    /* typing goes back to editing */
        else return 1;
        if(preview) return 1;
    }
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
    .open = n_open,
    .close = n_close,
    .draw = n_draw,
    .key = n_key,
    .click = n_click,
};
