/* Desktop preferences: the Dynamic Island, dock, widgets, sound and display settings.
 * They live in one struct; a change repaints the screen and is saved to AMDROID.CFG a
 * moment after the last change (so dragging a slider writes the disk once, not 60 times). */
#include "gui_internal.h"
#include "amdroid/fat32.h"
#include "amdroid/fb.h"
#include "amdroid/reel.h"
#include "amdroid/timer.h"

#define PREFS_MAGIC 0x41445002          /* "ADP" + layout version 2 */
#define PREFS_FILE  "AMDROID.CFG"
#define SAVE_DELAY  150                 /* 1.5 s after the last change */

ui_prefs_t ui_prefs;
static int loaded = 0, dirty = 0;
static u64 changed_at = 0;

static void defaults(void){
    ui_prefs = (ui_prefs_t){
        .magic = PREFS_MAGIC,
        .isl_idle = ISL_IDLE_NAME, .isl_size = 1, .isl_style = ISL_STYLE_BLACK, .isl_secs = 4,
        .isl_snap = 1, .isl_x = -1, .isl_y = -1, .dnd = 0,
        .dock_on = 1, .dock_edge = DOCK_RIGHT, .dock_size = 1, .dock_zoom = 2,
        .wid_on = 1, .wid_mask = (1 << WID_CLOCK) | (1 << WID_SYSTEM) | (1 << WID_CALENDAR),
        .volume = 80, .brightness = 100,
    };
    for(int i = 0; i < UI_NWIDGETS; i++) ui_prefs.wid_x[i] = ui_prefs.wid_y[i] = -1;
}

static int clampv(int v, int lo, int hi){ return v < lo ? lo : v > hi ? hi : v; }

/* Keeps every value inside what the screen code can handle (a damaged or older file). */
static void sanitize(void){
    ui_prefs_t *p = &ui_prefs;
    p->isl_idle  = clampv(p->isl_idle, 0, ISL_IDLE_N - 1);
    p->isl_size  = clampv(p->isl_size, 0, 2);
    p->isl_style = clampv(p->isl_style, 0, ISL_STYLE_N - 1);
    if(p->isl_secs != 2 && p->isl_secs != 4 && p->isl_secs != 8 && p->isl_secs != 12) p->isl_secs = 4;
    p->dock_edge = clampv(p->dock_edge, 0, DOCK_EDGE_N - 1);
    p->dock_size = clampv(p->dock_size, 0, 2);
    p->dock_zoom = clampv(p->dock_zoom, 0, 2);
    p->volume = clampv(p->volume, 0, 100);
    p->brightness = clampv(p->brightness, 20, 100);
    p->wid_mask &= (1 << UI_NWIDGETS) - 1;
}

static void apply(void){
    fb_set_brightness(ui_prefs.brightness);
    reel_set_volume(ui_prefs.volume);        /* 100 = as recorded; Reel scales by percent */
}

void prefs_load(void){
    if(loaded) return;
    loaded = 1;
    defaults();
    ui_prefs_t tmp;
    u32 size = 0;
    if(fat32_read_path(PREFS_FILE, &tmp, sizeof tmp, &size) == 0 && size == sizeof tmp && tmp.magic == PREFS_MAGIC){
        ui_prefs = tmp;
        sanitize();
    }
    apply();
}

void ui_prefs_changed(void){
    sanitize();
    apply();
    dirty = 1;
    changed_at = timer_ticks();
    gui_damage_all();
}

/* Called every GUI loop: writes the file once things have been quiet for a moment. */
void prefs_tick(u64 now){
    if(dirty && now - changed_at >= SAVE_DELAY){
        dirty = 0;
        fat32_write_path(PREFS_FILE, &ui_prefs, sizeof ui_prefs);   /* no disk: the settings last until reboot */
    }
}

/* Changes that move something only a little (dragging the island, a widget) save too, but
 * do not need the whole screen repainted: the mover reports its own damage. */
void prefs_touch(void){
    dirty = 1;
    changed_at = timer_ticks();
}

/* A slider moved: apply and save, without repainting the desktop. Only brightness changes
 * what is on the screen, and that needs the picture shown again, not drawn again. */
void ui_prefs_live(void){
    static int last_bright = -1;
    sanitize();
    apply();
    prefs_touch();
    if(ui_prefs.brightness != last_bright){ last_bright = ui_prefs.brightness; gui_present_all(); }
}
