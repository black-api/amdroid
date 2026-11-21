/* voicecmd.c — runs the calls Needle made for a voice command (include/amdroid/voice_tools.h
 * declares the tools). The list is grammar-made, so its shape is fixed:
 *   [{"name":"N","arguments":{"k":"v","k":12,"k":true}},...]
 * with plain strings; every value is still checked here before it touches anything. */
#include "amdroid/voicecmd.h"
#include "amdroid/ui.h"
#include "amdroid/keyboard.h"
#include "amdroid/timer.h"
#include "amdroid/rtc.h"
#include "gui_internal.h"

void browser_open_text(const char *s);
void notes_add_line(const char *s);

static int ncmp(const char *a, const char *b, int n){ for(int i = 0; i < n; i++){ if(a[i] != b[i]) return 1; if(!a[i]) return 0; } return 0; }
static void cat_n(char *dst, const char *src, u32 cap){ u32 l = ui_len(dst); while(*src && l + 1 < cap) dst[l++] = *src++; dst[l] = 0; }

#define MAXARG 4
typedef struct { char name[32]; int n; char key[MAXARG][24]; char val[MAXARG][208]; int num[MAXARG]; } call_t;

/* the next call in *p; 0 when there is none */
static int next_call(const char **pp, call_t *c){
    const char *p = *pp;
    while(*p && *p != '{') p++;
    if(!*p) return 0;
    p++;
    c->name[0] = 0; c->n = 0;
    int depth = 1;
    while(*p && depth > 0){
        if(*p == '}'){ depth--; p++; continue; }
        if(*p == '{'){ depth++; p++; continue; }
        if(*p != '"'){ p++; continue; }
        const char *k = ++p;
        while(*p && *p != '"') p++;
        int kl = (int)(p - k);
        if(!*p) break;
        p++;
        if(*p != ':') continue;
        p++;
        if(kl == 9 && !ncmp(k, "arguments", 9)) continue;
        char key[24]; int i = 0;
        for(; i < kl && i < 23; i++) key[i] = k[i];
        key[i] = 0;
        char val[208]; int vl = 0, num = 0, isnum = 0;
        if(*p == '"'){
            p++;
            while(*p && *p != '"'){ if(vl < 207) val[vl++] = *p; p++; }
            if(*p) p++;
        } else {
            int neg = 0;
            if(*p == '-'){ neg = 1; p++; }
            while(*p >= '0' && *p <= '9'){ num = num * 10 + (*p - '0'); if(vl < 207) val[vl++] = *p; p++; }
            while((*p >= 'a' && *p <= 'z') || *p == '.'){ if(vl < 207) val[vl++] = *p; p++; }
            if(neg) num = -num;
            isnum = 1;
        }
        val[vl] = 0;
        if(depth == 1 && !ui_streq(key, "name")) continue;
        if(ui_streq(key, "name") && depth == 1){ ui_copy(c->name, val, sizeof c->name); continue; }
        if(c->n < MAXARG){
            ui_copy(c->key[c->n], key, sizeof c->key[0]);
            ui_copy(c->val[c->n], val, sizeof c->val[0]);
            c->num[c->n] = isnum ? num : 0;
            c->n++;
        }
    }
    *pp = p;
    return c->name[0] != 0;
}

static const char *arg(const call_t *c, const char *key){
    for(int i = 0; i < c->n; i++) if(ui_streq(c->key[i], key)) return c->val[i];
    return 0;
}
static int argnum(const call_t *c, const char *key, int *out){
    for(int i = 0; i < c->n; i++) if(ui_streq(c->key[i], key)){ *out = c->num[i]; return 1; }
    return 0;
}

static char lc(char c){ return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int same_ci(const char *a, const char *b){
    while(*a && *b){ if(lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == *b;
}
/* the tool's app names -> the desktop's apps; -2 for the terminal, -1 unknown */
static int app_index(const char *name){
    if(!name) return -1;
    if(same_ci(name, "terminal")) return -2;
    if(same_ci(name, "video player")) name = "MVP";
    for(int a = 0; a < ui_app_count(); a++) if(same_ci(ui_apps[a]->name, name)) return a;
    return -1;
}

static void say(char *out, u32 cap, const char *a, const char *b){
    if(!out) return;
    if(out[0]) cat_n(out, ", ", cap);
    cat_n(out, a, cap);
    if(b) cat_n(out, b, cap);
}

static void num_str(int v, char *s){ ui_num((u32)(v < 0 ? 0 : v), s); }

/* what a call would do, in words ("Open Notes"); 0 when the call is not one of ours or is out of range */
static int describe(const call_t *c, char *out, u32 cap, int past){
    char n[16];
    if(ui_streq(c->name, "open_app")){
        int a = app_index(arg(c, "app"));
        if(a == -1) return 0;
        say(out, cap, past ? "Opened " : "Open ", a == -2 ? "Terminal" : ui_apps[a]->name);
        return 1;
    }
    if(ui_streq(c->name, "close_app")){
        const char *nm = arg(c, "app");
        if(nm){ int a = app_index(nm); if(a < 0) return 0; say(out, cap, past ? "Closed " : "Close ", ui_apps[a]->name); }
        else say(out, cap, past ? "Closed the window" : "Close the window", 0);
        return 1;
    }
    if(ui_streq(c->name, "search_web")){ const char *q = arg(c, "query"); if(!q || !q[0]) return 0; say(out, cap, past ? "Searched for " : "Search for ", q); return 1; }
    if(ui_streq(c->name, "open_website")){ const char *q = arg(c, "address"); if(!q || !q[0]) return 0; say(out, cap, past ? "Opened " : "Open ", q); return 1; }
    int v;
    if(ui_streq(c->name, "set_volume") || ui_streq(c->name, "set_brightness")){
        if(!argnum(c, "level", &v) || v < 0 || v > 100) return 0;
        num_str(v, n); ui_cat(n, "%");
        say(out, cap, c->name[4] == 'v' ? "Volume " : "Brightness ", n);
        return 1;
    }
    if(ui_streq(c->name, "do_not_disturb")){
        const char *a = arg(c, "action");
        if(!a || !(ui_streq(a, "on") || ui_streq(a, "off"))) return 0;
        say(out, cap, "Do Not Disturb ", a);
        return 1;
    }
    if(ui_streq(c->name, "add_note")){ const char *t = arg(c, "text"); if(!t || !t[0]) return 0; say(out, cap, past ? "Noted: " : "Note: ", t); return 1; }
    if(ui_streq(c->name, "show_date_and_time")){ say(out, cap, past ? "Showed the time" : "Show the time", 0); return 1; }
    return 0;
}

static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void run(const call_t *c, int *next_vt){
    u64 now = timer_ticks();
    int v;
    if(ui_streq(c->name, "open_app")){
        int a = app_index(arg(c, "app"));
        if(a == -2) *next_vt = 2;
        else if(a >= 0) win_open_app(a, now);
    } else if(ui_streq(c->name, "close_app")){
        const char *nm = arg(c, "app");
        int a = nm ? app_index(nm) : win_top_app();
        if(a >= 0 && win_is_open(a)) win_close_app(a);
    } else if(ui_streq(c->name, "search_web") || ui_streq(c->name, "open_website")){
        const char *q = arg(c, c->name[0] == 's' ? "query" : "address");
        int b = app_index("Browser");
        if(q && b >= 0){ win_open_app(b, now); browser_open_text(q); }
    } else if(ui_streq(c->name, "set_volume") && argnum(c, "level", &v)){
        ui_prefs.volume = v; ui_prefs_live();
    } else if(ui_streq(c->name, "set_brightness") && argnum(c, "level", &v)){
        ui_prefs.brightness = 20 + v * 80 / 100; ui_prefs_live();
    } else if(ui_streq(c->name, "do_not_disturb")){
        const char *a = arg(c, "action");
        ui_prefs.dnd = a && ui_streq(a, "on");
        ui_prefs_changed();
    } else if(ui_streq(c->name, "add_note")){
        const char *t = arg(c, "text");
        int nt = app_index("Notes");
        if(t){ notes_add_line(t); if(nt >= 0) win_open_app(nt, now); }
    } else if(ui_streq(c->name, "show_date_and_time")){
        rtc_time_t t; rtc_read(&t);
        char s[48] = "", n[8];
        ui_num(t.hour, n); if(t.hour < 10) ui_cat(s, "0"); ui_cat(s, n); ui_cat(s, ":");
        ui_num(t.min, n); if(t.min < 10) ui_cat(s, "0"); ui_cat(s, n);
        char d[48] = "";
        ui_num(t.day, n); ui_cat(d, n); ui_cat(d, " ");
        ui_cat(d, t.month >= 1 && t.month <= 12 ? months[t.month - 1] : "?"); ui_cat(d, " ");
        ui_num(t.year, n); ui_cat(d, n);
        int was = ui_prefs.dnd; ui_prefs.dnd = 0;           /* the answer is asked for: show it */
        ui_notify(-1, s, d);
        ui_prefs.dnd = was;
    }
}

int vc_describe(const char *calls, char *out, u32 cap, int past){
    out[0] = 0;
    const char *p = calls;
    call_t c;
    int n = 0;
    while(next_call(&p, &c)){ if(!describe(&c, out, cap, past)) return 0; n++; }
    return n;
}

int vc_execute(const char *calls, int *next_vt){
    char tmp[256];
    if(!vc_describe(calls, tmp, sizeof tmp, 0)) return 0;     /* all or nothing: every call must be valid */
    const char *p = calls;
    call_t c;
    int n = 0;
    while(next_call(&p, &c)){ run(&c, next_vt); n++; }
    return n;
}
