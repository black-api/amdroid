/* Calculator: whole-number arithmetic with + - * / and parentheses. */
#include "amdroid/ui.h"
#include "amdroid/keyboard.h"

#define EXPR_MAX 48

static char expr[EXPR_MAX + 1];
static char shown[24];                       /* the result or an error, under the expression */
static int shown_is_error = 0;

/* ---- evaluation: shunting-yard over integers ---- */
static int prec(char op){ return (op == '+' || op == '-') ? 1 : 2; }

static int apply(long long *v, int *vn, char op, int *err){
    if(*vn < 2){ *err = 1; return 0; }
    long long b = v[--*vn], a = v[--*vn];
    long long r = 0;
    if(op == '+') r = a + b;
    else if(op == '-') r = a - b;
    else if(op == '*') r = a * b;
    else {
        if(b == 0){ *err = 2; return 0; }
        r = a / b;
    }
    v[(*vn)++] = r;
    return 1;
}

static long long eval(const char *s, int *err){
    long long vals[24];
    char ops[24];
    int vn = 0, on = 0;
    *err = 0;
    for(const char *p = s; *p; ){
        if(*p == ' '){ p++; continue; }
        if(*p >= '0' && *p <= '9'){
            long long n = 0;
            while(*p >= '0' && *p <= '9'){ n = n * 10 + (*p - '0'); p++; }
            if(vn >= 24){ *err = 1; return 0; }
            vals[vn++] = n;
            continue;
        }
        if(*p == '('){ if(on >= 24) { *err = 1; return 0; } ops[on++] = '('; p++; continue; }
        if(*p == ')'){
            while(on > 0 && ops[on - 1] != '(') { if(!apply(vals, &vn, ops[--on], err)) return 0; }
            if(on == 0){ *err = 1; return 0; }
            on--;
            p++;
            continue;
        }
        if(*p == '+' || *p == '-' || *p == '*' || *p == '/'){
            while(on > 0 && ops[on - 1] != '(' && prec(ops[on - 1]) >= prec(*p)){
                if(!apply(vals, &vn, ops[--on], err)) return 0;
            }
            if(on >= 24){ *err = 1; return 0; }
            ops[on++] = *p;
            p++;
            continue;
        }
        *err = 1;
        return 0;
    }
    while(on > 0){
        if(ops[on - 1] == '('){ *err = 1; return 0; }
        if(!apply(vals, &vn, ops[--on], err)) return 0;
    }
    if(vn != 1){ *err = 1; return 0; }
    return vals[0];
}

static void run(void){
    int err = 0;
    if(expr[0] == 0){ shown[0] = 0; shown_is_error = 0; return; }
    long long r = eval(expr, &err);
    shown_is_error = err != 0;
    if(err == 2){ ui_copy(shown, "Cannot divide by zero", sizeof shown); return; }
    if(err){ ui_copy(shown, "Error", sizeof shown); return; }
    int neg = r < 0;
    unsigned long long u = (unsigned long long)(neg ? -r : r);
    char digits[24]; int n = 0;
    if(u == 0) digits[n++] = '0';
    while(u){ digits[n++] = (char)('0' + u % 10); u /= 10; }
    int p = 0;
    if(neg) shown[p++] = '-';
    for(int i = n - 1; i >= 0; i--) shown[p++] = digits[i];
    shown[p] = 0;
}

static void press(char c){
    if(c == 'C'){ expr[0] = 0; shown[0] = 0; shown_is_error = 0; return; }
    if(c == '<'){ int n = (int)ui_len(expr); if(n > 0) expr[n - 1] = 0; return; }
    if(c == '='){ run(); return; }
    int n = (int)ui_len(expr);
    if(n >= EXPR_MAX) return;
    if(shown[0] && !shown_is_error && c >= '0' && c <= '9'){ expr[0] = 0; shown[0] = 0; n = 0; }
    expr[n] = c;
    expr[n + 1] = 0;
}

/* ---- layout: an expression line, a result, and a 4 x 5 keypad ---- */
static const char *labels[5][4] = {
    { "C", "(", ")", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", "00", "<", "=" },
};
static const char keys[5][4] = {
    { 'C', '(', ')', '/' },
    { '7', '8', '9', '*' },
    { '4', '5', '6', '-' },
    { '1', '2', '3', '+' },
    { '0', 'Z', '<', '=' },   /* Z is the "00" key */
};

static rect_t key_rect(rect_t b, int r, int c){
    u32 gap = 8;
    u32 w = (b.w - 3 * gap) / 4;
    u32 h = 52;
    u32 top = b.y + 110;
    return (rect_t){ b.x + (u32)c * (w + gap), top + (u32)r * (h + gap), w, h };
}

static void c_draw(rect_t b, int focused){
    (void)focused;
    ui_fill((rect_t){ b.x, b.y, b.w, 96 }, C_CARD);
    ui_text(b.x + 16, b.y + 12, expr[0] ? expr : " ", C_MUTED, UI_SCALE);
    if(shown[0]){
        u32 col = shown_is_error ? C_WARN : C_TEXT;
        ui_text_right((rect_t){ b.x, b.y, b.w - 16, 96 }, b.y + 52, shown, col, UI_SCALE);
    }
    for(int r = 0; r < 5; r++)
        for(int c = 0; c < 4; c++){
            rect_t k = key_rect(b, r, c);
            int eq = labels[r][c][0] == '=';
            int op = labels[r][c][0] == '+' || labels[r][c][0] == '-' ||
                     labels[r][c][0] == '*' || labels[r][c][0] == '/' || eq;
            ui_rrect(k.x, k.y, k.w, k.h, 10, eq ? ui_accent() : (op ? C_CARD_HI : C_CARD));
            u32 w = ui_text_w(labels[r][c], UI_SCALE);
            ui_text(k.x + (k.w - w) / 2, k.y + (k.h - UI_CH) / 2, labels[r][c],
                    eq ? C_BAR : C_TEXT, UI_SCALE);
        }
}

static void c_click(rect_t b, int x, int y){
    for(int r = 0; r < 5; r++)
        for(int c = 0; c < 4; c++)
            if(ui_contains(key_rect(b, r, c), x, y)){
                if(keys[r][c] == 'Z'){ press('0'); press('0'); }
                else press(keys[r][c]);
                return;
            }
}

static int c_key(int c){
    if(c >= '0' && c <= '9'){ press((char)c); return 1; }
    if(c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')'){ press((char)c); return 1; }
    if(c == '\n' || c == '='){ press('='); return 1; }
    if(c == '\b'){ press('<'); return 1; }
    if(c == 'c' || c == 'C'){ press('C'); return 1; }
    return 0;
}

const app_t app_calc = {
    .name = "Calculator",
    .icon = UI_ICO_CALC,
    .pinned = 0,
    .open = NULL,
    .close = NULL,
    .draw = c_draw,
    .key = c_key,
    .click = c_click,
};
