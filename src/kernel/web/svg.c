/* SVG: a small renderer for the pictures sites use as logos and icons. Shapes (path, rect,
 * circle, ellipse, line, polyline, polygon), groups with transforms, solid fills and strokes
 * (gradients are drawn as their average colour), opacity, fill-rule, <style> rules by class or
 * tag, and style="" attributes. No text, filters, masks, clip paths or <use>.
 * Output matches the other decoders: 0xTTRRGGBB, the top byte being transparency (0 opaque). */
#include "amdroid/web.h"
#include "amdroid/heap.h"
#include "amdroid/kmath.h"
#include "web_internal.h"
#include "string.h"

#define SVG_MAX_SIDE 1024
#define SVG_MAX_PTS  200000
#define SVG_RULES    32
#define SVG_GRADS    24

typedef struct { double x, y; } pt_t;

/* ---- text helpers ---- */
static int is_ws(int c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_alpha(int c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int lc(int c){ return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int tok_eq(const char *a, u32 al, const char *lit){
    u32 i = 0;
    for(; i < al && lit[i]; i++) if(lc((unsigned char)a[i]) != lit[i]) return 0;
    return i == al && !lit[i];
}

/* a number at *p (advances); 0 if none */
static int num(const char **p, const char *end, double *out){
    const char *s = *p;
    while(s < end && (is_ws(*s) || *s == ',')) s++;
    const char *b = s;
    double sign = 1, v = 0;
    if(s < end && (*s == '-' || *s == '+')){ if(*s == '-') sign = -1; s++; }
    int digits = 0;
    while(s < end && *s >= '0' && *s <= '9'){ v = v * 10 + (*s - '0'); s++; digits++; }
    if(s < end && *s == '.'){
        s++;
        double f = 0.1;
        while(s < end && *s >= '0' && *s <= '9'){ v += (*s - '0') * f; f *= 0.1; s++; digits++; }
    }
    if(!digits){ (void)b; return 0; }
    if(s < end && (*s == 'e' || *s == 'E') && s + 1 < end && ((s[1] >= '0' && s[1] <= '9') || ((s[1] == '-' || s[1] == '+') && s + 2 < end && s[2] >= '0' && s[2] <= '9'))){
        s++;
        int es = 1, ev = 0;
        if(*s == '-'){ es = -1; s++; } else if(*s == '+') s++;
        while(s < end && *s >= '0' && *s <= '9'){ ev = ev * 10 + (*s - '0'); s++; }
        double m = 1;
        for(int i = 0; i < ev && i < 40; i++) m *= 10;
        v = es > 0 ? v * m : v / m;
    }
    *out = sign * v;
    *p = s;
    return 1;
}

/* a length like "24", "24px", "2em", "50%" -> px; pct set when it was a percentage */
static int length(const char *s, u32 n, double *out, int *pct){
    const char *p = s, *e = s + n;
    double v;
    if(pct) *pct = 0;
    if(!num(&p, e, &v)) return 0;
    u32 rest = (u32)(e - p);
    if(rest >= 1 && p[0] == '%'){ if(pct) *pct = 1; *out = v; return 1; }
    if(rest >= 2 && tok_eq(p, 2, "pt")) v = v * 4 / 3;
    else if(rest >= 2 && tok_eq(p, 2, "em")) v *= 16;
    else if(rest >= 2 && tok_eq(p, 2, "in")) v *= 96;
    else if(rest >= 2 && tok_eq(p, 2, "cm")) v *= 96 / 2.54;
    else if(rest >= 2 && tok_eq(p, 2, "mm")) v *= 96 / 25.4;
    *out = v;
    return 1;
}

/* ---- attributes of the current tag ---- */
#define MAXATTR 40
typedef struct { const char *n; u32 nl; const char *v; u32 vl; } attr_t;
typedef struct { attr_t a[MAXATTR]; int n; } attrs_t;

static const attr_t *attr(const attrs_t *as, const char *name){
    for(int i = 0; i < as->n; i++) if(tok_eq(as->a[i].n, as->a[i].nl, name)) return &as->a[i];
    return NULL;
}
static double attr_num(const attrs_t *as, const char *name, double def){
    const attr_t *a = attr(as, name);
    if(!a) return def;
    double v; int pct;
    return length(a->v, a->vl, &v, &pct) && !pct ? v : def;
}

/* ---- colours ---- */
/* The page engine (css.c) has the full colour parser and overrides this one; the kernel, which
 * links the image decoders without the page engine, gets hex, rgb() and a few names. */
__attribute__((weak)) int css_parse_color(const char *s, u32 n, u32 *rgb){
    if(n && s[0] == '#'){
        u32 d[8], k = 0;
        for(u32 i = 1; i < n && k < 8; i++){
            int c = s[i], h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if(h < 0) return 0;
            d[k++] = (u32)h;
        }
        if(k == 3){ *rgb = (d[0] * 17) << 16 | (d[1] * 17) << 8 | d[2] * 17; return 1; }
        if(k >= 6){ *rgb = d[0] << 20 | d[1] << 16 | d[2] << 12 | d[3] << 8 | d[4] << 4 | d[5]; return 1; }
        return 0;
    }
    if(n > 4 && s[0] == 'r' && s[1] == 'g' && s[2] == 'b'){
        const char *p = s, *e = s + n;
        while(p < e && *p != '(') p++;
        p++;
        double v[3];
        for(int i = 0; i < 3; i++){ if(!num(&p, e, &v[i])) return 0; }
        *rgb = ((u32)v[0] & 255) << 16 | ((u32)v[1] & 255) << 8 | ((u32)v[2] & 255);
        return 1;
    }
    if(tok_eq(s, n, "black")){ *rgb = 0; return 1; }
    if(tok_eq(s, n, "white")){ *rgb = 0xFFFFFF; return 1; }
    if(tok_eq(s, n, "red")){ *rgb = 0xFF0000; return 1; }
    if(tok_eq(s, n, "green")){ *rgb = 0x008000; return 1; }
    if(tok_eq(s, n, "blue")){ *rgb = 0x0000FF; return 1; }
    if(tok_eq(s, n, "gray") || tok_eq(s, n, "grey")){ *rgb = 0x808080; return 1; }
    return 0;
}

/* 0 invalid, 1 colour (*rgb), 2 none, 3 currentColor, 4 a url(#id) reference (id in *idp, idl) */
static int paint_of(const char *v, u32 n, u32 *rgb, const char **idp, u32 *idl){
    while(n && is_ws(*v)){ v++; n--; }
    while(n && is_ws(v[n - 1])) n--;
    if(!n) return 0;
    if(tok_eq(v, n, "none") || tok_eq(v, n, "transparent")) return 2;
    if(tok_eq(v, n, "currentcolor")) return 3;
    if(n > 4 && tok_eq(v, 4, "url(")){
        const char *s = v + 4, *e = v + n;
        while(s < e && (*s == ' ' || *s == '"' || *s == '\'')) s++;
        if(s < e && *s == '#') s++;
        const char *b = s;
        while(s < e && *s != ')' && *s != '"' && *s != '\'' && *s != ' ') s++;
        *idp = b; *idl = (u32)(s - b);
        return 4;
    }
    char t[48]; u32 k = 0;
    for(u32 i = 0; i < n && k + 1 < sizeof t; i++) t[k++] = (char)lc((unsigned char)v[i]);
    t[k] = 0;
    return css_parse_color(t, k, rgb);          /* 1 colour, 2 transparent, 3 current; 0 unknown */
}

/* ---- gradients: drawn as their average colour ---- */
typedef struct { char id[40]; u32 c0, c1; int n; } grad_t;
static grad_t grads[SVG_GRADS];
static int ngrads;
static u32 avg_rgb(u32 a, u32 b){
    return ((((a >> 16 & 255) + (b >> 16 & 255)) / 2) << 16) | ((((a >> 8 & 255) + (b >> 8 & 255)) / 2) << 8) | (((a & 255) + (b & 255)) / 2);
}
static int grad_find(const char *id, u32 il, u32 *rgb){
    for(int i = 0; i < ngrads; i++){
        if(strlen(grads[i].id) == il && !memcmp(grads[i].id, id, il)){
            if(!grads[i].n) return 0;
            *rgb = avg_rgb(grads[i].c0, grads[i].c1);
            return 1;
        }
    }
    return 0;
}

/* ---- style rules from <style> ---- */
typedef struct { char sel[28]; int cls; char decl[200]; } rule_t;
static rule_t rules[SVG_RULES];
static int nrules;

/* ---- matrix ---- */
typedef struct { double a, b, c, d, e, f; } mat_t;
static mat_t mat_mul(mat_t m, mat_t n){      /* m applied after n: x' = m(n(x)) */
    mat_t r;
    r.a = m.a * n.a + m.c * n.b;  r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;  r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;  r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}
static pt_t mat_pt(const mat_t *m, double x, double y){ pt_t p = { m->a * x + m->c * y + m->e, m->b * x + m->d * y + m->f }; return p; }

static mat_t parse_transform(const char *s, u32 n, mat_t base){
    const char *e = s + n;
    mat_t cur = base;
    while(s < e){
        while(s < e && (is_ws(*s) || *s == ',')) s++;
        const char *nb = s;
        while(s < e && is_alpha(*s)) s++;
        u32 nl = (u32)(s - nb);
        if(!nl) break;
        while(s < e && *s != '(') s++;
        if(s >= e) break;
        s++;
        double v[6]; int cnt = 0;
        while(cnt < 6 && num(&s, e, &v[cnt])) cnt++;
        while(s < e && *s != ')') s++;
        if(s < e) s++;
        mat_t t = { 1, 0, 0, 1, 0, 0 };
        if(tok_eq(nb, nl, "matrix") && cnt == 6){ t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5]; }
        else if(tok_eq(nb, nl, "translate")){ t.e = cnt > 0 ? v[0] : 0; t.f = cnt > 1 ? v[1] : 0; }
        else if(tok_eq(nb, nl, "scale")){ t.a = cnt > 0 ? v[0] : 1; t.d = cnt > 1 ? v[1] : t.a; }
        else if(tok_eq(nb, nl, "rotate") && cnt > 0){
            double r = v[0] * 3.14159265358979 / 180, cs = kcos(r), sn = ksin(r);
            t.a = cs; t.b = sn; t.c = -sn; t.d = cs;
            if(cnt >= 3){ t.e = v[1] - cs * v[1] + sn * v[2]; t.f = v[2] - sn * v[1] - cs * v[2]; }
        }
        else if(tok_eq(nb, nl, "skewx") && cnt > 0){ double r = v[0] * 3.14159265358979 / 180; t.c = ksin(r) / kcos(r); }
        else if(tok_eq(nb, nl, "skewy") && cnt > 0){ double r = v[0] * 3.14159265358979 / 180; t.b = ksin(r) / kcos(r); }
        cur = mat_mul(cur, t);
    }
    return cur;
}

/* ---- the drawing state ---- */
typedef struct {
    int fill_kind, stroke_kind;      /* 0 none, 1 colour */
    u32 fill, stroke;
    double sw, opacity, fop, sop;
    int evenodd, round_cap, hidden;
    u32 color;                       /* currentColor */
    mat_t m;
} gs_t;

typedef struct {
    pt_t *p; int n, cap;
    int *start; u8 *closed; int ns, scap;
} path_t;

static int path_pt(path_t *pa, double x, double y){
    if(pa->n >= SVG_MAX_PTS) return 0;
    if(pa->n == pa->cap){
        int nc = pa->cap ? pa->cap * 2 : 256;
        pt_t *np = kmalloc((u32)nc * sizeof(pt_t));
        if(!np) return 0;
        if(pa->p){ memcpy(np, pa->p, (u32)pa->n * sizeof(pt_t)); kfree(pa->p); }
        pa->p = np; pa->cap = nc;
    }
    pa->p[pa->n].x = x; pa->p[pa->n].y = y; pa->n++;
    return 1;
}
static int path_sub(path_t *pa){
    if(pa->ns == pa->scap){
        int nc = pa->scap ? pa->scap * 2 : 16;
        int *ns = kmalloc((u32)nc * sizeof(int));
        u8 *nclosed = kmalloc((u32)nc);
        if(!ns || !nclosed){ if(ns) kfree(ns); if(nclosed) kfree(nclosed); return 0; }
        if(pa->start){ memcpy(ns, pa->start, (u32)pa->ns * sizeof(int)); kfree(pa->start); }
        if(pa->closed){ memcpy(nclosed, pa->closed, (u32)pa->ns); kfree(pa->closed); }
        pa->start = ns; pa->closed = nclosed; pa->scap = nc;
    }
    pa->start[pa->ns] = pa->n; pa->closed[pa->ns] = 0; pa->ns++;
    return 1;
}
static void path_free(path_t *pa){
    if(pa->p) kfree(pa->p);
    if(pa->start) kfree(pa->start);
    if(pa->closed) kfree(pa->closed);
    memset(pa, 0, sizeof *pa);
}

/* ---- the canvas ---- */
typedef struct {
    int w, h;
    u32 *px;          /* premultiplied 0xAARRGGBB */
    u16 *cov;         /* coverage of the shape being drawn, 0..256 */
} canvas_t;

static void composite(canvas_t *cv, int x0, int y0, int x1, int y1, u32 rgb, int alpha){
    u32 r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    for(int y = y0; y < y1; y++){
        u16 *c = cv->cov + (long)y * cv->w;
        u32 *d = cv->px + (long)y * cv->w;
        for(int x = x0; x < x1; x++){
            int a = c[x];
            if(!a) continue;
            c[x] = 0;
            if(a > 256) a = 256;
            a = a * alpha >> 8;
            if(a <= 0) continue;
            u32 dp = d[x];
            u32 da = dp >> 24, dr = dp >> 16 & 255, dg = dp >> 8 & 255, db = dp & 255;
            u32 ia = 256 - (u32)a;
            u32 na = ((u32)a * 255 >> 8) + (da * ia >> 8);
            u32 nr = ((u32)a * r >> 8) + (dr * ia >> 8);
            u32 ng = ((u32)a * g >> 8) + (dg * ia >> 8);
            u32 nb = ((u32)a * b >> 8) + (db * ia >> 8);
            if(na > 255) na = 255; if(nr > 255) nr = 255; if(ng > 255) ng = 255; if(nb > 255) nb = 255;
            d[x] = na << 24 | nr << 16 | ng << 8 | nb;
        }
    }
}

/* Fills polygons (each subpath closed; device coordinates) with the nonzero or even-odd rule.
 * 4 sub-scanlines a pixel, exact horizontal coverage. */
static void fill_poly(canvas_t *cv, const path_t *pa, int evenodd, u32 rgb, int alpha){
    if(pa->n < 3 || alpha <= 0) return;
    double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
    for(int i = 0; i < pa->n; i++){
        if(pa->p[i].x < minx) minx = pa->p[i].x;
        if(pa->p[i].x > maxx) maxx = pa->p[i].x;
        if(pa->p[i].y < miny) miny = pa->p[i].y;
        if(pa->p[i].y > maxy) maxy = pa->p[i].y;
    }
    if(maxx < 0 || maxy < 0 || minx > cv->w || miny > cv->h) return;
    int x0 = (int)kfloor(minx), x1 = (int)kceil(maxx) + 1, y0 = (int)kfloor(miny), y1 = (int)kceil(maxy) + 1;
    if(x0 < 0) x0 = 0; if(y0 < 0) y0 = 0; if(x1 > cv->w) x1 = cv->w; if(y1 > cv->h) y1 = cv->h;
    if(x1 <= x0 || y1 <= y0) return;
    /* crossings of one sub-scanline */
    int cap = 64, *dirs = NULL; double *xs = NULL;
    for(int y = y0; y < y1; y++){
        for(int s = 0; s < 4; s++){
            double sy = y + (2 * s + 1) / 8.0;
            int nc = 0;
            for(int sp = 0; sp < pa->ns; sp++){
                int b = pa->start[sp], e = sp + 1 < pa->ns ? pa->start[sp + 1] : pa->n;
                int cnt = e - b;
                if(cnt < 2) continue;
                for(int i = 0; i < cnt; i++){
                    pt_t p = pa->p[b + i], q = pa->p[b + (i + 1) % cnt];
                    if(p.y == q.y) continue;
                    int dir = 1;
                    if(p.y > q.y){ pt_t t = p; p = q; q = t; dir = -1; }
                    if(sy < p.y || sy >= q.y) continue;
                    double x = p.x + (sy - p.y) * (q.x - p.x) / (q.y - p.y);
                    if(nc == cap || !xs){
                        int ncap = xs ? cap * 2 : cap;
                        double *nx = kmalloc((u32)ncap * sizeof(double)); int *nd = kmalloc((u32)ncap * sizeof(int));
                        if(!nx || !nd){ if(nx) kfree(nx); if(nd) kfree(nd); goto done; }
                        if(xs){ memcpy(nx, xs, (u32)nc * sizeof(double)); memcpy(nd, dirs, (u32)nc * sizeof(int)); kfree(xs); kfree(dirs); }
                        xs = nx; dirs = nd; cap = ncap;
                    }
                    int k = nc++;
                    while(k > 0 && xs[k - 1] > x){ xs[k] = xs[k - 1]; dirs[k] = dirs[k - 1]; k--; }
                    xs[k] = x; dirs[k] = dir;
                }
            }
            int wind = 0;
            for(int i = 0; i + 1 < nc; i++){
                wind += evenodd ? 1 : dirs[i];
                int inside = evenodd ? (wind & 1) : wind != 0;
                if(!inside) continue;
                double xa = xs[i], xb = xs[i + 1];
                if(xb <= 0 || xa >= cv->w) continue;
                if(xa < 0) xa = 0;
                if(xb > cv->w) xb = cv->w;
                int ia = (int)xa, ib = (int)xb;
                u16 *c = cv->cov + (long)y * cv->w;
                if(ia == ib){ c[ia] = (u16)(c[ia] + (int)((xb - xa) * 64)); }
                else {
                    c[ia] = (u16)(c[ia] + (int)((ia + 1 - xa) * 64));
                    for(int x = ia + 1; x < ib; x++) c[x] = (u16)(c[x] + 64);
                    if(ib < cv->w) c[ib] = (u16)(c[ib] + (int)((xb - ib) * 64));
                }
            }
        }
    }
done:
    if(xs){ kfree(xs); kfree(dirs); }
    composite(cv, x0, y0, x1, y1, rgb, alpha);
}

/* ---- shapes to device-space polygons ---- */
static void add_arc_pts(path_t *out, const mat_t *m, double x0, double y0, double rx, double ry, double phi, int fa, int fs, double x, double y){
    if(rx < 0) rx = -rx;
    if(ry < 0) ry = -ry;
    if(rx == 0 || ry == 0 || (x0 == x && y0 == y)){ pt_t p = mat_pt(m, x, y); path_pt(out, p.x, p.y); return; }
    double cp = kcos(phi), sp = ksin(phi);
    double dx = (x0 - x) / 2, dy = (y0 - y) / 2;
    double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    double lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if(lam > 1){ double s = ksqrt(lam); rx *= s; ry *= s; }
    double num_ = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double co = den == 0 ? 0 : ksqrt(num_ / den > 0 ? num_ / den : 0);
    if(fa == fs) co = -co;
    double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    double cx = cp * cxp - sp * cyp + (x0 + x) / 2, cy = sp * cxp + cp * cyp + (y0 + y) / 2;
    double th1 = katan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double th2 = katan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    double dth = th2 - th1;
    const double PI = 3.14159265358979;
    if(fs && dth < 0) dth += 2 * PI;
    if(!fs && dth > 0) dth -= 2 * PI;
    int steps = (int)(kfabs(dth) / (PI / 16)) + 2;
    for(int i = 1; i <= steps; i++){
        double t = th1 + dth * i / steps;
        double ex = rx * kcos(t), ey = ry * ksin(t);
        pt_t p = mat_pt(m, cp * ex - sp * ey + cx, sp * ex + cp * ey + cy);
        path_pt(out, p.x, p.y);
    }
}

/* Parses path data into device-space subpaths. */
static void parse_path(const char *d, u32 n, const mat_t *m, path_t *out){
    const char *p = d, *e = d + n;
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;   /* current, subpath start, last control */
    char cmd = 0, prev = 0;
    int open = 0;
    for(;;){
        while(p < e && (is_ws(*p) || *p == ',')) p++;
        if(p >= e) break;
        if(is_alpha(*p)){ cmd = *p++; if(cmd == 'z' || cmd == 'Z'){
            if(open && out->ns) out->closed[out->ns - 1] = 1;
            cx = sx; cy = sy; open = 0; prev = 'z'; continue; } }
        else if(!cmd) break;
        int rel = cmd >= 'a';
        char C = (char)(rel ? cmd - 32 : cmd);
        double v[7];
        int need = C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6 : C == 'S' || C == 'Q' ? 4 : C == 'A' ? 7 : 0;
        if(!need) break;
        int ok = 1;
        for(int i = 0; i < need; i++){
            if(C == 'A' && (i == 3 || i == 4)){          /* the flags can be run together: "a1 1 0 01.5 2" */
                while(p < e && (is_ws(*p) || *p == ',')) p++;
                if(p < e && (*p == '0' || *p == '1')){ v[i] = *p - '0'; p++; } else { ok = 0; break; }
            } else if(!num(&p, e, &v[i])){ ok = 0; break; }
        }
        if(!ok) break;
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch(C){
        case 'M': {
            cx = v[0] + ox; cy = v[1] + oy; sx = cx; sy = cy;
            if(!path_sub(out)) return;
            pt_t q = mat_pt(m, cx, cy); path_pt(out, q.x, q.y);
            open = 1; cmd = rel ? 'l' : 'L';                 /* more pairs are line-tos */
            break; }
        case 'L': case 'H': case 'V': {
            double nx = C == 'V' ? cx : v[0] + ox, ny = C == 'H' ? cy : (C == 'V' ? v[0] + oy : v[1] + oy);
            if(C == 'H') nx = v[0] + ox;
            if(!open){ if(!path_sub(out)) return; pt_t q0 = mat_pt(m, cx, cy); path_pt(out, q0.x, q0.y); open = 1; sx = cx; sy = cy; }
            cx = nx; cy = ny;
            pt_t q = mat_pt(m, cx, cy); path_pt(out, q.x, q.y);
            break; }
        case 'C': case 'S': case 'Q': case 'T': {
            double x1, y1, x2, y2, x, y;
            if(C == 'C'){ x1 = v[0] + ox; y1 = v[1] + oy; x2 = v[2] + ox; y2 = v[3] + oy; x = v[4] + ox; y = v[5] + oy; }
            else if(C == 'S'){
                int pc = prev == 'C' || prev == 'S';
                x1 = pc ? 2 * cx - lcx : cx; y1 = pc ? 2 * cy - lcy : cy;
                x2 = v[0] + ox; y2 = v[1] + oy; x = v[2] + ox; y = v[3] + oy;
            } else if(C == 'Q'){ x1 = v[0] + ox; y1 = v[1] + oy; x = v[2] + ox; y = v[3] + oy; x2 = x1; y2 = y1; }
            else { int pq = prev == 'Q' || prev == 'T'; x1 = pq ? 2 * cx - lcx : cx; y1 = pq ? 2 * cy - lcy : cy; x = v[0] + ox; y = v[1] + oy; x2 = x1; y2 = y1; }
            if(!open){ if(!path_sub(out)) return; pt_t q0 = mat_pt(m, cx, cy); path_pt(out, q0.x, q0.y); open = 1; sx = cx; sy = cy; }
            int quad = C == 'Q' || C == 'T';
            pt_t a = mat_pt(m, cx, cy), b = mat_pt(m, x1, y1), c = mat_pt(m, x2, y2), dd = mat_pt(m, x, y);
            double len = kfabs(b.x - a.x) + kfabs(b.y - a.y) + kfabs(c.x - b.x) + kfabs(c.y - b.y) + kfabs(dd.x - c.x) + kfabs(dd.y - c.y);
            int steps = (int)(len / 3) + 4;
            if(steps > 60) steps = 60;
            for(int i = 1; i <= steps; i++){
                double t = (double)i / steps, u = 1 - t;
                double px, py;
                if(quad){
                    px = u * u * a.x + 2 * u * t * b.x + t * t * dd.x;
                    py = u * u * a.y + 2 * u * t * b.y + t * t * dd.y;
                } else {
                    px = u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * dd.x;
                    py = u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * dd.y;
                }
                path_pt(out, px, py);
            }
            lcx = quad ? x1 : x2; lcy = quad ? y1 : y2;
            cx = x; cy = y;
            break; }
        case 'A': {
            double x = v[5] + ox, y = v[6] + oy;
            if(!open){ if(!path_sub(out)) return; pt_t q0 = mat_pt(m, cx, cy); path_pt(out, q0.x, q0.y); open = 1; sx = cx; sy = cy; }
            add_arc_pts(out, m, cx, cy, v[0], v[1], v[2] * 3.14159265358979 / 180, v[3] != 0, v[4] != 0, x, y);
            cx = x; cy = y;
            break; }
        }
        prev = C;
    }
}

/* the outline of a stroked polyline as many consistently wound quads and join discs */
static void add_quad(path_t *o, pt_t a, pt_t b, pt_t c, pt_t d){
    double area = (a.x * b.y - b.x * a.y) + (b.x * c.y - c.x * b.y) + (c.x * d.y - d.x * c.y) + (d.x * a.y - a.x * d.y);
    if(!path_sub(o)) return;
    if(area >= 0){ path_pt(o, a.x, a.y); path_pt(o, b.x, b.y); path_pt(o, c.x, c.y); path_pt(o, d.x, d.y); }
    else { path_pt(o, d.x, d.y); path_pt(o, c.x, c.y); path_pt(o, b.x, b.y); path_pt(o, a.x, a.y); }
}
static void add_disc(path_t *o, pt_t c, double r){
    if(!path_sub(o)) return;
    for(int i = 0; i < 12; i++){
        double t = 2 * 3.14159265358979 * i / 12;
        path_pt(o, c.x + r * kcos(t), c.y + r * ksin(t));
    }
}
static void stroke_path(const path_t *src, double w, int round_cap, path_t *o){
    double h = w / 2;
    for(int sp = 0; sp < src->ns; sp++){
        int b = src->start[sp], e = sp + 1 < src->ns ? src->start[sp + 1] : src->n;
        int cnt = e - b;
        if(cnt < 2) continue;
        int segs = src->closed[sp] ? cnt : cnt - 1;
        for(int i = 0; i < segs; i++){
            pt_t p = src->p[b + i], q = src->p[b + (i + 1) % cnt];
            double dx = q.x - p.x, dy = q.y - p.y, len = ksqrt(dx * dx + dy * dy);
            if(len < 1e-6) continue;
            double nx = -dy / len * h, ny = dx / len * h;
            pt_t a = { p.x + nx, p.y + ny }, bb = { q.x + nx, q.y + ny }, c = { q.x - nx, q.y - ny }, d = { p.x - nx, p.y - ny };
            add_quad(o, a, bb, c, d);
            if(i > 0 || src->closed[sp]) add_disc(o, p, h);         /* round joins */
        }
        if(round_cap && !src->closed[sp]){ add_disc(o, src->p[b], h); add_disc(o, src->p[e - 1], h); }
    }
}

/* ---- drawing one shape with the current state ---- */
static void draw_shape(canvas_t *cv, const gs_t *g, path_t *pa){
    if(g->hidden || pa->n < 2) return;
    if(g->fill_kind){
        int a = (int)(256 * g->opacity * g->fop);
        fill_poly(cv, pa, g->evenodd, g->fill, a);
    }
    if(g->stroke_kind && g->sw > 0){
        double sc = ksqrt(kfabs(g->m.a * g->m.d - g->m.b * g->m.c));
        double w = g->sw * sc;
        if(w < 0.6) w = 0.6;
        path_t so = { 0 };
        stroke_path(pa, w, g->round_cap, &so);
        fill_poly(cv, &so, 0, g->stroke, (int)(256 * g->opacity * g->sop));
        path_free(&so);
    }
}

/* ---- style application ---- */
static void apply_prop(gs_t *g, const char *k, u32 kl, const char *v, u32 vl, const char *tag_unused){
    (void)tag_unused;
    while(vl && is_ws(*v)){ v++; vl--; }
    while(vl && is_ws(v[vl - 1])) vl--;
    u32 rgb; const char *id; u32 idl;
    if(tok_eq(k, kl, "fill") || tok_eq(k, kl, "stroke")){
        int is_fill = tok_eq(k, kl, "fill");
        int r = paint_of(v, vl, &rgb, &id, &idl);
        int kind = 0; u32 col = 0;
        if(r == 1){ kind = 1; col = rgb; }
        else if(r == 3){ kind = 1; col = g->color; }
        else if(r == 4){ if(grad_find(id, idl, &rgb)){ kind = 1; col = rgb; } }
        else if(r == 2) kind = 0;
        else return;
        if(is_fill){ g->fill_kind = kind; g->fill = col; } else { g->stroke_kind = kind; g->stroke = col; }
    }
    else if(tok_eq(k, kl, "color")){ if(paint_of(v, vl, &rgb, &id, &idl) == 1) g->color = rgb; }
    else if(tok_eq(k, kl, "stroke-width")){ double x; int pct; if(length(v, vl, &x, &pct) && !pct) g->sw = x; }
    else if(tok_eq(k, kl, "opacity")){ double x; const char *q = v; if(num(&q, v + vl, &x)) g->opacity *= x < 0 ? 0 : x > 1 ? 1 : x; }
    else if(tok_eq(k, kl, "fill-opacity")){ double x; const char *q = v; if(num(&q, v + vl, &x)) g->fop = x < 0 ? 0 : x > 1 ? 1 : x; }
    else if(tok_eq(k, kl, "stroke-opacity")){ double x; const char *q = v; if(num(&q, v + vl, &x)) g->sop = x < 0 ? 0 : x > 1 ? 1 : x; }
    else if(tok_eq(k, kl, "fill-rule")) g->evenodd = tok_eq(v, vl, "evenodd");
    else if(tok_eq(k, kl, "stroke-linecap")) g->round_cap = tok_eq(v, vl, "round");
    else if(tok_eq(k, kl, "display")){ if(tok_eq(v, vl, "none")) g->hidden = 1; }
    else if(tok_eq(k, kl, "visibility")){ if(tok_eq(v, vl, "hidden")) g->hidden = 1; }
}

static void apply_decls(gs_t *g, const char *s, u32 n){
    const char *e = s + n;
    while(s < e){
        const char *kb = s;
        while(s < e && *s != ':' && *s != ';') s++;
        if(s >= e || *s == ';'){ if(s < e) s++; continue; }
        u32 kl = (u32)(s - kb);
        while(kl && is_ws(kb[kl - 1])) kl--;
        while(kl && is_ws(*kb)){ kb++; kl--; }
        s++;
        const char *vb = s;
        int paren = 0;
        while(s < e && (paren || *s != ';')){ if(*s == '(') paren++; if(*s == ')') paren--; s++; }
        u32 vl = (u32)(s - vb);
        const char *imp = vb;
        for(u32 i = 0; i + 9 <= vl; i++) if(vb[i] == '!') imp = vb + i;
        if(imp != vb) vl = (u32)(imp - vb);
        apply_prop(g, kb, kl, vb, vl, NULL);
        if(s < e) s++;
    }
}

static void apply_attrs(gs_t *g, const attrs_t *as, const char *tag, u32 tl){
    /* <style> rules first (tag selectors, then classes), then presentation attributes, then style="" */
    for(int i = 0; i < nrules; i++)
        if(!rules[i].cls && (rules[i].sel[0] == '*' || (strlen(rules[i].sel) == tl && tok_eq(tag, tl, rules[i].sel)))) apply_decls(g, rules[i].decl, (u32)strlen(rules[i].decl));
    static const char *const pres[] = { "fill", "stroke", "stroke-width", "opacity", "fill-opacity", "stroke-opacity", "fill-rule", "stroke-linecap", "display", "color", 0 };
    for(int k = 0; pres[k]; k++){
        const attr_t *a = attr(as, pres[k]);
        if(a) apply_prop(g, a->n, a->nl, a->v, a->vl, NULL);
    }
    const attr_t *cls = attr(as, "class");
    if(cls){
        const char *s = cls->v, *e = cls->v + cls->vl;
        while(s < e){
            while(s < e && is_ws(*s)) s++;
            const char *b = s;
            while(s < e && !is_ws(*s)) s++;
            u32 l = (u32)(s - b);
            if(!l) break;
            for(int i = 0; i < nrules; i++)
                if(rules[i].cls && strlen(rules[i].sel) == l && !memcmp(rules[i].sel, b, l)) apply_decls(g, rules[i].decl, (u32)strlen(rules[i].decl));
        }
    }
    const attr_t *st = attr(as, "style");
    if(st) apply_decls(g, st->v, st->vl);
    const attr_t *tr = attr(as, "transform");
    if(tr) g->m = parse_transform(tr->v, tr->vl, g->m);
}

static void parse_style_text(const char *s, const char *e){
    while(s < e && nrules < SVG_RULES){
        while(s < e && (is_ws(*s) || *s == '}')) s++;
        if(s + 1 < e && s[0] == '/' && s[1] == '*'){ while(s + 1 < e && !(s[0] == '*' && s[1] == '/')) s++; s += 2; continue; }
        if(s < e && *s == '@'){ while(s < e && *s != '{' && *s != ';') s++; if(s < e && *s == '{'){ int d = 1; s++; while(s < e && d){ if(*s == '{') d++; if(*s == '}') d--; s++; } } else if(s < e) s++; continue; }
        const char *sb = s;
        while(s < e && *s != '{') s++;
        if(s >= e) break;
        const char *se = s;
        s++;
        const char *db = s;
        while(s < e && *s != '}') s++;
        u32 dl = (u32)(s - db);
        /* selectors separated by commas */
        const char *q = sb;
        while(q < se && nrules < SVG_RULES){
            while(q < se && (is_ws(*q) || *q == ',')) q++;
            const char *b = q;
            while(q < se && *q != ',' && !is_ws(*q)) q++;
            u32 l = (u32)(q - b);
            if(!l) break;
            int cls = b[0] == '.';
            if(cls){ b++; l--; }
            if(l && l < sizeof rules[0].sel && dl < sizeof rules[0].decl && b[0] != '#' && b[0] != ':'){
                memcpy(rules[nrules].sel, b, l);
                rules[nrules].sel[l] = 0;
                for(u32 i = 0; i < l; i++) rules[nrules].sel[i] = (char)lc((unsigned char)rules[nrules].sel[i]);
                if(cls) for(u32 i = 0; i < l; i++) rules[nrules].sel[i] = b[i];       /* class names keep their case */
                rules[nrules].cls = cls;
                memcpy(rules[nrules].decl, db, dl);
                rules[nrules].decl[dl] = 0;
                nrules++;
            }
            while(q < se && !(*q == ',')) { if(is_ws(*q)) break; q++; }
            /* skip combinators: only the first simple selector of a compound is used */
            while(q < se && *q != ',') q++;
        }
        if(s < e) s++;
    }
}

/* ---- the document ---- */
typedef struct { gs_t g; int skip_to; } frame_t;
#define MAXDEPTH 48

int svg_decode(const u8 *data, u32 len, u32 **out_px, u32 *out_w, u32 *out_h, int *ss_out){
    const char *s = (const char *)data, *end = s + len;
    nrules = 0; ngrads = 0;
    /* the <svg> root tag first, for the size */
    const char *q = s;
    while(q + 4 < end && !(q[0] == '<' && lc((unsigned char)q[1]) == 's' && lc((unsigned char)q[2]) == 'v' && lc((unsigned char)q[3]) == 'g' && (is_ws(q[4]) || q[4] == '>'))) q++;
    if(q + 4 >= end) return -1;

    /* pass 1: size, <style> rules, gradients */
    double vbx = 0, vby = 0, vbw = 0, vbh = 0, W = 0, H = 0;
    int have_vb = 0, none_ar = 0;
    {
        const char *p = q;
        int depth = 0;
        grad_t *cur = NULL;
        while(p < end){
            if(*p != '<'){ p++; continue; }
            if(p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-'){ while(p + 2 < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) p++; p += 3; continue; }
            if(p + 8 < end && !memcmp(p, "<![CDATA[", 9)){ p += 9; continue; }
            if(p + 1 < end && (p[1] == '?' || p[1] == '!')){ while(p < end && *p != '>') p++; p++; continue; }
            int closing = p + 1 < end && p[1] == '/';
            const char *nb = p + 1 + closing;
            const char *t = nb;
            while(t < end && !is_ws(*t) && *t != '>' && *t != '/') t++;
            u32 nl = (u32)(t - nb);
            attrs_t as; as.n = 0;
            const char *tag_end = t;
            if(!closing){
                while(tag_end < end && *tag_end != '>'){
                    while(tag_end < end && (is_ws(*tag_end) || *tag_end == '/')) tag_end++;
                    if(tag_end >= end || *tag_end == '>') break;
                    const char *an = tag_end;
                    while(tag_end < end && *tag_end != '=' && !is_ws(*tag_end) && *tag_end != '>' && *tag_end != '/') tag_end++;
                    u32 anl = (u32)(tag_end - an);
                    while(tag_end < end && is_ws(*tag_end)) tag_end++;
                    if(tag_end < end && *tag_end == '='){
                        tag_end++;
                        while(tag_end < end && is_ws(*tag_end)) tag_end++;
                        char qt = tag_end < end && (*tag_end == '"' || *tag_end == '\'') ? *tag_end++ : 0;
                        const char *vb = tag_end;
                        while(tag_end < end && (qt ? *tag_end != qt : (!is_ws(*tag_end) && *tag_end != '>'))) tag_end++;
                        if(as.n < MAXATTR){ as.a[as.n].n = an; as.a[as.n].nl = anl; as.a[as.n].v = vb; as.a[as.n].vl = (u32)(tag_end - vb); as.n++; }
                        if(qt && tag_end < end) tag_end++;
                    } else if(!anl) tag_end++;
                }
            } else { while(tag_end < end && *tag_end != '>') tag_end++; }
            int selfclose = tag_end > p && tag_end[-1] == '/';
            if(tok_eq(nb, nl, "svg") && !closing && depth == 0){
                const attr_t *a;
                double v; int pct;
                if((a = attr(&as, "width")) && length(a->v, a->vl, &v, &pct) && !pct) W = v;
                if((a = attr(&as, "height")) && length(a->v, a->vl, &v, &pct) && !pct) H = v;
                if((a = attr(&as, "viewbox"))){
                    const char *vp = a->v, *ve = a->v + a->vl;
                    double b0, b1, b2, b3;
                    if(num(&vp, ve, &b0) && num(&vp, ve, &b1) && num(&vp, ve, &b2) && num(&vp, ve, &b3) && b2 > 0 && b3 > 0){ vbx = b0; vby = b1; vbw = b2; vbh = b3; have_vb = 1; }
                }
                if((a = attr(&as, "preserveaspectratio")) && tok_eq(a->v, a->vl > 4 ? 4 : a->vl, "none")) none_ar = 1;
            }
            if(tok_eq(nb, nl, "style") && !closing && !selfclose){
                const char *tb = tag_end + 1, *te = tb;
                while(te + 7 < end && !(te[0] == '<' && te[1] == '/')) te++;
                const char *tb2 = tb;
                if(tb2 + 8 < te && !memcmp(tb2, "<![CDATA[", 9)) tb2 += 9;
                const char *te2 = te;
                if(te2 - 3 > tb2 && !memcmp(te2 - 3, "]]>", 3)) te2 -= 3;
                parse_style_text(tb2, te2);
                p = te;
                continue;
            }
            if((tok_eq(nb, nl, "lineargradient") || tok_eq(nb, nl, "radialgradient")) && !closing){
                const attr_t *a = attr(&as, "id");
                cur = NULL;
                if(a && ngrads < SVG_GRADS && a->vl < sizeof grads[0].id){
                    cur = &grads[ngrads++];
                    memcpy(cur->id, a->v, a->vl); cur->id[a->vl] = 0; cur->n = 0; cur->c0 = cur->c1 = 0;
                }
                if(selfclose) cur = NULL;
            }
            if(tok_eq(nb, nl, "stop") && !closing && cur){
                u32 col = 0; int got = 0;
                const attr_t *a = attr(&as, "stop-color");
                u32 rgb; const char *id; u32 idl;
                if(a && paint_of(a->v, a->vl, &rgb, &id, &idl) == 1){ col = rgb; got = 1; }
                const attr_t *st = attr(&as, "style");
                if(st){
                    const char *sp = st->v, *se = st->v + st->vl;
                    for(; sp + 10 < se; sp++){
                        if(!memcmp(sp, "stop-color", 10)){
                            const char *vp = sp + 10;
                            while(vp < se && (*vp == ':' || is_ws(*vp))) vp++;
                            const char *ve = vp;
                            while(ve < se && *ve != ';') ve++;
                            if(paint_of(vp, (u32)(ve - vp), &rgb, &id, &idl) == 1){ col = rgb; got = 1; }
                            break;
                        }
                    }
                }
                if(got){ if(!cur->n) cur->c0 = col; cur->c1 = col; cur->n++; }
            }
            if(closing && (tok_eq(nb, nl, "lineargradient") || tok_eq(nb, nl, "radialgradient"))) cur = NULL;
            if(!closing && !selfclose) depth++;
            if(closing && depth > 0) depth--;
            p = tag_end < end ? tag_end + 1 : end;
        }
    }
    double iw = W, ih = H;
    if(iw <= 0 && ih <= 0){ if(have_vb){ iw = vbw; ih = vbh; } else { iw = 300; ih = 150; } }
    else if(iw <= 0){ iw = have_vb ? ih * vbw / vbh : ih; }
    else if(ih <= 0){ ih = have_vb ? iw * vbh / vbw : iw; }
    if(!have_vb){ vbx = 0; vby = 0; vbw = iw; vbh = ih; }
    if(iw < 1) iw = 1;
    if(ih < 1) ih = 1;
    if(iw > 4096 || ih > 4096){ double k = (iw > ih ? iw : ih) / 4096.0; iw /= k; ih /= k; }
    /* drawn a little larger when small, so scaling it up still looks sharp */
    int ss = (iw <= 200 && ih <= 200) ? 2 : 1;
    int rw = (int)(iw * ss + 0.5), rh = (int)(ih * ss + 0.5);
    if(rw > SVG_MAX_SIDE || rh > SVG_MAX_SIDE){ ss = 1; rw = (int)(iw + 0.5); rh = (int)(ih + 0.5); }
    while(rw > SVG_MAX_SIDE || rh > SVG_MAX_SIDE){ rw /= 2; rh /= 2; }
    if(rw < 1) rw = 1;
    if(rh < 1) rh = 1;

    canvas_t cv;
    cv.w = rw; cv.h = rh;
    cv.px = kmalloc((u32)rw * (u32)rh * 4);
    cv.cov = kmalloc((u32)rw * (u32)rh * 2);
    if(!cv.px || !cv.cov){ if(cv.px) kfree(cv.px); if(cv.cov) kfree(cv.cov); return -1; }
    memset(cv.px, 0, (u32)rw * (u32)rh * 4);
    memset(cv.cov, 0, (u32)rw * (u32)rh * 2);

    /* the viewBox onto the canvas */
    mat_t base;
    {
        double sx = rw / vbw, sy = rh / vbh;
        if(!none_ar){ double k = sx < sy ? sx : sy; sx = sy = k; }
        base.a = sx; base.b = 0; base.c = 0; base.d = sy;
        base.e = (rw - vbw * sx) / 2 - vbx * sx;
        base.f = (rh - vbh * sy) / 2 - vby * sy;
    }

    /* pass 2: draw */
    frame_t *stack = kmalloc(sizeof(frame_t) * MAXDEPTH);
    if(!stack){ kfree(cv.px); kfree(cv.cov); return -1; }
    int sp = 0;
    memset(&stack[0], 0, sizeof stack[0]);
    stack[0].g = (gs_t){ .fill_kind = 1, .fill = 0x000000, .stroke_kind = 0, .sw = 1, .opacity = 1, .fop = 1, .sop = 1, .color = 0x000000, .m = base };
    stack[0].skip_to = -1;
    int skip_depth = -1;                    /* inside a subtree we do not draw: its depth */
    int depth = 0;
    const char *p = q;
    while(p < end){
        if(*p != '<'){ p++; continue; }
        if(p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-'){ while(p + 2 < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) p++; p += 3; continue; }
        if(p + 8 < end && !memcmp(p, "<![CDATA[", 9)){ p += 9; continue; }
        if(p + 1 < end && (p[1] == '?' || p[1] == '!')){ while(p < end && *p != '>') p++; p++; continue; }
        int closing = p + 1 < end && p[1] == '/';
        const char *nb = p + 1 + closing;
        const char *t = nb;
        while(t < end && !is_ws(*t) && *t != '>' && *t != '/') t++;
        u32 nl = (u32)(t - nb);
        attrs_t as; as.n = 0;
        const char *tag_end = t;
        if(!closing){
            while(tag_end < end && *tag_end != '>'){
                while(tag_end < end && (is_ws(*tag_end) || *tag_end == '/')) tag_end++;
                if(tag_end >= end || *tag_end == '>') break;
                const char *an = tag_end;
                while(tag_end < end && *tag_end != '=' && !is_ws(*tag_end) && *tag_end != '>' && *tag_end != '/') tag_end++;
                u32 anl = (u32)(tag_end - an);
                while(tag_end < end && is_ws(*tag_end)) tag_end++;
                if(tag_end < end && *tag_end == '='){
                    tag_end++;
                    while(tag_end < end && is_ws(*tag_end)) tag_end++;
                    char qt = tag_end < end && (*tag_end == '"' || *tag_end == '\'') ? *tag_end++ : 0;
                    const char *vb = tag_end;
                    while(tag_end < end && (qt ? *tag_end != qt : (!is_ws(*tag_end) && *tag_end != '>'))) tag_end++;
                    if(as.n < MAXATTR){ as.a[as.n].n = an; as.a[as.n].nl = anl; as.a[as.n].v = vb; as.a[as.n].vl = (u32)(tag_end - vb); as.n++; }
                    if(qt && tag_end < end) tag_end++;
                } else if(!anl) tag_end++;
            }
        } else { while(tag_end < end && *tag_end != '>') tag_end++; }
        int selfclose = !closing && tag_end > p && tag_end[-1] == '/';
        const char *next = tag_end < end ? tag_end + 1 : end;

        if(closing){
            if(depth > 0) depth--;
            if(skip_depth >= 0 && depth == skip_depth) skip_depth = -1;
            if(sp > 0 && depth == stack[sp].skip_to) sp--;      /* the end of the container on top */
            p = next;
            continue;
        }
        if(skip_depth >= 0){ if(!selfclose) depth++; p = next; continue; }

        int container = tok_eq(nb, nl, "g") || tok_eq(nb, nl, "svg") || tok_eq(nb, nl, "a") || tok_eq(nb, nl, "switch");
        int skipper = tok_eq(nb, nl, "defs") || tok_eq(nb, nl, "clippath") || tok_eq(nb, nl, "mask") || tok_eq(nb, nl, "symbol") ||
                      tok_eq(nb, nl, "pattern") || tok_eq(nb, nl, "filter") || tok_eq(nb, nl, "style") || tok_eq(nb, nl, "title") ||
                      tok_eq(nb, nl, "desc") || tok_eq(nb, nl, "metadata") || tok_eq(nb, nl, "lineargradient") ||
                      tok_eq(nb, nl, "radialgradient") || tok_eq(nb, nl, "text") || tok_eq(nb, nl, "image") ||
                      tok_eq(nb, nl, "foreignobject") || tok_eq(nb, nl, "marker") || tok_eq(nb, nl, "script");
        if(skipper){
            if(!selfclose){ skip_depth = depth; depth++; }
            p = next;
            continue;
        }
        gs_t g = stack[sp].g;
        if(!(tok_eq(nb, nl, "svg") && depth == 0)) apply_attrs(&g, &as, nb, nl);
        else apply_attrs(&g, &as, nb, nl), g.m = base;        /* the root's own transform is not used */
        if(tok_eq(nb, nl, "svg") && depth == 0){ g.m = base; }
        if(container){
            if(!selfclose){
                if(sp + 1 < MAXDEPTH){ sp++; stack[sp].g = g; stack[sp].skip_to = depth; }
                else { skip_depth = depth; }
                depth++;
            }
            p = next;
            continue;
        }
        path_t pa = { 0 };
        if(tok_eq(nb, nl, "path")){
            const attr_t *d = attr(&as, "d");
            if(d) parse_path(d->v, d->vl, &g.m, &pa);
        } else if(tok_eq(nb, nl, "rect")){
            double x = attr_num(&as, "x", 0), y = attr_num(&as, "y", 0), w = attr_num(&as, "width", 0), h = attr_num(&as, "height", 0);
            double rx = attr_num(&as, "rx", -1), ry = attr_num(&as, "ry", -1);
            if(rx < 0 && ry >= 0) rx = ry;
            if(ry < 0 && rx >= 0) ry = rx;
            if(rx < 0) rx = 0;
            if(ry < 0) ry = 0;
            if(rx > w / 2) rx = w / 2;
            if(ry > h / 2) ry = h / 2;
            if(w > 0 && h > 0){
                path_sub(&pa);
                const double PI = 3.14159265358979;
                if(rx <= 0 || ry <= 0){
                    pt_t c0 = mat_pt(&g.m, x, y), c1 = mat_pt(&g.m, x + w, y), c2 = mat_pt(&g.m, x + w, y + h), c3 = mat_pt(&g.m, x, y + h);
                    path_pt(&pa, c0.x, c0.y); path_pt(&pa, c1.x, c1.y); path_pt(&pa, c2.x, c2.y); path_pt(&pa, c3.x, c3.y);
                } else {
                    double cxs[4] = { x + w - rx, x + w - rx, x + rx, x + rx }, cys[4] = { y + ry, y + h - ry, y + h - ry, y + ry };
                    for(int k = 0; k < 4; k++)
                        for(int i = 0; i <= 8; i++){
                            double a = (-PI / 2) + (PI / 2) * (k + (double)i / 8);
                            pt_t pp = mat_pt(&g.m, cxs[k] + rx * kcos(a), cys[k] + ry * ksin(a));
                            path_pt(&pa, pp.x, pp.y);
                        }
                }
                if(pa.ns) pa.closed[0] = 1;
            }
        } else if(tok_eq(nb, nl, "circle") || tok_eq(nb, nl, "ellipse")){
            double cx = attr_num(&as, "cx", 0), cy = attr_num(&as, "cy", 0), rx, ry;
            if(tok_eq(nb, nl, "circle")){ rx = ry = attr_num(&as, "r", 0); }
            else { rx = attr_num(&as, "rx", 0); ry = attr_num(&as, "ry", 0); }
            if(rx > 0 && ry > 0){
                path_sub(&pa);
                for(int i = 0; i < 64; i++){
                    double a = 2 * 3.14159265358979 * i / 64;
                    pt_t pp = mat_pt(&g.m, cx + rx * kcos(a), cy + ry * ksin(a));
                    path_pt(&pa, pp.x, pp.y);
                }
                pa.closed[0] = 1;
            }
        } else if(tok_eq(nb, nl, "line")){
            path_sub(&pa);
            pt_t a = mat_pt(&g.m, attr_num(&as, "x1", 0), attr_num(&as, "y1", 0)), b = mat_pt(&g.m, attr_num(&as, "x2", 0), attr_num(&as, "y2", 0));
            path_pt(&pa, a.x, a.y); path_pt(&pa, b.x, b.y);
            g.fill_kind = 0;
        } else if(tok_eq(nb, nl, "polyline") || tok_eq(nb, nl, "polygon")){
            const attr_t *a = attr(&as, "points");
            if(a){
                path_sub(&pa);
                const char *pp = a->v, *pe = a->v + a->vl;
                double x, y;
                while(num(&pp, pe, &x) && num(&pp, pe, &y)){ pt_t m = mat_pt(&g.m, x, y); path_pt(&pa, m.x, m.y); }
                if(tok_eq(nb, nl, "polygon") && pa.ns) pa.closed[0] = 1;
            }
        }
        if(pa.n) draw_shape(&cv, &g, &pa);
        path_free(&pa);
        if(!selfclose) depth++;       /* an unknown element with children: its end tag comes */
        p = next;
    }
    kfree(stack);
    kfree(cv.cov);

    /* premultiplied -> the engine's format: top byte transparency, colour straight */
    u32 n = (u32)rw * (u32)rh;
    for(u32 i = 0; i < n; i++){
        u32 v = cv.px[i], a = v >> 24;
        if(a == 0){ cv.px[i] = 0xFF000000u; continue; }
        u32 r = (v >> 16 & 255) * 255 / a, gg = (v >> 8 & 255) * 255 / a, b = (v & 255) * 255 / a;
        if(r > 255) r = 255; if(gg > 255) gg = 255; if(b > 255) b = 255;
        cv.px[i] = ((255 - a) << 24) | (r << 16) | (gg << 8) | b;
    }
    *out_px = cv.px; *out_w = (u32)rw; *out_h = (u32)rh;
    if(ss_out) *ss_out = ss;
    return 0;
}
