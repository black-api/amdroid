/* Network: the address details of the network card, and a ping test to the gateway. */
#include "amdroid/ui.h"
#include "amdroid/net.h"

static char ping_result[48] = "";
static int ping_ok = 0;

static rect_t ping_btn(rect_t b){ return (rect_t){ b.x, b.y + 5 * 40 + 20, 260, 44 }; }

static void ping_gateway(void){
    u32 ip = 0, gw = 0; u8 mac[6];
    if(!net_up()){
        ui_copy(ping_result, "Network is down", sizeof ping_result);
        ping_ok = 0;
        return;
    }
    net_info(&ip, &gw, mac);
    u32 rtt = 0;
    if(net_ping(gw, 1000, &rtt) == 0){
        char n[12] = "", m[48] = "Reply in ";
        ui_num(rtt, n);
        ui_cat(m, n); ui_cat(m, " ms");
        ui_copy(ping_result, m, sizeof ping_result);
        ping_ok = 1;
    } else {
        ui_copy(ping_result, "No reply from the gateway", sizeof ping_result);
        ping_ok = 0;
    }
}

static void row(rect_t b, int i, const char *label, const char *value, u32 col){
    u32 y = b.y + (u32)i * 40;
    ui_text(b.x, y, label, C_MUTED, UI_SCALE);
    ui_text(b.x + 200, y, value, col, UI_SCALE);
}

static void n_draw(rect_t b, int focused){
    (void)focused;
    if(!net_up()){
        ui_text(b.x, b.y, "No network card found.", C_WARN, UI_SCALE);
        return;
    }
    u32 ip = 0, gw = 0; u8 mac[6];
    net_info(&ip, &gw, mac);
    u32 dns = net_dns();

    char a[24], g[24], d[24], m[24] = "";
    ui_ip(a, ip); ui_ip(g, gw); ui_ip(d, dns);
    const char *hex = "0123456789abcdef";
    for(int i = 0; i < 6; i++){
        int p = (int)ui_len(m);
        m[p] = hex[mac[i] >> 4];
        m[p + 1] = hex[mac[i] & 15];
        m[p + 2] = i < 5 ? ':' : 0;
        if(i == 5) m[p + 2] = 0;
    }

    row(b, 0, "Status", "Connected", C_GOOD);
    row(b, 1, "Address", a, C_TEXT);
    row(b, 2, "Gateway", g, C_TEXT);
    row(b, 3, "DNS", d, C_TEXT);
    row(b, 4, "MAC address", m, C_TEXT);

    rect_t pb = ping_btn(b);
    ui_rrect(pb.x, pb.y, pb.w, pb.h, 10, ui_accent());
    ui_text(pb.x + (pb.w - ui_text_w("Ping gateway", UI_SCALE)) / 2, pb.y + (pb.h - UI_CH) / 2, "Ping gateway", C_BAR, UI_SCALE);
    if(ping_result[0])
        ui_text(pb.x + pb.w + 20, pb.y + (pb.h - UI_CH) / 2, ping_result, ping_ok ? C_GOOD : C_WARN, UI_SCALE);
}

static void n_click(rect_t b, int x, int y){
    if(ui_contains(ping_btn(b), x, y)) ping_gateway();
}

const app_t app_network = {
    .name = "Network",
    .icon = UI_ICO_NET,
    .open = NULL,
    .close = NULL,
    .draw = n_draw,
    .key = NULL,
    .click = n_click,
};
