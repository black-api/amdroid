/* About: what this system is and how long it has been running. */
#include "amdroid/ui.h"
#include "amdroid/timer.h"

static void a_draw(rect_t b, int focused){
    (void)focused;
    ui_text(b.x, b.y, "am-droid", ui_accent(), 4);
    ui_text(b.x, b.y + 90, "A small 32-bit kernel with its own GUI", C_TEXT, UI_SCALE);
    const char *feats[] = {
        "Fuss interpreter built in",
        "FAT32 read and write, ext2 read-only, USB storage",
        "TCP/IP over e1000 and virtio-net",
        "Intel HDA audio, PS/2 and USB input",
        "Processes, signals, per-user permissions",
    };
    for(int i = 0; i < 5; i++){
        ui_fill((rect_t){ b.x, b.y + 160 + i * 40 + 12, 8, 8 }, ui_accent());
        ui_text(b.x + 24, b.y + 152 + i * 40, feats[i], C_MUTED, UI_SCALE);
    }
    char up[12] = "", line[64] = "";
    ui_uptime(up, timer_ticks());
    ui_cat(line, "Uptime "); ui_cat(line, up);
    ui_text(b.x, b.y + b.h - 24, line, C_DIM, 1);
}

const app_t app_about = {
    .name = "About",
    .icon = UI_ICO_ABOUT,
    .pinned = 0,
    .open = NULL,
    .close = NULL,
    .draw = a_draw,
    .key = NULL,
    .click = NULL,
};
