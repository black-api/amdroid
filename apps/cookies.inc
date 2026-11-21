/* cookies.inc — Browser's cookie jar (included by browser.c).
 *
 * Every request carries the cookies of its site (Cookie header) and every answer's Set-Cookie
 * headers go into the jar, including those of redirects (the HTTPS proxy passes them all on).
 * Pages see theirs as document.cookie: the browser hands Husser the page's cookies with the
 * page, and what scripts assign comes back the same way. Rules: Domain (host-only when not
 * given), Path, Secure, HttpOnly (not shown to scripts), Max-Age and Expires. The jar lasts
 * until the machine is turned off. */
#include "amdroid/rtc.h"

#define CK_MAX 320
typedef struct {
    u8 used, host_only, secure, http_only;
    char name[96];
    char domain[96];
    char path[128];
    char *value;
    u64 expires;            /* seconds since 1970; 0 = when the browser closes (never, here) */
    u32 born;               /* order of creation, to drop the oldest when full */
} cookie_t;
static cookie_t jar[CK_MAX];
static u32 cookie_seq_consumed = 0, cookie_off = 0;     /* document.cookie writes read so far */
static u32 ck_born = 0;

static u64 ck_days(int y, int m, int d){          /* days since 1970-01-01 (civil calendar) */
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (u64)(i64)(era * 146097 + doe - 719468);
}
static u64 ck_now(void){
    rtc_time_t t;
    rtc_read(&t);
    return ck_days(t.year, t.month, t.day) * 86400 + (u64)t.hour * 3600 + (u64)t.min * 60 + t.sec;
}
static int ck_lower(int c){ return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ck_ieq(const char *a, u32 n, const char *lit){
    u32 k = 0;
    for(; k < n && lit[k]; k++) if(ck_lower(a[k]) != lit[k]) return 0;
    return k == n && !lit[k];
}

/* "Wed, 21 Oct 2026 07:28:00 GMT" and the older "Wednesday, 21-Oct-26 07:28:00 GMT". 0 if not a date. */
static u64 ck_date(const char *s, u32 n){
    static const char *mon = "janfebmaraprmayjunjulaugsepoctnovdec";
    int day = -1, month = -1, year = -1, hh = 0, mm = 0, ss = 0;
    u32 i = 0;
    while(i < n){
        while(i < n && !((s[i] >= '0' && s[i] <= '9') || (ck_lower(s[i]) >= 'a' && ck_lower(s[i]) <= 'z'))) i++;
        u32 b = i;
        if(i < n && s[i] >= '0' && s[i] <= '9'){
            int v = 0;
            while(i < n && s[i] >= '0' && s[i] <= '9'){ v = v * 10 + (s[i] - '0'); i++; }
            if(i < n && s[i] == ':'){
                hh = v; i++;
                mm = 0; while(i < n && s[i] >= '0' && s[i] <= '9'){ mm = mm * 10 + (s[i] - '0'); i++; }
                if(i < n && s[i] == ':'){ i++; ss = 0; while(i < n && s[i] >= '0' && s[i] <= '9'){ ss = ss * 10 + (s[i] - '0'); i++; } }
            } else if(day < 0 && i - b <= 2) day = v;
            else if(year < 0) year = v < 70 ? 2000 + v : v < 100 ? 1900 + v : v;
        } else {
            while(i < n && ck_lower(s[i]) >= 'a' && ck_lower(s[i]) <= 'z') i++;
            if(i - b >= 3 && month < 0)
                for(int m = 0; m < 12; m++)
                    if(ck_lower(s[b]) == mon[m * 3] && ck_lower(s[b + 1]) == mon[m * 3 + 1] && ck_lower(s[b + 2]) == mon[m * 3 + 2]) month = m + 1;
            if(i == b) i++;
        }
    }
    if(day < 1 || month < 1 || year < 1601) return 0;
    if(year < 1970) return 1;                              /* long past (0 means "not a date") */
    u64 t = ck_days(year, month, day) * 86400 + (u64)hh * 3600 + (u64)mm * 60 + (u64)ss;
    return t ? t : 1;
}

static int ck_domain_ok(const char *host, const char *dom, int host_only){
    u32 hl = ui_len(host), dl = ui_len(dom);
    if(hl == dl) return ck_ieq(host, hl, dom);
    if(host_only || dl >= hl) return 0;
    if(host[hl - dl - 1] != '.') return 0;
    for(u32 k = 0; k < dl; k++) if(ck_lower(host[hl - dl + k]) != ck_lower(dom[k])) return 0;
    return 1;
}
static int ck_path_ok(const char *req, const char *cp){
    u32 n = ui_len(cp);
    for(u32 k = 0; k < n; k++) if(req[k] != cp[k]) return 0;
    return req[n] == 0 || req[n] == '?' || cp[n - 1] == '/' || req[n] == '/';
}
static void ck_drop(cookie_t *c){ if(c->value) kfree(c->value); c->value = NULL; c->used = 0; }

/* One Set-Cookie header value, from an answer to url. from_script: document.cookie (it may
 * not set or replace HttpOnly cookies). */
static void ck_set(const char *url, const char *v, u32 n, int from_script){
    url_t u;
    if(parse_url(url, &u) != 0) return;
    char host[HOST_MAX];
    u32 hk = 0;
    for(; u.host[hk] && hk + 1 < sizeof host; hk++) host[hk] = (char)ck_lower(u.host[hk]);
    host[hk] = 0;
    /* name=value up to the first ';' */
    u32 e = 0;
    while(e < n && v[e] != ';') e++;
    u32 eq = 0;
    while(eq < e && v[eq] != '=') eq++;
    if(eq >= e) return;                                 /* no '=': nothing to keep */
    u32 ns = 0, ne = eq;
    while(ns < ne && v[ns] == ' ') ns++;
    while(ne > ns && v[ne - 1] == ' ') ne--;
    u32 vs = eq + 1, ve = e;
    while(vs < ve && v[vs] == ' ') vs++;
    while(ve > vs && v[ve - 1] == ' ') ve--;
    if(ne == ns || ne - ns >= sizeof jar[0].name || ve - vs > 4000) return;
    cookie_t c;
    memset(&c, 0, sizeof c);
    c.host_only = 1;
    ui_copy(c.domain, host, sizeof c.domain);
    /* the default path: the request path up to its last '/' */
    {
        const char *p = u.path;
        u32 last = 0;
        for(u32 k = 0; p[k] && p[k] != '?'; k++) if(p[k] == '/') last = k;
        if(last == 0){ c.path[0] = '/'; c.path[1] = 0; }
        else { u32 l = last < sizeof c.path - 1 ? last : sizeof c.path - 1; memcpy(c.path, p, l); c.path[l] = 0; }
    }
    int remove = 0;
    u64 now = ck_now();
    int have_max_age = 0;
    for(u32 a = e; a < n; ){
        a++;
        while(a < n && v[a] == ' ') a++;
        u32 b = a;
        while(a < n && v[a] != ';') a++;
        u32 k = b;
        while(k < a && v[k] != '=') k++;
        const char *an = v + b; u32 al = k - b;
        while(al && an[al - 1] == ' ') al--;
        const char *av = k < a ? v + k + 1 : v + a; u32 avl = k < a ? a - k - 1 : 0;
        while(avl && *av == ' '){ av++; avl--; }
        while(avl && av[avl - 1] == ' ') avl--;
        if(ck_ieq(an, al, "domain") && avl){
            if(*av == '.'){ av++; avl--; }
            char d[96];
            u32 dl = 0;
            for(; dl < avl && dl + 1 < sizeof d; dl++) d[dl] = (char)ck_lower(av[dl]);
            d[dl] = 0;
            int dots = 0;
            for(u32 q = 0; q < dl; q++) if(d[q] == '.') dots++;
            if(!ck_domain_ok(host, d, 0) || (!dots && !ck_ieq(host, hk, d))) return;   /* another site's, or "com" */
            ui_copy(c.domain, d, sizeof c.domain);
            c.host_only = 0;
        } else if(ck_ieq(an, al, "path") && avl && *av == '/'){
            u32 l = avl < sizeof c.path - 1 ? avl : sizeof c.path - 1;
            memcpy(c.path, av, l); c.path[l] = 0;
        } else if(ck_ieq(an, al, "secure")) c.secure = 1;
        else if(ck_ieq(an, al, "httponly")) c.http_only = 1;
        else if(ck_ieq(an, al, "max-age")){
            int neg = avl && *av == '-';
            i64 s = 0;
            for(u32 q = neg; q < avl && av[q] >= '0' && av[q] <= '9'; q++) s = s * 10 + (av[q] - '0');
            have_max_age = 1;
            if(neg || s == 0) remove = 1; else c.expires = now + (u64)s;
        } else if(ck_ieq(an, al, "expires") && !have_max_age){
            u64 t = ck_date(av, avl);
            if(t && t <= now) remove = 1; else if(t) c.expires = t;
        }
    }
    if(c.secure && !u.https) return;
    memcpy(c.name, v + ns, ne - ns); c.name[ne - ns] = 0;
    /* the same name, domain and path is replaced */
    for(int i = 0; i < CK_MAX; i++){
        cookie_t *o = &jar[i];
        if(!o->used || strcmp(o->name, c.name) != 0 || strcmp(o->domain, c.domain) != 0 || strcmp(o->path, c.path) != 0) continue;
        if(from_script && o->http_only) return;
        ck_drop(o);
    }
    if(remove || (from_script && c.http_only)) return;
    int slot = -1;
    for(int i = 0; i < CK_MAX && slot < 0; i++) if(!jar[i].used) slot = i;
    if(slot < 0){                                       /* full: the oldest goes */
        slot = 0;
        for(int i = 1; i < CK_MAX; i++) if(jar[i].born < jar[slot].born) slot = i;
        ck_drop(&jar[slot]);
    }
    c.value = kmalloc(ve - vs + 1);
    if(!c.value) return;
    memcpy(c.value, v + vs, ve - vs); c.value[ve - vs] = 0;
    c.used = 1;
    c.born = ++ck_born;
    jar[slot] = c;
}

/* The Cookie header value for a request to url ("a=1; b=2"). for_script: without HttpOnly. */
static void ck_header(const char *url, int for_script, char *out, u32 cap){
    out[0] = 0;
    url_t u;
    if(parse_url(url, &u) != 0) return;
    u64 now = ck_now();
    u32 k = 0;
    for(int i = 0; i < CK_MAX; i++){
        cookie_t *c = &jar[i];
        if(!c->used) continue;
        if(c->expires && c->expires <= now){ ck_drop(c); continue; }
        if(c->secure && !u.https) continue;
        if(for_script && c->http_only) continue;
        if(!ck_domain_ok(u.host, c->domain, c->host_only) || !ck_path_ok(u.path, c->path)) continue;
        u32 nl = ui_len(c->name), vl = ui_len(c->value);
        if(k + nl + vl + 4 >= cap) break;
        if(k){ out[k++] = ';'; out[k++] = ' '; }
        memcpy(out + k, c->name, nl); k += nl;
        out[k++] = '=';
        memcpy(out + k, c->value, vl); k += vl;
    }
    out[k] = 0;
}

/* Every Set-Cookie header in an answer's headers (hdr bytes at buf). */
static void ck_from_answer(const char *url, const u8 *buf, u32 hdr){
    static const char key[] = "set-cookie:";
    for(u32 i = 0; i + 11 < hdr; i++){
        if(i > 0 && buf[i - 1] != '\n') continue;
        u32 k = 0;
        while(k < 11 && ck_lower(buf[i + k]) == key[k]) k++;
        if(k < 11) continue;
        u32 p = i + 11;
        while(p < hdr && buf[p] == ' ') p++;
        u32 e = p;
        while(e < hdr && buf[e] != '\r' && buf[e] != '\n') e++;
        ck_set(url, (const char *)buf + p, e - p, 0);
    }
}

/* The request line's extra header: "Cookie: ...\r\n" (empty when there are none). */
static void ck_request_line(const char *url, char *out, u32 cap){
    static char v[4096];
    ck_header(url, 0, v, sizeof v);
    out[0] = 0;
    if(!v[0]) return;
    ui_copy(out, "Cookie: ", cap);
    cat_n(out, v, cap);
    cat_n(out, "\r\n", cap);
}
