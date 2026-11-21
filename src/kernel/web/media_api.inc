/* media_api.inc — the native side of fetch(), MSE and <video> (media_api.js has the
 * JavaScript objects). Included by jsdom.c, so it builds wherever the engine does.
 *
 * Nothing here touches the network or a decoder: requests and media commands are queued,
 * and Husser's main loop hands them to the browser app through the shared region
 * (husser_proto.h). Answers come back through web_fetch_done and web_media_state. */
#include "media_api_js.inc"
#include "amdroid/husser_proto.h"        /* HUSSER_MEDIA_BYTES: how much one hand-over carries */

/* ---- fetch() requests ---- */
#define WF_MAX 32
static struct { u32 id; char *url; char method[12]; char ctype[128]; u8 *body; u32 blen; } wf[WF_MAX];
static int nwf = 0;
static u32 wf_next = 1;                 /* ids keep rising across pages (the browser relies on it) */
static int wf_changed = 0;

/* M.fetch(url, method, body, contentType): queues a request, returns its id (0: refused) */
static js_val_t m_fetch(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    const char *u = argc ? js_to_str(argv[0]) : "";
    u32 n = (u32)strlen(u);
    if(nwf >= WF_MAX || n == 0 || n >= 4000) return js_num(0);
    /* the body: a string (UTF-8) or bytes */
    const u8 *raw = NULL; int bl = 0;
    if(argc > 2 && argv[2].t == JS_STR){ raw = (const u8 *)js_to_str(argv[2]); bl = (int)strlen((const char *)raw); }
    else if(argc > 2 && argv[2].t == JS_OBJ) bl = js_bytes_of(argv[2], &raw, NULL, 0);
    if(bl < 0) bl = 0;
    if((u32)bl > HUSSER_FETCH_BODY) return js_num(0);          /* too big for one hand-over */
    u8 *body = NULL;
    if(bl){
        body = kmalloc((u32)bl);
        if(!body) return js_num(0);
        if(raw) memcpy(body, raw, (u32)bl); else js_bytes_of(argv[2], &raw, body, (u32)bl);
    }
    /* a relative address is resolved by the browser against the page */
    char *c = kmalloc(n + 1);
    if(!c){ if(body) kfree(body); return js_num(0); }
    memcpy(c, u, n + 1);
    wf[nwf].id = wf_next++;
    wf[nwf].url = c;
    const char *m = argc > 1 && argv[1].t == JS_STR ? js_to_str(argv[1]) : "GET";
    u32 k = 0;
    for(; m[k] && k + 1 < sizeof wf[nwf].method; k++) wf[nwf].method[k] = (char)(m[k] >= 'a' && m[k] <= 'z' ? m[k] - 32 : m[k]);
    wf[nwf].method[k] = 0;
    const char *ct = argc > 3 && argv[3].t == JS_STR ? js_to_str(argv[3]) : "";
    for(k = 0; ct[k] && ct[k] != '\r' && ct[k] != '\n' && k + 1 < sizeof wf[nwf].ctype; k++) wf[nwf].ctype[k] = ct[k];
    wf[nwf].ctype[k] = 0;
    wf[nwf].body = body; wf[nwf].blen = (u32)bl;
    nwf++;
    wf_changed = 1;
    return js_num(wf[nwf - 1].id);
}

int web_fetch_requests(web_fetch_req_t *out, int max, int *changed){
    *changed = wf_changed;
    wf_changed = 0;
    int k = 0;
    for(int i = 0; i < nwf && k < max; i++, k++)
        out[k] = (web_fetch_req_t){ wf[i].id, wf[i].url, wf[i].method, wf[i].ctype, wf[i].body, wf[i].blen };
    return k;
}

void web_fetch_done(u32 id, u32 status, const u8 *data, u32 len, int failed){
    int at = -1;
    for(int i = 0; i < nwf; i++) if(wf[i].id == id){ at = i; break; }
    if(at < 0) return;                  /* from an earlier page */
    kfree(wf[at].url);
    if(wf[at].body) kfree(wf[at].body);
    for(int i = at + 1; i < nwf; i++) wf[i - 1] = wf[i];
    nwf--;
    wf_changed = 1;
    js_val_t fn = js_get(js_global(), "__amdroidFetchDone");
    if(!js_callable(fn)) return;
    js_val_t args[3] = { js_num(id), js_num(status), failed ? js_null() : js_objv(js_new_buffer_bytes(data, len)) };
    js_call(fn, js_undef(), 3, args);
}

/* ---- media commands ---- */
#define WM_MAX 96
typedef struct { u32 cmd; i32 a; i64 t0, t1; u8 *data; u32 len; } wm_t;
static wm_t wm[WM_MAX];
static int nwm = 0;
static u32 wm_pushed[2], wm_acked[2];   /* per source buffer: commands queued, and handed over */
static u32 ack_state[2];                /* the state seq when the last one was handed over */
static u32 last_state_seq = 0, last_pending[2];
static int inflight_sb = -1;

static int wm_push(u32 cmd, i32 a, i64 t0, i64 t1, u8 *data, u32 len){
    if(nwm >= WM_MAX) return 0;
    wm[nwm] = (wm_t){ cmd, a, t0, t1, data, len };
    nwm++;
    if((cmd == 2 || cmd == 3 || cmd == 4) && a >= 0 && a < 2) wm_pushed[a]++;
    return 1;
}

static js_val_t m_cmd(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    u32 cmd = argc > 0 ? (u32)js_to_num(argv[0]) : 0;
    i32 a = argc > 1 ? (i32)js_to_num(argv[1]) : 0;
    i64 t0 = argc > 2 ? (i64)js_to_num(argv[2]) : 0, t1 = argc > 3 ? (i64)js_to_num(argv[3]) : 0;
    if(cmd == 1){                         /* a new presentation: what was queued is moot */
        for(int i = 0; i < nwm; i++) if(wm[i].data) kfree(wm[i].data);
        nwm = 0;
        wm_pushed[0] = wm_pushed[1] = wm_acked[0] = wm_acked[1] = 0;
        inflight_sb = -1;
    }
    return js_bool(wm_push(cmd, a, t0, t1, NULL, 0));
}

/* appendBuffer: the bytes are copied now (the page may reuse its buffer at once) */
static js_val_t m_append(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    if(argc < 2) return js_bool(0);
    i32 sb = (i32)js_to_num(argv[0]);
    if(sb < 0 || sb > 1) return js_bool(0);
    const u8 *raw;
    /* the length first, then the bytes */
    int len = js_bytes_of(argv[1], &raw, NULL, 0);
    if(len < 0) return js_bool(0);
    u8 *all = kmalloc((u32)len + 1);
    if(!all) return js_bool(0);
    if(raw) memcpy(all, raw, (u32)len);
    else len = js_bytes_of(argv[1], &raw, all, (u32)len);
    /* in pieces that fit the shared area */
    u32 left = (u32)len, off = 0;
    int pieces = (int)((left + HUSSER_MEDIA_BYTES - 1) / HUSSER_MEDIA_BYTES);
    if(pieces == 0) pieces = 1;
    if(nwm + pieces > WM_MAX){ kfree(all); return js_bool(0); }
    if(pieces == 1){ wm_push(2, sb, 0, 0, all, left); return js_bool(1); }
    while(left){
        u32 n = left > HUSSER_MEDIA_BYTES ? HUSSER_MEDIA_BYTES : left;
        u8 *p = kmalloc(n);
        if(!p){ kfree(all); return js_bool(0); }
        memcpy(p, all + off, n);
        wm_push(2, sb, 0, 0, p, n);
        off += n; left -= n;
    }
    kfree(all);
    return js_bool(1);
}

/* a marker for "everything queued for this buffer so far" */
static js_val_t m_sent(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    int sb = argc ? (int)js_to_num(argv[0]) : 0;
    return js_num(sb >= 0 && sb < 2 ? wm_pushed[sb] : 0);
}
/* 1 when everything up to the marker was handed over and Reel has finished with it */
static js_val_t m_done(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    int sb = argc ? (int)js_to_num(argv[0]) : 0;
    u32 since = argc > 1 ? (u32)js_to_num(argv[1]) : 0;
    if(sb < 0 || sb > 1) return js_bool(1);
    return js_bool(wm_acked[sb] >= since && (i32)(last_state_seq - ack_state[sb]) > 0 && last_pending[sb] == 0);
}

static js_val_t m_dispatch(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    if(argc < 2 || argv[0].t != JS_OBJ) return js_undef();
    wnode_t *n = (wnode_t *)js_host(argv[0].o);
    if(n) web_dispatch(n, js_to_str(argv[1]), NULL);
    return js_undef();
}

static js_val_t m_text(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    if(argc < 1) return js_str("");
    const u8 *raw;
    int len = js_bytes_of(argv[0], &raw, NULL, 0);
    if(len <= 0) return js_str("");
    if(raw) return js_str_copy((const char *)raw, (u32)len);
    char *tmp = kmalloc((u32)len + 1);
    if(!tmp) return js_str("");
    len = js_bytes_of(argv[0], &raw, (u8 *)tmp, (u32)len);
    js_val_t v = js_str_copy(tmp, (u32)len);
    kfree(tmp);
    return v;
}

/* Husser's loop: the next command to hand over (0 when none). */
int web_media_next(u32 *cmd, i32 *a, i64 *t0, i64 *t1, const u8 **data, u32 *len){
    if(!nwm) return 0;
    *cmd = wm[0].cmd; *a = wm[0].a; *t0 = wm[0].t0; *t1 = wm[0].t1; *data = wm[0].data; *len = wm[0].len;
    return 1;
}
/* ...it is in the shared region now: drop it from the queue */
void web_media_pop(void){
    if(!nwm) return;
    if(wm[0].data) kfree(wm[0].data);
    u32 c = wm[0].cmd;
    inflight_sb = (c == 2 || c == 3 || c == 4) ? wm[0].a : -1;
    for(int i = 1; i < nwm; i++) wm[i - 1] = wm[i];
    nwm--;
}
/* ...and the browser took it, at state seq `seq` */
void web_media_acked(u32 seq){
    if(inflight_sb >= 0 && inflight_sb < 2){ wm_acked[inflight_sb]++; ack_state[inflight_sb] = seq; }
    inflight_sb = -1;
}

/* The browser's report: hands it to the page (events fire from there). */
void web_media_state(const web_mstate_t *s){
    last_state_seq = s->seq;
    last_pending[0] = s->pending[0]; last_pending[1] = s->pending[1];
    js_val_t fn = js_get(js_global(), "__amdroidMediaTick");
    if(!js_callable(fn)) return;
    js_obj_t *o = js_new_object();
    js_set(o, "t", js_num((double)s->time_us));
    js_set(o, "dur", js_num((double)s->duration_us));
    js_set(o, "playing", js_bool((int)s->playing));
    js_set(o, "ended", js_bool((int)s->ended));
    js_set(o, "stalled", js_bool((int)s->stalled));
    js_set(o, "ready", js_bool((int)s->ready));
    js_set(o, "vw", js_num(s->video_w));
    js_set(o, "vh", js_num(s->video_h));
    js_set(o, "shown", js_num(0));
    js_obj_t *rs = js_new_array();
    for(int k = 0; k < 2; k++){
        js_obj_t *r = js_new_array();
        for(u32 i = 0; i < s->nranges[k] * 2 && i < 16; i++) js_array_push(r, js_num((double)s->ranges[k][i]));
        js_array_push(rs, js_objv(r));
    }
    js_set(o, "ranges", js_objv(rs));
    js_set(o, "errSeq", js_num(s->error_seq));
    js_set(o, "err", js_str_copy(s->error, (u32)strlen(s->error)));
    js_val_t arg = js_objv(o);
    js_call(fn, js_undef(), 1, &arg);
}

/* A new page: requests and commands of the old one are dropped. */
static void media_reset(void){
    for(int i = 0; i < nwf; i++) kfree(wf[i].url);
    nwf = 0;
    wf_changed = 1;
    for(int i = 0; i < nwm; i++) if(wm[i].data) kfree(wm[i].data);
    nwm = 0;
    wm_pushed[0] = wm_pushed[1] = wm_acked[0] = wm_acked[1] = 0;
    inflight_sb = -1;
}

static void media_setup(void){
    media_reset();
    js_obj_t *m = js_new_object();
    js_set(m, "fetch", js_objv(js_new_native(m_fetch, NULL)));
    js_set(m, "cmd", js_objv(js_new_native(m_cmd, NULL)));
    js_set(m, "append", js_objv(js_new_native(m_append, NULL)));
    js_set(m, "sent", js_objv(js_new_native(m_sent, NULL)));
    js_set(m, "done", js_objv(js_new_native(m_done, NULL)));
    js_set(m, "dispatch", js_objv(js_new_native(m_dispatch, NULL)));
    js_set(m, "text", js_objv(js_new_native(m_text, NULL)));
    js_set(js_global(), "__media", js_objv(m));
}
