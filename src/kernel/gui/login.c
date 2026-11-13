/* The login screen: pick an account, type its password, go in. */
#include "amdroid/gui.h"
#include "amdroid/fb.h"
#include "amdroid/mouse.h"
#include "amdroid/keyboard.h"
#include "amdroid/timer.h"
#include "amdroid/auth.h"
#include "amdroid/rtc.h"
#include "gui_internal.h"

#define CARD_W      480
#define ROW_H_L     72
#define ROW_GAP     10
#define MAX_ACCOUNTS 12
#define PW_MAX      40
#define PW_PANEL    150

static rect_t card_rect(u32 sw, u32 sh, int n, int pw_mode){
    /* the card fits what it shows: the password area only when the password box is open */
    u32 h = 96 + (u32)n * (ROW_H_L + ROW_GAP) + (pw_mode ? PW_PANEL : 50);
    if(n == 0) h = 200;
    return (rect_t){ (sw - CARD_W) / 2, sh / 2 - h / 2 + 40, CARD_W, h };
}
static rect_t account_row(rect_t card, int i){
    return (rect_t){ card.x + 24, card.y + 96 + (u32)i * (ROW_H_L + ROW_GAP), card.w - 48, ROW_H_L };
}
static rect_t pw_box(rect_t card, int n){
    return (rect_t){ card.x + 24, card.y + 96 + (u32)n * (ROW_H_L + ROW_GAP) + 46, card.w - 48, 48 };
}

static void draw_screen(u32 sw, u32 sh, const char *names[], const int uids[], int n,
                        int sel, int hover, int pw_mode, int pw_len, const char *msg){
    wall_draw();

    rtc_time_t t;
    rtc_read(&t);
    char clk[8], date[16];
    clk[0] = (char)('0' + t.hour / 10); clk[1] = (char)('0' + t.hour % 10); clk[2] = ':';
    clk[3] = (char)('0' + t.min / 10);  clk[4] = (char)('0' + t.min % 10);  clk[5] = 0;
    date[0] = (char)('0' + t.year / 1000 % 10); date[1] = (char)('0' + t.year / 100 % 10);
    date[2] = (char)('0' + t.year / 10 % 10);   date[3] = (char)('0' + t.year % 10); date[4] = '-';
    date[5] = (char)('0' + t.month / 10); date[6] = (char)('0' + t.month % 10); date[7] = '-';
    date[8] = (char)('0' + t.day / 10);   date[9] = (char)('0' + t.day % 10); date[10] = 0;
    ui_text((sw - ui_text_w(clk, 4)) / 2, sh / 8, clk, C_TEXT, 4);
    ui_text((sw - ui_text_w(date, UI_SCALE)) / 2, sh / 8 + 84, date, C_MUTED, UI_SCALE);

    rect_t card = card_rect(sw, sh, n, pw_mode);
    ui_rrect(card.x + 6, card.y + 10, card.w, card.h, 20, C_SHADOW);
    ui_rrect(card.x, card.y, card.w, card.h, 20, C_WIN);
    const char *head = pw_mode ? "Enter the password" : "Choose an account";
    ui_text(card.x + (card.w - ui_text_w(head, UI_SCALE)) / 2, card.y + 26, head, C_TEXT, UI_SCALE);
    if(n == 0){
        ui_text(card.x + 24, card.y + 110, "No accounts yet. Add one in the text shell (adduser).", C_WARN, 1);
        return;
    }
    for(int i = 0; i < n; i++){
        rect_t r = account_row(card, i);
        int chosen = i == sel;
        int hot = i == hover;
        ui_rrect(r.x, r.y, r.w, r.h, 14, chosen ? C_CARD_HI : (hot ? C_HOVER : C_CARD));
        if(chosen || hot){
            /* a clear outline and an accent bar, so the row you point at stands out */
            ui_rrect(r.x, r.y, r.w, 2, 0, ui_accent());
            ui_rrect(r.x, r.y + r.h - 2, r.w, 2, 0, ui_accent());
            ui_rrect(r.x, r.y, 4, r.h, 0, ui_accent());
            ui_rrect(r.x + r.w - 4, r.y, 4, r.h, 0, ui_accent());
        }
        rect_t av = { r.x + 14, r.y + 8, 56, 56 };
        ui_rrect(av.x, av.y, av.w, av.h, 12, ui_accent());
        char letter[2] = { names[i][0], 0 };
        if(letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
        ui_text(av.x + (av.w - ui_text_w(letter, 3)) / 2, av.y + (av.h - FONT_H * 3) / 2, letter, C_BAR, 3);
        ui_text(r.x + 92, r.y + 10, names[i], C_TEXT, UI_SCALE);
        char sub[32] = "";
        if(uids[i] == 0) ui_cat(sub, "administrator");
        else { char u[12] = ""; ui_num((u32)uids[i], u); ui_cat(sub, "user, uid "); ui_cat(sub, u); }
        ui_text(r.x + 92, r.y + 42, sub, C_MUTED, 1);
    }

    if(pw_mode && sel >= 0 && sel < n){
        rect_t b = pw_box(card, n);
        ui_text(b.x, b.y - 24, "Password", C_MUTED, 1);
        ui_rrect(b.x, b.y, b.w, b.h, 10, C_CARD_HI);
        ui_rrect(b.x, b.y, b.w, 2, 0, ui_accent());
        char dots[PW_MAX + 2];
        for(int k = 0; k < pw_len && k < PW_MAX; k++) dots[k] = '*';
        dots[pw_len < PW_MAX ? pw_len : PW_MAX] = 0;
        ui_text(b.x + 14, b.y + 12, dots, C_TEXT, UI_SCALE);
        u32 cx = b.x + 14 + (u32)(pw_len < PW_MAX ? pw_len : PW_MAX) * UI_CW;
        ui_fill((rect_t){ cx, b.y + 10, 3, UI_CH - 4 }, ui_accent());
    }
    if(msg && msg[0]){
        ui_text(card.x + 24, card.y + card.h - 34, msg, C_WARN, 1);
    } else {
        ui_text(card.x + 24, card.y + card.h - 34,
                pw_mode ? "Enter to sign in   Esc to go back" : "Click an account, or use Up/Down and Enter",
                C_DIM, 1);
    }
}

/* Lists the accounts, then signs the chosen one in with its password. Returns 0 when signed in. */
int gui_login(void){
    if(!fb_ready()) return -1;
    u32 sw = fb_width(), sh = fb_height();
    wall_init(sw, sh);
    mouse_init();

    const char *names[MAX_ACCOUNTS];
    int uids[MAX_ACCOUNTS];
    int n = 0;
    for(int i = 0; i < auth_user_count() && n < MAX_ACCOUNTS; i++){
        int uid = -1;
        const char *nm = auth_user_at(i, &uid);
        if(!nm) break;
        names[n] = nm;
        uids[n] = uid;
        n++;
    }

    /* auto-login: skip the login screen when the only account has no password */
    if(n == 1 && auth_login(names[0], "") == 0){ mouse_disable(); return 0; }

    int sel = 0, hover = -1, mx = (int)sw / 2, my = (int)sh / 2;
    int redraw = 1;                         /* the screen is drawn when something on it changed */
    u8 shown_min = 255;
    int pw_mode = 0, pw_len = 0;
    char pw[PW_MAX + 1];
    char msg[64] = "";
    int result = -1;
    while(result != 0){
        mouse_event_t ev;
        if(mouse_poll(&ev)){
            redraw = 1;
            mx = mx + ev.dx < 0 ? 0 : (mx + ev.dx >= (int)sw ? (int)sw - 1 : mx + ev.dx);
            my = my + ev.dy < 0 ? 0 : (my + ev.dy >= (int)sh ? (int)sh - 1 : my + ev.dy);
            if(ev.pressed & 1){
                rect_t card = card_rect(sw, sh, n, pw_mode);
                for(int i = 0; i < n; i++)
                    if(ui_contains(account_row(card, i), mx, my)){
                        sel = i;
                        pw_mode = 1;
                        pw_len = 0; pw[0] = 0;
                        msg[0] = 0;
                    }
            }
        }
        rect_t card = card_rect(sw, sh, n, pw_mode);
        hover = -1;
        for(int i = 0; i < n; i++)
            if(ui_contains(account_row(card, i), mx, my)) hover = i;

        if(keyboard_has_input() && n > 0){
            int c = keyboard_getchar();
            redraw = 1;
            if(!pw_mode){
                if(c == KEY_UP && sel > 0) sel--;
                else if(c == KEY_DOWN && sel < n - 1) sel++;
                else if(c == '\n' || c == ' '){ pw_mode = 1; pw_len = 0; pw[0] = 0; msg[0] = 0; }
            } else {
                if(c == 27){ pw_mode = 0; pw_len = 0; pw[0] = 0; msg[0] = 0; }
                else if(c == '\b'){ if(pw_len) pw[--pw_len] = 0; }
                else if(c == '\n'){
                    if(auth_login(names[sel], pw) == 0) result = 0;
                    else {
                        ui_copy(msg, "Wrong password. Try again.", sizeof msg);
                        pw_len = 0; pw[0] = 0;
                    }
                }
                else if(c >= 32 && c <= 126 && pw_len < PW_MAX){ pw[pw_len++] = (char)c; pw[pw_len] = 0; }
            }
        }
        if(result == 0) break;

        rtc_time_t t;
        rtc_read(&t);
        if(t.min != shown_min){ shown_min = t.min; redraw = 1; }      /* the clock */
        if(redraw){
            draw_screen(sw, sh, names, uids, n, sel, hover, pw_mode, pw_len, msg);
            ui_cursor(mx, my);
            fb_swap();
            redraw = 0;
        }
        timer_sleep(10);
    }
    mouse_disable();
    return 0;
}
