/* auth.c — accounts (USERS.DB) and file permissions (PERMS.DB).
 *
 * USERS.DB lines:  name uid saltHex hashHex
 * PERMS.DB lines:  FILE owner modeOctal
 * Files without a PERMS.DB line belong to root with mode 0644, except the two
 * database files, which are root-only (0600). */
#include "amdroid/auth.h"
#include "amdroid/fat32.h"
#include "amdroid/heap.h"
#include "amdroid/console.h"
#include "amdroid/keyboard.h"
#include "amdroid/serial.h"
#include "amdroid/sha256.h"
#include "amdroid/sched.h"
#include "amdroid/timer.h"
#include "amdroid/types.h"
#include <string.h>

#define MAX_USERS 16
#define MAX_PERMS 128
#define DB_MAX 8192
#define ROUNDS 1000

typedef struct { char name[16]; u32 uid; u8 salt[16]; u8 hash[32]; int kdf; int used; } user_t;
typedef struct { char name[16]; u32 owner; u32 mode; int used; } perm_t;

static user_t users[MAX_USERS];
static perm_t perms[MAX_PERMS];
static int current = -1;          /* index into users[]; -1 = nobody logged in */
static int perms_loaded = 0;

/* ---------- text helpers ---------- */
static void copy(char *d, const char *s, int max){
    int k = 0;
    while(s[k] && k < max - 1){ d[k] = s[k]; k++; }
    d[k] = 0;
}
static u32 parse_u32(const char **p){
    u32 v = 0;
    while(**p >= '0' && **p <= '9'){ v = v * 10 + (u32)(**p - '0'); (*p)++; }
    return v;
}
static void put_hex(char *d, const u8 *s, int n){
    static const char H[] = "0123456789abcdef";
    for(int i = 0; i < n; i++){ d[2*i] = H[s[i] >> 4]; d[2*i+1] = H[s[i] & 15]; }
    d[2*n] = 0;
}
static int hexval(char c){
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int get_hex(const char **p, u8 *out, int n){
    for(int i = 0; i < n; i++){
        int hi = hexval((*p)[0]), lo = hexval((*p)[1]);
        if(hi < 0 || lo < 0) return -1;
        out[i] = (u8)(hi * 16 + lo);
        *p += 2;
    }
    return 0;
}
static int upper_eq(const char *a, const char *b){
    for(;;){
        char x = *a, y = *b;
        if(x >= 'a' && x <= 'z') x = (char)(x - 32);
        if(y >= 'a' && y <= 'z') y = (char)(y - 32);
        if(x != y) return 0;
        if(!x) return 1;
        a++; b++;
    }
}
static void upper_copy(char *d, const char *s, int max){
    int k = 0;
    while(s[k] && k < max - 1){ char c = s[k]; d[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; k++; }
    d[k] = 0;
}

/* salt material: the tick counter and a constant, mixed */
static u64 salt_seed(void){
    return 0x9E3779B97F4A7C15ULL ^ (timer_ticks() * 0x2545F4914F6CDD1DULL);
}

/* ---------- password hashing: iterated SHA-256 over salt and password ---------- */
#define KDF_LEGACY 1      /* iterated SHA-256 (accounts made before PBKDF2) */
#define KDF_PBKDF2 2      /* PBKDF2-HMAC-SHA256, the current scheme */
#define PBKDF2_ROUNDS 4096

static void derive_kdf(int kdf, const u8 salt[16], const char *pass, u8 out[32]){
    int pl = (int)strlen(pass);
    if(pl > 64) pl = 64;
    if(kdf == KDF_PBKDF2){
        pbkdf2_sha256(pass, (u32)pl, salt, 16, PBKDF2_ROUNDS, out, 32);
        return;
    }
    u8 buf[16 + 64 + 32];
    memcpy(buf, salt, 16);
    memcpy(buf + 16, pass, (size_t)pl);
    sha256(buf, 16 + (u32)pl, out);
    for(int r = 0; r < ROUNDS; r++){
        memcpy(buf, out, 32);
        memcpy(buf + 32, salt, 16);
        memcpy(buf + 48, pass, (size_t)pl);
        sha256(buf, 48 + (u32)pl, out);
    }
}

static void derive(const u8 salt[16], const char *pass, u8 out[32]){
    derive_kdf(KDF_PBKDF2, salt, pass, out);
}

/* ---------- reading and writing the databases ---------- */
static char *read_db(const char *name, u32 *len){
    u32 size = 0;
    if(fat32_read_path(name, NULL, 0, &size) != 0) return NULL;
    if(size > DB_MAX) return NULL;
    char *db = kmalloc(size + 1);
    if(!db) return NULL;
    u32 got = 0;
    if(fat32_read_path(name, db, size, &got) != 0){ kfree(db); return NULL; }
    db[got] = 0;
    if(len) *len = got;
    return db;
}

static int count_users(void){
    int n = 0;
    for(int i = 0; i < MAX_USERS; i++) if(users[i].used) n++;
    return n;
}

static void load_users(void){
    char *db = read_db("USERS.DB", NULL);
    if(!db) return;
    const char *p = db;
    while(*p){
        char name[16];
        int k = 0;
        while(*p && *p != ' ' && k < 15) name[k++] = *p++;
        name[k] = 0;
        if(*p != ' ') break;
        p++;
        u32 uid = parse_u32(&p);
        if(*p != ' ') break;
        p++;
        u8 salt[16], hash[32];
        if(get_hex(&p, salt, 16) != 0 || *p != ' ') break;
        p++;
        if(get_hex(&p, hash, 32) != 0) break;
        int kdf = KDF_LEGACY;                    /* lines without a kdf field are old */
        if(*p == ' '){ p++; kdf = (int)parse_u32(&p); }
        while(*p == '\n' || *p == '\r') p++;
        for(int i = 0; i < MAX_USERS; i++){
            if(users[i].used) continue;
            copy(users[i].name, name, sizeof users[i].name);
            users[i].uid = uid;
            memcpy(users[i].salt, salt, 16);
            memcpy(users[i].hash, hash, 32);
            users[i].kdf = kdf;
            users[i].used = 1;
            break;
        }
    }
    kfree(db);
}

static void load_perms(void){
    perms_loaded = 1;
    char *db = read_db("PERMS.DB", NULL);
    if(!db) return;
    const char *p = db;
    while(*p){
        char name[16];
        int k = 0;
        while(*p && *p != ' ' && k < 15) name[k++] = *p++;
        name[k] = 0;
        if(*p != ' ') break;
        p++;
        u32 owner = parse_u32(&p);
        if(*p != ' ') break;
        p++;
        u32 mode = 0;
        while(*p >= '0' && *p <= '7'){ mode = mode * 8 + (u32)(*p - '0'); p++; }
        while(*p == '\n' || *p == '\r') p++;
        for(int i = 0; i < MAX_PERMS; i++){
            if(perms[i].used) continue;
            copy(perms[i].name, name, sizeof perms[i].name);
            perms[i].owner = owner;
            perms[i].mode = mode;
            perms[i].used = 1;
            break;
        }
    }
    kfree(db);
}

static int save_users(void){
    char *out = kmalloc(MAX_USERS * 96 + 1);
    if(!out) return -1;
    int n = 0;
    for(int i = 0; i < MAX_USERS; i++){
        if(!users[i].used) continue;
        char hs[33], hh[65];
        put_hex(hs, users[i].salt, 16);
        put_hex(hh, users[i].hash, 32);
        char kb[4]; kb[0] = (char)('0' + users[i].kdf); kb[1] = 0;
        int k = 0;
        for(const char *s = users[i].name; *s; s++) out[n++] = *s;
        out[n++] = ' ';
        char num[12]; int nl = 0; u32 v = users[i].uid;
        do { num[nl++] = (char)('0' + v % 10); v /= 10; } while(v);
        while(nl) out[n++] = num[--nl];
        out[n++] = ' ';
        for(k = 0; hs[k]; k++) out[n++] = hs[k];
        out[n++] = ' ';
        for(k = 0; hh[k]; k++) out[n++] = hh[k];
        out[n++] = ' ';
        out[n++] = kb[0];
        out[n++] = '\n';
    }
    int rc = fat32_write_file("USERS.DB", out, (u32)n);
    kfree(out);
    return rc;
}

static int save_perms(void){
    char *out = kmalloc(MAX_PERMS * 40 + 1);
    if(!out) return -1;
    int n = 0;
    for(int i = 0; i < MAX_PERMS; i++){
        if(!perms[i].used) continue;
        for(const char *s = perms[i].name; *s; s++) out[n++] = *s;
        out[n++] = ' ';
        char num[12]; int nl = 0; u32 v = perms[i].owner;
        do { num[nl++] = (char)('0' + v % 10); v /= 10; } while(v);
        while(nl) out[n++] = num[--nl];
        out[n++] = ' ';
        u32 m = perms[i].mode;
        out[n++] = (char)('0' + ((m >> 6) & 7));
        out[n++] = (char)('0' + ((m >> 3) & 7));
        out[n++] = (char)('0' + (m & 7));
        out[n++] = '\n';
    }
    int rc = fat32_write_file("PERMS.DB", out, (u32)n);
    kfree(out);
    return rc;
}

/* ---------- permission table ---------- */
static perm_t *find_perm(const char *path){
    for(int i = 0; i < MAX_PERMS; i++)
        if(perms[i].used && upper_eq(perms[i].name, path)) return &perms[i];
    return NULL;
}

static perm_t *ensure_perm(const char *path, u32 owner, u32 mode){
    perm_t *p = find_perm(path);
    if(p) return p;
    for(int i = 0; i < MAX_PERMS; i++){
        if(perms[i].used) continue;
        upper_copy(perms[i].name, path, sizeof perms[i].name);
        perms[i].owner = owner;
        perms[i].mode = mode;
        perms[i].used = 1;
        return &perms[i];
    }
    return NULL;
}

static int is_system_file(const char *path){
    return upper_eq(path, "USERS.DB") || upper_eq(path, "PERMS.DB");
}

void auth_logout(void){ current = -1; proc_set_uid(0, -1); }

/* Signs in without a password. Only the graphical login screen calls this; the
 * text shell still asks for one. */
int auth_login_nopass(const char *name){
    for(int i = 0; i < MAX_USERS; i++){
        if(!users[i].used || !upper_eq(users[i].name, name)) continue;
        current = i;
        proc_set_uid(0, (int)users[i].uid);
        return 0;
    }
    return -1;
}

int auth_user_count(void){
    int n = 0;
    for(int i = 0; i < MAX_USERS; i++) if(users[i].used) n++;
    return n;
}

const char *auth_user_at(int i, int *uid){
    for(int k = 0; k < MAX_USERS; k++){
        if(!users[k].used) continue;
        if(i-- == 0){
            if(uid) *uid = (int)users[k].uid;
            return users[k].name;
        }
    }
    return NULL;
}

/* the current user's uid, or -1 when nobody is logged in */
int auth_uid(void){ return current < 0 ? -1 : (int)users[current].uid; }
const char *auth_name(void){ return current < 0 ? "nobody" : users[current].name; }

/* ---------- public API ---------- */

static void first_boot_setup(void){
    /* No persistent disk, so accounts can't survive a reboot: auto-create a
     * passwordless root instead of prompting, and let the GUI log straight in.
     * (Set a password later with `passwd` to require a login.) */
    int i = 0;
    for(; i < MAX_USERS; i++) if(!users[i].used) break;
    copy(users[i].name, "root", sizeof users[i].name);
    users[i].uid = 0;
    u64 t = salt_seed();
    for(int k = 0; k < 16; k++){ users[i].salt[k] = (u8)(t >> ((k % 8) * 8)); t = t * 6364136223846793005ULL + 1; }
    derive(users[i].salt, "", users[i].hash);        /* empty password */
    users[i].kdf = KDF_PBKDF2;
    users[i].used = 1;
    save_users();
    ensure_perm("USERS.DB", 0, 0600);
    ensure_perm("PERMS.DB", 0, 0600);
    save_perms();
    console_puts("Created a passwordless root account (set one with 'passwd').\n");
}

void auth_init(void){
    load_users();
    load_perms();
    if(count_users() == 0) first_boot_setup();
}

int auth_login(const char *name, const char *pass){
    for(int i = 0; i < MAX_USERS; i++){
        if(!users[i].used || !upper_eq(users[i].name, name)) continue;
        u8 h[32];
        derive_kdf(users[i].kdf, users[i].salt, pass, h);
        if(memcmp(h, users[i].hash, 32) != 0) return -1;
        if(users[i].kdf != KDF_PBKDF2){
            /* upgrade an old hash to the current scheme on a good login */
            derive(users[i].salt, pass, users[i].hash);
            users[i].kdf = KDF_PBKDF2;
            save_users();
        }
        current = i;
        proc_set_uid(0, (int)users[i].uid);    /* the shell now runs as this user */
        return 0;
    }
    return -1;
}

int auth_adduser(const char *name, const char *pass){
    if(auth_uid() != 0) return -1;
    if(!name[0] || strlen(name) > 15) return -1;
    for(int i = 0; i < MAX_USERS; i++) if(users[i].used && upper_eq(users[i].name, name)) return -1;
    int slot = -1;
    u32 next = 1;
    for(int i = 0; i < MAX_USERS; i++){
        if(!users[i].used){ if(slot < 0) slot = i; continue; }
        if(users[i].uid >= next) next = users[i].uid + 1;
    }
    if(slot < 0) return -1;
    copy(users[slot].name, name, sizeof users[slot].name);
    users[slot].uid = next;
    u64 t = salt_seed();
    for(int k = 0; k < 16; k++){ users[slot].salt[k] = (u8)(t >> ((k % 8) * 8)); t = t * 6364136223846793005ULL + 1; }
    derive(users[slot].salt, pass, users[slot].hash);
    users[slot].kdf = KDF_PBKDF2;
    users[slot].used = 1;
    return save_users();
}

int auth_setpass(const char *name, const char *pass){
    int me = current;
    for(int i = 0; i < MAX_USERS; i++){
        if(!users[i].used || !upper_eq(users[i].name, name)) continue;
        if(auth_uid() != 0 && me != i) return -1;
        derive(users[i].salt, pass, users[i].hash);
        users[i].kdf = KDF_PBKDF2;
        return save_users();
    }
    return -1;
}

int auth_may_as(int uid, const char *path, int write){
    if(!perms_loaded) load_perms();
    if(uid == 0) return 1;
    if(uid < 0) return 0;
    u32 owner = 0, mode = 0644;
    if(is_system_file(path)){ owner = 0; mode = 0600; }
    else {
        perm_t *p = find_perm(path);
        if(p){ owner = p->owner; mode = p->mode; }
    }
    int bit = write ? 2 : 4;
    if((u32)uid == owner) return (mode >> 6) & bit ? 1 : 0;
    return (mode & bit) ? 1 : 0;
}

int auth_may(const char *path, int write){
    return auth_may_as(auth_uid(), path, write);
}

int auth_chmod(const char *path, int mode){
    if(!perms_loaded) load_perms();
    int uid = auth_uid();
    u32 owner = 0;
    if(is_system_file(path)) return -1;
    perm_t *p = find_perm(path);
    owner = p ? p->owner : 0;
    if(uid != 0 && (u32)uid != owner) return -1;
    p = ensure_perm(path, owner, (u32)mode & 0777);
    if(!p) return -1;
    p->mode = (u32)mode & 0777;
    return save_perms();
}

void auth_claim_as(int uid, const char *path){
    if(!perms_loaded) load_perms();
    if(find_perm(path)) return;
    ensure_perm(path, uid < 0 ? 0 : (u32)uid, 0644);
    save_perms();
}

void auth_claim(const char *path){ auth_claim_as(auth_uid(), path); }

int auth_mode_of(const char *path, int *owner){
    perm_t *p = find_perm(path);
    if(owner) *owner = p ? (int)p->owner : 0;
    return p ? (int)p->mode : 0644;
}

/* ---------- console input for passwords ---------- */
static int readkey(void){
    for(;;){
        if(serial_has_input()) return serial_getc();
        if(keyboard_has_input()) return keyboard_getchar();
        proc_yield();
    }
}

void auth_read_line(char *buf, int max){
    int n = 0;
    for(;;){
        int c = readkey();
        if(c == '\r' || c == '\n'){ console_putc('\n'); break; }
        if((c == '\b' || c == 127) && n > 0){ n--; console_puts("\b \b"); continue; }
        if(c >= 32 && c < 127 && n < max - 1){ buf[n++] = (char)c; console_putc((char)c); }
    }
    buf[n] = 0;
}

void auth_read_secret(char *buf, int max){
    int n = 0;
    for(;;){
        int c = readkey();
        if(c == '\r' || c == '\n'){ console_putc('\n'); break; }
        if((c == '\b' || c == 127) && n > 0){ n--; continue; }
        if(c >= 32 && c < 127 && n < max - 1){ buf[n++] = (char)c; }
    }
    buf[n] = 0;
}
