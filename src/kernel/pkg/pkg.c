/* pkg.c - the Am-Droid package manager. See include/amdroid/pkg.h.
 *
 * A .adp package is a text container:
 *
 *   ADP1\n
 *   key=value\n            (header; keys: id,name,version,author,icon,desc,perms,entry)
 *   ...
 *   %%FILE <NAME> <LEN>\n   (one section per bundled file)
 *   <LEN raw bytes>\n
 *   ...
 *   %%END\n
 *
 * Installing writes each file to /APPS/<ID>/<NAME> and the header to
 * /APPS/<ID>/META so the app can be listed later. */
#include "amdroid/pkg.h"
#include "amdroid/heap.h"
#include "amdroid/fat32.h"
#include "amdroid/net.h"
#include "amdroid/console.h"
#include <string.h>

/* ---- small helpers ---- */

static int starts(const char *p, const char *pre){ return strncmp(p, pre, strlen(pre)) == 0; }

/* Copy src into dst (cap includes the NUL), trimming trailing spaces/CR. */
static void field_copy(char *dst, u32 cap, const char *src, u32 n){
    while(n && (src[n-1] == ' ' || src[n-1] == '\r')) n--;
    if(n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Join "a/b" or "a/b/c" (c may be NULL) into out. */
static void path_join(char *out, const char *a, const char *b, const char *c){
    int n = 0;
    while(*a) out[n++] = *a++;
    out[n++] = '/';
    while(*b) out[n++] = *b++;
    if(c){ out[n++] = '/'; while(*c) out[n++] = *c++; }
    out[n] = 0;
}

static void set_field(pkg_meta_t *m, const char *k, const char *v, u32 vn){
    if(!strcmp(k, "id"))            field_copy(m->id, sizeof m->id, v, vn);
    else if(!strcmp(k, "name"))     field_copy(m->name, sizeof m->name, v, vn);
    else if(!strcmp(k, "version"))  field_copy(m->version, sizeof m->version, v, vn);
    else if(!strcmp(k, "author"))   field_copy(m->author, sizeof m->author, v, vn);
    else if(!strcmp(k, "desc"))     field_copy(m->desc, sizeof m->desc, v, vn);
    else if(!strcmp(k, "perms"))    field_copy(m->perms, sizeof m->perms, v, vn);
    else if(!strcmp(k, "requires")) field_copy(m->requires, sizeof m->requires, v, vn);
    else if(!strcmp(k, "entry"))    field_copy(m->entry, sizeof m->entry, v, vn);
    else if(!strcmp(k, "icon")){
        int val = 0; const char *p = v; u32 i = 0;
        while(i < vn && p[i] >= '0' && p[i] <= '9'){ val = val * 10 + (p[i] - '0'); i++; }
        m->icon = val;
    }
}

/* ---- header parsing ---- */

int pkg_parse_header(const char *data, u32 len, pkg_meta_t *meta){
    memset(meta, 0, sizeof *meta);
    meta->icon = 9;                                   /* a sensible default tile */
    if(len < 5 || strncmp(data, "ADP1", 4) != 0) return -1;
    u32 i = 0;
    while(i < len && data[i] != '\n') i++;            /* skip the ADP1 line */
    i++;
    while(i < len){
        if(starts(data + i, "%%FILE") || starts(data + i, "%%END")) break;
        u32 j = i;
        while(j < len && data[j] != '\n') j++;
        const char *line = data + i;
        u32 n = j - i;
        /* split key=value */
        u32 eq = 0;
        while(eq < n && line[eq] != '=') eq++;
        if(eq < n){
            char key[16];
            u32 kn = eq; if(kn > sizeof key - 1) kn = sizeof key - 1;
            memcpy(key, line, kn); key[kn] = 0;
            set_field(meta, key, line + eq + 1, n - eq - 1);
        }
        i = j + 1;
    }
    if(!meta->id[0] || !meta->entry[0]) return -1;
    return 0;
}

/* Offset of the first "%%FILE" line, or len if there is none. */
static u32 files_offset(const char *data, u32 len){
    u32 i = 0;
    while(i < len){
        if(starts(data + i, "%%FILE") || starts(data + i, "%%END")) return i;
        while(i < len && data[i] != '\n') i++;
        i++;
    }
    return len;
}

/* ---- version compatibility ----
 * "requires" looks like "amdroidX[.Y][+]" or "amdroid.any". Rules:
 *   amdroid.any / any   -> any version
 *   amdroidX.Y+         -> this build must be >= X.Y
 *   amdroidX.Y          -> this build must be exactly X.Y (Y optional: match X.*)
 */
static int ci_eq(char a, char b){
    if(a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if(b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}
static int has_sub(const char *s, const char *sub){
    for(; *s; s++){
        const char *a = s, *b = sub;
        while(*a && *b && ci_eq(*a, *b)){ a++; b++; }
        if(!*b) return 1;
    }
    return 0;
}

/* Returns 1 if this build satisfies req; writes a short reason into why. */
static int version_ok(const char *req, char *why, u32 cap){
    if(!req || !req[0]){
        field_copy(why, cap, "missing 'requires' field", 24);
        return 0;
    }
    if(has_sub(req, "any")){ if(cap) why[0] = 0; return 1; }
    const char *p = req;
    /* skip an optional "amdroid" prefix */
    if(has_sub(req, "amdroid")){
        const char *q = req;
        while(*q && !(*q >= '0' && *q <= '9')) q++;
        p = q;
    }
    int major = 0, minor = 0, have_minor = 0, plus = 0;
    while(*p >= '0' && *p <= '9') major = major * 10 + (*p++ - '0');
    if(*p == '.'){ p++; have_minor = 1; while(*p >= '0' && *p <= '9') minor = minor * 10 + (*p++ - '0'); }
    while(*p){ if(*p == '+') plus = 1; p++; }

    int cur = AMD_VER_MAJOR * 100 + AMD_VER_MINOR;
    int need = major * 100 + minor;
    int ok;
    if(plus) ok = cur >= need;
    else if(have_minor) ok = cur == need;
    else ok = AMD_VER_MAJOR == major;      /* "amdroid2" -> any 2.x */
    if(!ok){
        /* "needs Am-Droid <req> (this is 1.0)" */
        char buf[64]; u32 n = 0;
        const char *a = "needs Am-Droid ";
        while(*a && n < sizeof buf - 1) buf[n++] = *a++;
        for(const char *r = req; *r && n < sizeof buf - 10; r++) buf[n++] = *r;
        const char *b = " (have ";
        while(*b && n < sizeof buf - 6) buf[n++] = *b++;
        buf[n++] = (char)('0' + AMD_VER_MAJOR); buf[n++] = '.';
        buf[n++] = (char)('0' + AMD_VER_MINOR); buf[n++] = ')';
        buf[n] = 0;
        field_copy(why, cap, buf, n);
    } else if(cap) why[0] = 0;
    return ok;
}

/* ---- security scan ----
 * Flags dangerous Fuss builtins that the manifest does not declare a matching
 * permission for (hidden capability), and access to sensitive system paths. */
static int word_in(const char *hay, u32 n, const char *word){
    u32 wl = (u32)strlen(word);
    for(u32 i = 0; i + wl <= n; i++){
        if(strncmp(hay + i, word, wl) != 0) continue;
        char before = i ? hay[i-1] : ' ';
        char after  = hay[i+wl];
        int bw = (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') || (before >= '0' && before <= '9') || before == '_';
        int aw = (after  >= 'a' && after  <= 'z') || (after  >= 'A' && after  <= 'Z') || (after  >= '0' && after  <= '9') || after  == '_';
        if(!bw && !aw) return 1;           /* whole-word match */
    }
    return 0;
}

static void finding_add(pkg_report_t *rep, const char *msg){
    u32 cur = (u32)strlen(rep->findings);
    u32 ml = (u32)strlen(msg);
    if(cur + ml + 2 >= sizeof rep->findings) return;
    if(cur){ rep->findings[cur++] = '\n'; }
    memcpy(rep->findings + cur, msg, ml);
    rep->findings[cur + ml] = 0;
    rep->nfindings++;
}

/* Scans one bundled file's body for dangerous operations. */
static void scan_body(pkg_report_t *rep, const pkg_meta_t *m, const char *body, u32 n){
    int decl_storage = has_sub(m->perms, "storage");
    int decl_network = has_sub(m->perms, "network");
    static const char *wr[] = { "write_file", "delete_file", "mkdir_p", "write_ppm" };
    for(int i = 0; i < 4; i++){
        if(word_in(body, n, wr[i])){
            rep->dangerous = 1;
            if(!decl_storage){
                char msg[80] = "uses ";
                field_copy(msg + 5, sizeof msg - 5, wr[i], (u32)strlen(wr[i]));
                const char *tail = " without the storage permission";
                u32 c = (u32)strlen(msg);
                for(const char *t = tail; *t && c < sizeof msg - 1; t++) msg[c++] = *t;
                msg[c] = 0;
                finding_add(rep, msg);
                rep->blocking = 1;
            }
        }
    }
    /* sensitive paths: other apps, the user database, raw devices */
    static const char *bad[] = { "USERS", "PASSWD", "SHADOW", "/APPS/", "BOOT" };
    static const char *badmsg[] = {
        "touches the USERS area", "touches PASSWD", "touches SHADOW",
        "writes into another app's folder", "touches BOOT files" };
    for(int i = 0; i < 5; i++){
        if(word_in(body, n, bad[i]) || has_sub(body, bad[i])){
            finding_add(rep, badmsg[i]);
            rep->blocking = 1;
            rep->dangerous = 1;
        }
    }
    if(word_in(body, n, "tcp_connect") || word_in(body, n, "net_")){
        rep->dangerous = 1;
        if(!decl_network){ finding_add(rep, "uses the network without the network permission"); rep->blocking = 1; }
    }
}

/* Walks the %%FILE sections, validating structure and (optionally) scanning
 * each body. Returns 0 if well-formed, -1 with err set otherwise. */
static int walk_files(const char *data, u32 len, const pkg_meta_t *m,
                      pkg_report_t *rep, int *entry_seen, char *err, u32 errcap){
    int saw_end = 0;
    *entry_seen = 0;
    u32 i = files_offset(data, len);
    while(i < len){
        if(starts(data + i, "%%END")){ saw_end = 1; break; }
        if(!starts(data + i, "%%FILE")){
            while(i < len && data[i] != '\n') i++;
            i++;
            continue;
        }
        u32 j = i;
        while(j < len && data[j] != '\n') j++;
        const char *line = data + i + 7;
        const char *lend = data + j;
        char name[16]; int nn = 0;
        while(line < lend && *line == ' ') line++;
        while(line < lend && *line != ' ' && nn < (int)sizeof name - 1) name[nn++] = *line++;
        name[nn] = 0;
        while(line < lend && *line == ' ') line++;
        int have_len = (line < lend && *line >= '0' && *line <= '9');
        u32 flen = 0;
        while(line < lend && *line >= '0' && *line <= '9') flen = flen * 10 + (u32)(*line++ - '0');
        if(!nn || !have_len){ field_copy(err, errcap, "corrupt package (bad file header)", 33); return -1; }
        u32 body = j + 1;
        if(body + flen > len){ field_copy(err, errcap, "package is truncated", 20); return -1; }
        if(strcmp(name, m->entry) == 0) *entry_seen = 1;
        if(rep) scan_body(rep, m, data + body, flen);
        i = body + flen;
        if(i < len && data[i] == '\n') i++;
    }
    if(!saw_end){ field_copy(err, errcap, "package is incomplete (no end marker)", 37); return -1; }
    return 0;
}

int pkg_check(const char *data, u32 len, pkg_meta_t *meta, pkg_report_t *rep){
    memset(rep, 0, sizeof *rep);
    if(pkg_parse_header(data, len, meta) != 0){
        field_copy(rep->error, sizeof rep->error, "not a valid .adp package (bad header)", 37);
        rep->blocking = 1;
        return -1;
    }
    /* structural walk + security scan */
    int entry_seen = 0;
    if(walk_files(data, len, meta, rep, &entry_seen, rep->error, sizeof rep->error) != 0){
        rep->blocking = 1;
        return -1;
    }
    if(!entry_seen){
        field_copy(rep->error, sizeof rep->error, "entry program is missing from the package", 41);
        rep->blocking = 1;
        return -1;
    }
    /* version compatibility */
    char why[64];
    rep->version_ok = version_ok(meta->requires, why, sizeof why);
    if(!rep->version_ok){
        field_copy(rep->error, sizeof rep->error, why, (u32)strlen(why));
        rep->blocking = 1;
        return -1;
    }
    if(has_sub(meta->perms, "storage") || has_sub(meta->perms, "network") || has_sub(meta->perms, "system"))
        rep->dangerous = 1;
    rep->ok = !rep->blocking;
    return rep->blocking ? -1 : 0;
}

/* ---- install ---- */

int pkg_install(const char *data, u32 len, pkg_meta_t *out, char *err, u32 errcap){
    if(err && errcap) err[0] = 0;
    pkg_meta_t m;
    pkg_report_t rep;
    if(pkg_check(data, len, &m, &rep) != 0){
        if(err) field_copy(err, errcap, rep.error[0] ? rep.error : "package blocked by security scan",
                           (u32)strlen(rep.error[0] ? rep.error : "package blocked by security scan"));
        return -1;
    }

    char dir[8 + PKG_ID_MAX];                          /* "APPS/<ID>" */
    path_join(dir, "APPS", m.id, 0);
    fat32_mkdir("APPS");                               /* ok if it exists already */
    fat32_mkdir(dir);

    /* write each %%FILE section */
    u32 i = files_offset(data, len);
    while(i < len){
        if(starts(data + i, "%%END")) break;
        if(!starts(data + i, "%%FILE")){
            while(i < len && data[i] != '\n') i++;
            i++;
            continue;
        }
        /* parse "%%FILE <NAME> <LEN>" */
        u32 j = i;
        while(j < len && data[j] != '\n') j++;
        const char *line = data + i + 7;               /* past "%%FILE " */
        const char *lend = data + j;
        char name[16]; int nn = 0;
        while(line < lend && *line == ' ') line++;
        while(line < lend && *line != ' ' && nn < (int)sizeof name - 1) name[nn++] = *line++;
        name[nn] = 0;
        while(line < lend && *line == ' ') line++;
        u32 flen = 0;
        while(line < lend && *line >= '0' && *line <= '9') flen = flen * 10 + (u32)(*line++ - '0');

        u32 body = j + 1;
        if(body + flen > len) return -1;               /* truncated package */

        char fpath[8 + PKG_ID_MAX + 16];
        path_join(fpath, "APPS", m.id, name);
        if(fat32_write_path(fpath, data + body, flen) != 0) return -1;

        i = body + flen;
        if(i < len && data[i] == '\n') i++;            /* trailing newline */
    }

    /* write the header block as META for later listing */
    char meta_path[8 + PKG_ID_MAX + 8];
    path_join(meta_path, "APPS", m.id, "META");
    u32 hlen = files_offset(data, len);
    if(fat32_write_path(meta_path, data, hlen) != 0) return -1;

    if(out) *out = m;
    return 0;
}

int pkg_install_file(const char *fat_path, pkg_meta_t *out, char *err, u32 errcap){
    if(err && errcap) err[0] = 0;
    u32 size = 0;
    if(fat32_read_path(fat_path, 0, 0, &size) != 0){
        if(err) field_copy(err, errcap, "cannot read the package file", 28);
        return -1;
    }
    char *buf = kmalloc(size + 1);
    if(!buf){ if(err) field_copy(err, errcap, "out of memory", 13); return -1; }
    u32 got = 0;
    int rc = -1;
    if(fat32_read_path(fat_path, buf, size, &got) == 0){
        buf[got] = 0;
        rc = pkg_install(buf, got, out, err, errcap);
    } else if(err) field_copy(err, errcap, "cannot read the package file", 28);
    kfree(buf);
    return rc;
}

/* ---- HTTP download (a small GET, headers stripped) ---- */

static int http_get(u32 ip, u16 port, const char *path, u8 **body, u32 *blen){
    int sock = tcp_connect(ip, port);
    if(sock < 0) return -1;
    char req[300];
    int n = 0;
    const char *h = "GET ";
    for(const char *s = h; *s; s++) req[n++] = *s;
    for(const char *s = path; *s && n < 250; s++) req[n++] = *s;
    const char *tail = " HTTP/1.0\r\n\r\n";
    for(const char *s = tail; *s; s++) req[n++] = *s;
    if(tcp_send(sock, req, (u32)n) != 0){ tcp_close(sock); return -1; }
    u32 cap = 512 * 1024, len = 0;
    u8 *buf = kmalloc(cap);
    if(!buf){ tcp_close(sock); return -1; }
    for(;;){
        if(len + 1024 > cap){ tcp_close(sock); kfree(buf); return -1; }
        int got = tcp_recv(sock, buf + len, 1024, 3000);
        if(got <= 0) break;
        len += (u32)got;
    }
    tcp_close(sock);
    u32 start = 0;
    for(u32 i = 0; i + 3 < len; i++)
        if(buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n'){ start = i + 4; break; }
    if(start == 0 || buf[9] != '2'){ kfree(buf); return -1; }
    memmove(buf, buf + start, len - start);
    *body = buf;
    *blen = len - start;
    return 0;
}

int pkg_install_url(const char *host, u16 port, const char *path, pkg_meta_t *out, char *err, u32 errcap){
    if(err && errcap) err[0] = 0;
    if(!net_up()){ if(err) field_copy(err, errcap, "no network connection", 21); return -1; }
    u32 ip = 0;
    if(net_resolve(host, &ip, 0, 0) != 0){ if(err) field_copy(err, errcap, "could not resolve host", 22); return -1; }
    u8 *body = 0; u32 blen = 0;
    if(http_get(ip, port ? port : 80, path, &body, &blen) != 0){ if(err) field_copy(err, errcap, "download failed", 15); return -1; }
    int rc = pkg_install((const char *)body, blen, out, err, errcap);
    kfree(body);
    return rc;
}

/* ---- listing ---- */

static pkg_meta_t *g_list;
static int g_list_n, g_list_max;

static void list_cb(const char *name, int is_dir, u32 size){
    (void)size;
    if(!is_dir || g_list_n >= g_list_max) return;
    char meta_path[8 + PKG_ID_MAX + 8];
    path_join(meta_path, "APPS", name, "META");
    u32 sz = 0;
    if(fat32_read_path(meta_path, 0, 0, &sz) != 0) return;
    char *buf = kmalloc(sz + 1);
    if(!buf) return;
    u32 got = 0;
    if(fat32_read_path(meta_path, buf, sz, &got) == 0){
        buf[got] = 0;
        pkg_meta_t m;
        if(pkg_parse_header(buf, got, &m) == 0) g_list[g_list_n++] = m;
    }
    kfree(buf);
}

int pkg_list(pkg_meta_t *out, int max){
    g_list = out; g_list_n = 0; g_list_max = max;
    fat32_list_path("APPS", list_cb);                 /* missing dir -> 0 apps */
    return g_list_n;
}

static char (*g_av)[16];
static int g_av_n, g_av_max;

static void avail_cb(const char *name, int is_dir, u32 size){
    (void)size;
    if(is_dir || g_av_n >= g_av_max) return;
    int L = (int)strlen(name);
    if(L > 4 && strcmp(name + L - 4, ".ADP") == 0){
        u32 c = L < 15 ? (u32)L : 15;
        memcpy(g_av[g_av_n], name, c);
        g_av[g_av_n][c] = 0;
        g_av_n++;
    }
}

int pkg_available(char names[][16], int max){
    g_av = names; g_av_n = 0; g_av_max = max;
    fat32_list_path("", avail_cb);
    return g_av_n;
}

/* ---- entry path / installed / remove ---- */

int pkg_entry_path(const char *id, char *out, u32 n){
    char meta_path[8 + PKG_ID_MAX + 8];
    path_join(meta_path, "APPS", id, "META");
    u32 sz = 0;
    if(fat32_read_path(meta_path, 0, 0, &sz) != 0) return -1;
    char *buf = kmalloc(sz + 1);
    if(!buf) return -1;
    u32 got = 0;
    pkg_meta_t m;
    int rc = -1;
    if(fat32_read_path(meta_path, buf, sz, &got) == 0){
        buf[got] = 0;
        if(pkg_parse_header(buf, got, &m) == 0){
            char p[8 + PKG_ID_MAX + 16];
            path_join(p, "APPS", id, m.entry);
            u32 pl = (u32)strlen(p);
            if(pl < n){ memcpy(out, p, pl + 1); rc = 0; }
        }
    }
    kfree(buf);
    return rc;
}

int pkg_installed(const char *id){
    char meta_path[8 + PKG_ID_MAX + 8];
    path_join(meta_path, "APPS", id, "META");
    u32 sz = 0;
    return fat32_read_path(meta_path, 0, 0, &sz) == 0;
}

/* Collect the files inside /APPS/<ID> first, then delete them (deleting during
 * a directory scan would disturb it). */
static char g_rm_files[16][16];
static int g_rm_n;
static void rm_cb(const char *name, int is_dir, u32 size){
    (void)is_dir; (void)size;
    if(g_rm_n >= 16) return;
    u32 c = (u32)strlen(name); if(c > 15) c = 15;
    memcpy(g_rm_files[g_rm_n], name, c);
    g_rm_files[g_rm_n][c] = 0;
    g_rm_n++;
}

int pkg_remove(const char *id){
    char base[8 + PKG_ID_MAX];
    path_join(base, "APPS", id, 0);
    g_rm_n = 0;
    fat32_list_path(base, rm_cb);
    for(int i = 0; i < g_rm_n; i++){
        char fpath[8 + PKG_ID_MAX + 16];
        path_join(fpath, "APPS", id, g_rm_files[i]);
        fat32_delete_path(fpath);
    }
    fat32_delete_path(base);
    return 0;
}
