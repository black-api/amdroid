/* Settings: accent colour, wallpaper, animations and the network address. */
#include "amdroid/ui.h"
#include "amdroid/net.h"
#include "amdroid/keyboard.h"

static int ssel = 0;                 /* keyboard selection among the accent colours */

/* Layout: four sections, each a small heading, then its controls, with a line between. */
#define SEC_ACCENT     0
#define SEC_WALLPAPER  136
#define SEC_ANIM       296
#define SEC_NETWORK    392

static rect_t swatch(rect_t b, int i){ return (rect_t){ b.x + (u32)i * 92, b.y + SEC_ACCENT + 30, 64, 64 }; }
static rect_t thumb(rect_t b, int i){  return (rect_t){ b.x + (u32)i * 172, b.y + SEC_WALLPAPER + 30, 148, 76 }; }
static rect_t toggle(rect_t b){        return (rect_t){ b.x, b.y + SEC_ANIM + 30, 240, 40 }; }

static void heading(rect_t b, u32 y, const char *t, int line){
    if(line) ui_fill((rect_t){ b.x, b.y + y - 14, b.w, 1 }, C_SEP);
    ui_text(b.x, b.y + y, t, C_MUTED, 1);
}

static void s_draw(rect_t b, int focused){
    (void)focused;
    heading(b, SEC_ACCENT, "ACCENT COLOR", 0);
    for(int i = 0; i < UI_NACCENT; i++){
        rect_t s = swatch(b, i);
        if(ui_accent_index() == i) ui_rrect(s.x - 5, s.y - 5, s.w + 10, s.h + 10, 14, C_TEXT);
        else if(ssel == i) ui_rrect(s.x - 5, s.y - 5, s.w + 10, s.h + 10, 14, C_BAR_EDGE);
        ui_rrect(s.x, s.y, s.w, s.h, 10, ui_accent_color_of(i));
        u32 nw = ui_text_w(ui_accent_name(i), 1);
        ui_text(s.x + (s.w - nw) / 2, s.y + s.h + 10, ui_accent_name(i), ui_accent_index() == i ? C_TEXT : C_MUTED, 1);
    }

    heading(b, SEC_WALLPAPER, "WALLPAPER", 1);
    for(int i = 0; i < ui_wall_count(); i++){
        rect_t t = thumb(b, i);
        if(ui_wall_index() == i) ui_rrect(t.x - 5, t.y - 5, t.w + 10, t.h + 10, 12, C_TEXT);
        ui_rrect(t.x, t.y, t.w, t.h, 8, ui_wall_swatch(i));
        /* a hint of the light swoosh, so the preview looks like the wallpaper */
        ui_rrect(t.x + t.w / 2, t.y + t.h - 26, t.w / 2 - 6, 4, 2, 0x8A96A0);
        ui_rrect(t.x + t.w / 2 + 10, t.y + t.h - 16, t.w / 2 - 16, 3, 1, 0x6A7680);
        u32 nw = ui_text_w(ui_wall_name(i), 1);
        ui_text(t.x + (t.w - nw) / 2, t.y + t.h + 10, ui_wall_name(i), ui_wall_index() == i ? C_TEXT : C_MUTED, 1);
    }

    heading(b, SEC_ANIM, "ANIMATIONS", 1);
    rect_t t = toggle(b);
    /* a switch: a pill with a knob that sits right when on */
    u32 sw_w = 64, sw_h = 32;
    ui_rrect(t.x, t.y + 4, sw_w, sw_h, sw_h / 2, ui_anim_on ? ui_accent() : C_CARD_HI);
    u32 knob_x = ui_anim_on ? t.x + sw_w - 28 : t.x + 4;
    ui_rrect(knob_x, t.y + 8, 24, 24, 12, C_TEXT);
    ui_text(t.x + sw_w + 16, t.y + 4, ui_anim_on ? "On" : "Off", ui_anim_on ? C_TEXT : C_MUTED, UI_SCALE);

    heading(b, SEC_NETWORK, "NETWORK", 1);
    if(net_up()){
        u32 ip = 0, gw = 0; u8 mac[6];
        net_info(&ip, &gw, mac);
        char s[24];
        ui_ip(s, ip);
        ui_text(b.x, b.y + SEC_NETWORK + 28, s, C_GOOD, UI_SCALE);
    } else {
        ui_text(b.x, b.y + SEC_NETWORK + 28, "not connected", C_WARN, UI_SCALE);
    }
}

static void s_click(rect_t b, int x, int y){
    for(int i = 0; i < UI_NACCENT; i++)
        if(ui_contains(swatch(b, i), x, y)){ ui_set_accent(i); ssel = i; return; }
    for(int i = 0; i < ui_wall_count(); i++)
        if(ui_contains(thumb(b, i), x, y)){ ui_wall_set(i); return; }
    if(ui_contains(toggle(b), x, y)){ ui_anim_on = !ui_anim_on; return; }
}

static int s_key(int c){
    if(c == KEY_LEFT)       { ssel = ssel > 0 ? ssel - 1 : 0; return 1; }
    if(c == KEY_RIGHT)      { ssel = ssel < UI_NACCENT - 1 ? ssel + 1 : UI_NACCENT - 1; return 1; }
    if(c == '\n' || c == ' '){ ui_set_accent(ssel); return 1; }
    if(c == 'a' || c == 'A'){ ui_anim_on = !ui_anim_on; return 1; }
    if(c == 'w' || c == 'W'){ ui_wall_set((ui_wall_index() + 1) % ui_wall_count()); return 1; }
    return 0;
}

const app_t app_settings = {
    .name = "Settings",
    .icon = UI_ICO_SETTINGS,
    .pinned = 1,
    .open = NULL,
    .close = NULL,
    .draw = s_draw,
    .key = s_key,
    .click = s_click,
};
