/* reel_bridge.c — the kernel side of Reel, the media process.
 *
 * Owns the shared region and the process, hands commands over one at a time (appends are
 * queued here so a client never waits), and does the two jobs that need the kernel:
 *  - the sound: Reel's PCM ring goes to the HD Audio card, a little ahead of the speaker;
 *  - the clock: the position is what the card has really played, so pictures follow the
 *    sound. Silence written while Reel has nothing ready does not move the clock (the
 *    picture waits for the data instead of the sound running ahead of it).
 * Without an audio track the clock runs on the timer. */
#include "amdroid/reel.h"
#include "amdroid/pmm.h"
#include "amdroid/vm.h"
#include "amdroid/sched.h"
#include "amdroid/fb.h"
#include "amdroid/heap.h"
#include "amdroid/timer.h"
#include "amdroid/hda.h"
#include "amdroid/serial.h"
#include "amdroid/console.h"
#include <string.h>

#define SHM_PAGES   (REEL_SHM_BYTES / 4096)
#define STACK_TOP   0x7F000000ULL
#define STACK_PAGES 2048                     /* 8 MiB: dav1d and libavcodec like deep stacks */

extern u8 reel_blob_start[], reel_blob_end[];
extern char reel_bss_end[];
extern char reel_entry[];

static u64 *shm_pages;                       /* SHM_PAGES physical pages */
static int shm_ready = 0;
static int reel_pid = -1;
static const void *owner = NULL;

static void log_line(const char *a, const char *b){
    for(const char *p = "reel: "; *p; p++) serial_putc(*p);
    for(const char *p = a; p && *p; p++) serial_putc(*p);
    for(const char *p = b; p && *p; p++) serial_putc(*p);
    serial_putc('\n');
}

static volatile reel_shm_t *hdr(void){ return shm_ready ? (volatile reel_shm_t *)(unsigned long)shm_pages[0] : NULL; }
const volatile reel_shm_t *reel_header(void){ return hdr(); }

static int shm_alloc(void){
    if(!shm_pages){
        shm_pages = kmalloc(SHM_PAGES * sizeof(u64));
        if(!shm_pages) return -1;
        memset(shm_pages, 0, SHM_PAGES * sizeof(u64));
    }
    for(u64 i = 0; i < SHM_PAGES; i++){
        if(shm_pages[i]) continue;
        u64 p = (u64)(unsigned long)pmm_alloc_page();
        if(!p) return -1;
        shm_pages[i] = p;
    }
    for(u64 i = 0; i < REEL_HDR_BYTES / 4096; i++) memset((void *)(unsigned long)shm_pages[i], 0, 4096);
    /* the sound ring starts silent */
    for(u64 i = REEL_PCM_OFF / 4096; i < (REEL_PCM_OFF + REEL_PCM_BYTES) / 4096; i++) memset((void *)(unsigned long)shm_pages[i], 0, 4096);
    shm_ready = 1;
    return 0;
}

static void shm_write(u64 off, const void *src, u64 len){
    const u8 *s = src;
    while(len){
        u64 page = off / 4096;
        u32 inner = (u32)(off % 4096), take = 4096 - inner;
        if(take > len) take = (u32)len;
        memcpy((void *)(unsigned long)(shm_pages[page] + inner), s, take);
        s += take; off += take; len -= take;
    }
}
static void shm_read(u64 off, void *dst, u64 len){
    u8 *d = dst;
    while(len){
        u64 page = off / 4096;
        u32 inner = (u32)(off % 4096), take = 4096 - inner;
        if(take > len) take = (u32)len;
        memcpy(d, (const void *)(unsigned long)(shm_pages[page] + inner), take);
        d += take; off += take; len -= take;
    }
}

/* ---- the process ---- */
static int spawn(void){
    volatile reel_shm_t *m = hdr();
    memset((void *)m, 0, REEL_HDR_BYTES);
    m->magic = REEL_MAGIC; m->ver = REEL_VER;
    u64 free_mb = pmm_free_pages() * 4096 / (1024 * 1024);
    u32 heap = REEL_HEAP_BYTES;
    if(free_mb < (REEL_HEAP_BYTES >> 20) + 64) heap = free_mb > (REEL_HEAP_MIN >> 20) + 48 ? (u32)((free_mb - 48) << 20) : REEL_HEAP_MIN;
    m->heap_bytes = heap;

    u64 cr3 = vm_new_space();
    if(!cr3) return -1;
    u64 va = VM_USER_BASE;
    const u8 *src = reel_blob_start;
    u64 left = (u64)(reel_blob_end - reel_blob_start);
    while(left){
        u64 pa = vm_alloc_user_page(cr3, va);
        if(!pa){ vm_free_space(cr3); return -1; }
        u32 n = left > 4096 ? 4096 : (u32)left;
        memcpy((void *)(unsigned long)pa, src, n);
        src += n; left -= n; va += 4096;
    }
    u64 bss_end = (u64)(unsigned long)reel_bss_end;
    while(va < bss_end){ if(!vm_alloc_user_page(cr3, va)){ vm_free_space(cr3); return -1; } va += 4096; }
    for(u64 hv = REEL_HEAP_VA; hv < REEL_HEAP_VA + heap; hv += 4096)
        if(!vm_alloc_user_page(cr3, hv)){ vm_free_space(cr3); return -1; }
    for(u32 i = 0; i < STACK_PAGES; i++)
        if(!vm_alloc_user_page(cr3, STACK_TOP - (u64)(i + 1) * 4096)){ vm_free_space(cr3); return -1; }
    u64 sva = REEL_SHM_VA;
    for(u64 i = 0; i < SHM_PAGES; i++, sva += 4096)
        if(vm_map_user(cr3, sva, shm_pages[i], PTE_W | PTE_U | PTE_KEEP) != 0){ vm_free_space(cr3); return -1; }
    int pid = proc_create_user("reel", VM_USER_BASE + ((u64)(unsigned long)reel_entry - 0x40000000ULL), STACK_TOP, cr3, 0);
    if(pid < 0){ vm_free_space(cr3); return -1; }
    proc_detach(pid);
    reel_pid = pid;
    /* it reports READY once its heap is up */
    u64 deadline = timer_ticks() + 300;
    while(m->state == R_ST_BOOT && proc_alive(pid) && timer_ticks() < deadline) proc_yield();
    if(m->state != R_ST_READY){ log_line("the media process did not start", 0); return -1; }
    char n[16]; u32 mb = heap >> 20, k = 0; char t[12]; do { t[k++] = (char)('0' + mb % 10); mb /= 10; } while(mb);
    for(u32 i = 0; i < k; i++) n[i] = t[k - 1 - i];
    n[k] = 0;
    log_line("started, heap MB ", n);
    return 0;
}

int reel_alive(void){
    if(reel_pid >= 0 && !proc_alive(reel_pid)){
        reel_pid = -1;
        volatile reel_shm_t *m = hdr();
        if(m && m->error[0]) log_line("the media process ended: ", (const char *)m->error);
        else log_line("the media process ended", 0);
    }
    return reel_pid >= 0;
}

/* ---- commands ---- */
typedef struct qcmd {
    u32 cmd;
    i32 a, b;
    i64 t0, t1;
    u32 smooth;
    u8 *data;                 /* appends: the bytes (kmalloc'd), handed over in pieces */
    u32 len, done;
    struct qcmd *next;
} qcmd_t;
static qcmd_t *qhead, *qtail;
static int inflight = 0;
static u32 inflight_seq = 0;
static qcmd_t *inflight_cmd = NULL;
static u64 queued_bytes = 0;
static int pending_appends[REEL_SBS];
static int seek_waiting = 0;           /* a seek was handed over: reset the sound when it is done */

static void q_push(qcmd_t *c){
    c->next = NULL;
    if(qtail) qtail->next = c; else qhead = c;
    qtail = c;
}

static void q_free(qcmd_t *c){
    if(c->data){ kfree(c->data); queued_bytes -= c->len; }
    if(c->cmd == R_CMD_APPEND && c->a >= 0 && c->a < REEL_SBS && pending_appends[c->a] > 0) pending_appends[c->a]--;
    kfree(c);
}

static void q_clear(void){
    while(qhead){ qcmd_t *c = qhead; qhead = c->next; q_free(c); }
    qtail = NULL;
    inflight_cmd = NULL;               /* its ack still comes; there is nothing to finish */
}

static void audio_after_seek(void);

/* hands the next command (or the next piece of an append) to Reel */
static void pump(void){
    volatile reel_shm_t *m = hdr();
    if(!m || !reel_alive()){ inflight = 0; return; }
    if(inflight && m->ack_seq == inflight_seq){
        inflight = 0;
        if(inflight_cmd){
            if(inflight_cmd->cmd == R_CMD_SEEK) audio_after_seek();
            if(inflight_cmd->cmd != R_CMD_APPEND || inflight_cmd->done >= inflight_cmd->len){
                if(qhead == inflight_cmd){ qhead = inflight_cmd->next; if(!qhead) qtail = NULL; }
                q_free(inflight_cmd);
            }
            inflight_cmd = NULL;
        }
    }
    if(inflight || !qhead) return;
    qcmd_t *c = qhead;
    m->cmd = c->cmd; m->a = c->a; m->b = c->b; m->t0 = c->t0; m->t1 = c->t1; m->smooth = c->smooth;
    if(c->cmd == R_CMD_APPEND){
        u32 piece = c->len - c->done;
        if(piece > REEL_IN_BYTES) piece = REEL_IN_BYTES;
        shm_write(REEL_IN_OFF, c->data + c->done, piece);
        c->done += piece;
        m->b = (i32)piece;
    }
    __asm__ volatile("" ::: "memory");
    inflight_seq = ++m->cmd_seq;
    inflight = 1;
    inflight_cmd = c;
    proc_wake(reel_pid);
}

static qcmd_t *new_cmd(u32 cmd){
    qcmd_t *c = kmalloc(sizeof *c);
    if(!c) return NULL;
    memset(c, 0, sizeof *c);
    c->cmd = cmd;
    return c;
}
static void post(u32 cmd, i32 a, i32 b, i64 t0, i64 t1){
    if(reel_pid < 0) return;
    qcmd_t *c = new_cmd(cmd);
    if(!c) return;
    c->a = a; c->b = b; c->t0 = t0; c->t1 = t1;
    q_push(c);
    pump();
}

/* ---- the clock and the sound ---- */
static int playing = 0;
static int volume = 100;
static i64 paused_at = 0;              /* the position while not playing */
static u64 timer_t0 = 0;               /* video-only clock: ticks when play started */
static i64 timer_pos0 = 0;
static int audio_mode = 0;             /* 1 while the clock comes from the sound card */
static int timer_running = 0;          /* the timer clock has started (video only) */
static int hda_on = 0;
static u64 hw_pos = 0;                 /* ring frames written to the card */
static u64 hda_frames = 0;             /* frames written to the card since it started */
static u32 epoch_seen = 0;
static int want_first = 1;             /* show the next picture whatever its time (after a seek) */
static int stalled = 0;
/* what the card is playing, in runs: real samples (from ring position r) or silence */
typedef struct { u64 hda_at, ring_at; u32 n; u8 silent; } run_t;
#define RUNS 64
static run_t runs[RUNS];
static int run_h = 0, run_n = 0;

static void runs_clear(void){ run_h = 0; run_n = 0; }
static void run_add(u64 ring_at, u32 n, int silent){
    if(run_n){
        run_t *l = &runs[(run_h + run_n - 1) % RUNS];
        if(l->silent == silent && (silent || l->ring_at + l->n == ring_at) && l->hda_at + l->n == hda_frames){ l->n += n; return; }
    }
    if(run_n == RUNS){ run_h = (run_h + 1) % RUNS; run_n--; }
    runs[(run_h + run_n) % RUNS] = (run_t){ hda_frames, ring_at, n, (u8)silent };
    run_n++;
}

/* the ring position the speaker is at */
static u64 played_ring_pos(void){
    if(!hda_on) return hw_pos;
    u64 played = hda_stream_played() / 4;
    while(run_n > 1){
        run_t *r = &runs[run_h];
        if(r->hda_at + r->n > played) break;
        run_h = (run_h + 1) % RUNS; run_n--;
    }
    if(!run_n) return hw_pos;
    run_t *r = &runs[run_h];
    if(played < r->hda_at) return r->ring_at;
    u64 in = played - r->hda_at;
    if(in > r->n) in = r->n;
    return r->silent ? r->ring_at : r->ring_at + in;
}

static i64 ring_time(u64 pos){
    volatile reel_shm_t *m = hdr();
    i64 p0 = (i64)m->pcm_p0;
    return m->pcm_t0 + ((i64)pos - p0) * 1000000 / 48000;
}

static i64 clock_now(void){
    if(!playing || seek_waiting) return paused_at;
    if(audio_mode) return ring_time(played_ring_pos());
    if(!timer_running) return paused_at;           /* nothing to play yet: time stands still */
    return timer_pos0 + (i64)(timer_ticks() - timer_t0) * 10000;
}

/* the ring position for time t (where the sound starts after play/seek) */
static u64 ring_pos_for(i64 t){
    volatile reel_shm_t *m = hdr();
    i64 d = t - m->pcm_t0;
    if(d < 0) d = 0;
    return m->pcm_p0 + (u64)(d * 48000 / 1000000);
}

static void sound_stop(void){
    if(hda_on){
        hw_pos = played_ring_pos();    /* what was written but not heard is written again */
        hda_stream_stop();
        hda_on = 0;
    }
    runs_clear();
}

static void sound_start(void){
    if(hda_on || !hda_present()) return;
    if(hda_stream_start() != 0) return;
    hda_on = 1;
    hda_frames = 0;
    runs_clear();
}

/* keeps about 0.2 s queued on the card */
#define AHEAD_FRAMES 9600
static void feed_sound(void){
    volatile reel_shm_t *m = hdr();
    if(!hda_on) return;
    static int16_t buf[4096 * 2];
    for(int rounds = 0; rounds < 8; rounds++){
        u64 played = hda_stream_played() / 4;
        u64 queued = hda_frames > played ? hda_frames - played : 0;
        if(queued >= AHEAD_FRAMES) break;
        u32 want = (u32)(AHEAD_FRAMES - queued);
        if(want > 4096) want = 4096;
        u32 room = hda_stream_room() / 4;
        if(want > room) want = room;
        if(!want) break;
        u64 w = m->pcm_w;
        u64 have = w > hw_pos ? w - hw_pos : 0;
        if(hw_pos < m->pcm_r) have = 0;              /* (cannot happen) overwritten */
        if(have){
            u32 n = have < want ? (u32)have : want;
            u64 at = hw_pos % REEL_PCM_FRAMES;
            u32 first = (u32)(REEL_PCM_FRAMES - at);
            if(first > n) first = n;
            shm_read(REEL_PCM_OFF + at * 4, buf, (u64)first * 4);
            if(n > first) shm_read(REEL_PCM_OFF, buf + first * 2, (u64)(n - first) * 4);
            if(volume != 100){
                for(u32 i = 0; i < n * 2; i++){
                    int v = buf[i] * volume / 100;
                    buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            u32 wrote = hda_stream_write(buf, n * 4) / 4;
            run_add(hw_pos, wrote, 0);
            hw_pos += wrote;
            hda_frames += wrote;
            stalled = 0;
            if(wrote < n) break;
        } else {
            /* nothing decoded yet: silence, which the clock does not count */
            memset(buf, 0, (u64)want * 4);
            u32 wrote = hda_stream_write(buf, want * 4) / 4;
            run_add(hw_pos, wrote, 1);
            hda_frames += wrote;
            if(m->has_audio && !m->ended) stalled = 1;
            if(wrote < want) break;
        }
    }
    /* Reel may reuse what the speaker has played */
    u64 r = played_ring_pos();
    if(r > m->pcm_w) r = m->pcm_w;
    if(r > m->pcm_r) m->pcm_r = r;
}

/* a seek finished in Reel: the sound starts over at the new place */
static void audio_after_seek(void){
    volatile reel_shm_t *m = hdr();
    epoch_seen = m->pcm_epoch;
    if(hda_on){ hda_stream_stop(); hda_on = 0; }
    runs_clear();
    hw_pos = m->pcm_p0;
    m->pcm_r = hw_pos;
    seek_waiting = 0;
    /* the clock starts again from the target once there is something to play (reel_tick) */
    audio_mode = 0;
    timer_running = 0;
}

/* ---- the API ---- */
int reel_begin(const void *who){
    if(!shm_ready && shm_alloc() != 0){ log_line("out of memory for the shared region", 0); return -1; }
    if(owner && owner != who){ /* someone else had it: their sound stops */ }
    if(!reel_alive()){
        q_clear(); inflight = 0; inflight_cmd = NULL;
        memset(pending_appends, 0, sizeof pending_appends);
        if(spawn() != 0){ reel_shutdown(); return -1; }
    }
    owner = who;
    sound_stop();
    playing = 0; paused_at = 0; audio_mode = 0; timer_running = 0;
    hw_pos = 0; want_first = 1; stalled = 0;
    /* whatever was queued for the last presentation is moot */
    q_clear();
    memset(pending_appends, 0, sizeof pending_appends);
    volatile reel_shm_t *m = hdr();
    for(int k = 0; k < REEL_SLOTS; k++) if(m->slot[k].state == R_SLOT_SHOWN) m->slot[k].state = R_SLOT_FREE;
    m->clock_us = 0; m->playing = 0;
    post(R_CMD_RESET, 0, 0, 0, 0);
    /* the reset is done before anything else is handed over, so the ring restarts at its
       new epoch: wait for it (it is quick) */
    u64 deadline = timer_ticks() + 100;
    while(inflight && timer_ticks() < deadline){ pump(); if(inflight) proc_yield(); }
    epoch_seen = m->pcm_epoch;
    hw_pos = m->pcm_p0;
    m->pcm_r = hw_pos;
    return 0;
}

int reel_owned_by(const void *who){ return owner && owner == who && reel_pid >= 0; }

void reel_end(const void *who){
    if(owner != who) return;
    sound_stop();
    playing = 0;
    owner = NULL;
    q_clear();
    if(!inflight) inflight_cmd = NULL;
    memset(pending_appends, 0, sizeof pending_appends);
    if(reel_pid >= 0) post(R_CMD_RESET, 0, 0, 0, 0);
}

void reel_shutdown(void){
    sound_stop();
    playing = 0;
    owner = NULL;
    if(reel_pid >= 0){ proc_kill(reel_pid); reel_pid = -1; }
    q_clear(); inflight = 0; inflight_cmd = NULL;
    memset(pending_appends, 0, sizeof pending_appends);
}

#define QUEUE_LIMIT (24ull * 1024 * 1024)
int reel_append(int sb, const void *data, u32 len){
    if(reel_pid < 0 || sb < 0 || sb >= REEL_SBS || !len) return -1;
    if(queued_bytes + len > QUEUE_LIMIT) return -1;
    qcmd_t *c = new_cmd(R_CMD_APPEND);
    if(!c) return -1;
    c->data = kmalloc(len);
    if(!c->data){ kfree(c); return -1; }
    memcpy(c->data, data, len);
    c->len = len; c->a = sb;
    queued_bytes += len;
    pending_appends[sb]++;
    q_push(c);
    pump();
    return 0;
}
int reel_append_pending(int sb){ return sb >= 0 && sb < REEL_SBS ? pending_appends[sb] : 0; }
u64 reel_queued_bytes(void){ return queued_bytes; }
void reel_remove(int sb, i64 t0, i64 t1){ post(R_CMD_REMOVE, sb, 0, t0, t1); }
void reel_abort(int sb){ post(R_CMD_ABORT, sb, 0, 0, 0); }
void reel_eos(void){ post(R_CMD_EOS, 0, 0, 0, 0); }

/* The size the picture is shown at. While it plays, Reel converts at the video's own size
 * and reel_blit stretches it on the way to the screen (cheaper than writing a big picture
 * and copying it again); a still (smooth) is converted by Reel at the size it is shown. */
static u32 size_w, size_h;
static int size_smooth;
void reel_set_size(u32 w, u32 h, int smooth){
    if(w > REEL_MAX_W) w = REEL_MAX_W;
    if(h > REEL_MAX_H) h = REEL_MAX_H;
    if(w == size_w && h == size_h && smooth == size_smooth) return;
    int was_smooth = size_smooth;
    size_w = w; size_h = h; size_smooth = smooth;
    if(reel_pid < 0) return;
    if(!smooth && !was_smooth) return;      /* Reel goes on at the video's size */
    qcmd_t *c = new_cmd(R_CMD_SIZE);
    if(!c) return;
    c->a = smooth ? (i32)w : 0; c->b = smooth ? (i32)h : 0; c->smooth = (u32)smooth;
    q_push(c);
    /* paused: the picture on screen is converted again at the new size */
    volatile reel_shm_t *m = hdr();
    for(int k = 0; k < REEL_SLOTS; k++){
        if(m->slot[k].state == R_SLOT_SHOWN && !playing){
            qcmd_t *r = new_cmd(R_CMD_REDRAW);
            if(r){ r->a = (i32)w; r->b = (i32)h; r->smooth = (u32)smooth; r->t0 = m->slot[k].pts; q_push(r); }
        }
    }
    pump();
}

/* Playing starts the clock only once there is something to play: like a <video>, time does
 * not run while the data is still on its way (reel_tick decides, every tick). */
void reel_play(void){
    if(playing || reel_pid < 0) return;
    volatile reel_shm_t *m = hdr();
    playing = 1;
    m->playing = 1;
    audio_mode = 0;
    timer_running = 0;
}

void reel_pause(void){
    if(!playing) return;
    paused_at = clock_now();
    playing = 0;
    volatile reel_shm_t *m = hdr();
    if(m){ m->playing = 0; m->clock_us = paused_at; }
    sound_stop();
    /* the ring keeps what was not heard: it plays from there on resume */
    audio_mode = 0;
    timer_running = 0;
}

int reel_playing(void){ return playing; }

void reel_seek(i64 t){
    if(t < 0) t = 0;
    volatile reel_shm_t *m = hdr();
    if(!m || reel_pid < 0) return;
    /* a seek replaces one that has not been handed over yet */
    for(qcmd_t **pp = &qhead; *pp; ){
        qcmd_t *c = *pp;
        if(c->cmd == R_CMD_SEEK && c != inflight_cmd){ *pp = c->next; if(qtail == c){ qtail = qhead; while(qtail && qtail->next) qtail = qtail->next; } q_free(c); }
        else pp = &c->next;
    }
    sound_stop();
    audio_mode = 0;
    timer_running = 0;
    paused_at = t;
    timer_t0 = timer_ticks();
    timer_pos0 = t;
    m->clock_us = t;
    want_first = 1;
    seek_waiting = 1;
    post(R_CMD_SEEK, 0, 0, t, 0);
}

i64 reel_time(void){
    if(!hdr()) return 0;
    i64 t = clock_now();
    return t < 0 ? 0 : t;
}

i64 reel_duration(void){ volatile reel_shm_t *m = hdr(); return m ? m->duration_us : -1; }

int reel_ended(void){
    volatile reel_shm_t *m = hdr();
    if(!m || !m->ended) return 0;
    if(audio_mode && m->has_audio) return played_ring_pos() >= m->pcm_w;
    for(int k = 0; k < REEL_SLOTS; k++) if(m->slot[k].state == R_SLOT_READY) return 0;
    return 1;
}

int reel_stalled(void){ return playing && stalled; }

void reel_set_volume(int v){ volume = v < 0 ? 0 : v > 200 ? 200 : v; }

const char *reel_error(u32 *seq){
    volatile reel_shm_t *m = hdr();
    if(!m){ if(seq) *seq = 0; return ""; }
    if(seq) *seq = m->error_seq;
    return (const char *)m->error;
}

static int shown = -1;
static u32 last_log_seq = 0;

int reel_tick(void){
    volatile reel_shm_t *m = hdr();
    if(!m) return 0;
    if(!reel_alive()){ playing = 0; return 0; }
    pump();
    if(m->log_seq != last_log_seq){ last_log_seq = m->log_seq; log_line((const char *)m->log, 0); }
    /* which clock: the sound card's once there is a sound track, else the timer once there
       is a picture; nothing while a source buffer in use has not even had its init segment */
    if(playing && !audio_mode && !seek_waiting){
        int waiting = 0;
        for(int k = 0; k < REEL_SBS; k++) if(m->sb[k].used && !m->sb[k].have_init && !m->sb[k].error) waiting = 1;
        int picture = shown >= 0;
        for(int k = 0; k < REEL_SLOTS; k++) if(m->slot[k].state == R_SLOT_READY) picture = 1;
        if(!waiting && m->has_audio && hda_present()){
            i64 now = clock_now();               /* where the timer got to, if it ran (video came first) */
            audio_mode = 1;
            hw_pos = ring_pos_for(now);
            if(hw_pos < m->pcm_r) hw_pos = m->pcm_r;
            sound_start();
        } else if(!timer_running && (picture || (!waiting && !m->has_video && m->has_audio))){
            timer_running = 1;
            timer_t0 = timer_ticks();
            timer_pos0 = paused_at;
        }
    }
    if(playing && audio_mode && !seek_waiting) feed_sound();
    i64 now = clock_now();
    if(playing) m->clock_us = now;
    /* the picture that is due */
    int best = -1;
    i64 best_pts = 0;
    int earliest = -1;
    for(int k = 0; k < REEL_SLOTS; k++){
        if(m->slot[k].state != R_SLOT_READY) continue;
        i64 p = m->slot[k].pts;
        if(earliest < 0 || p < m->slot[earliest].pts) earliest = k;
        if(p <= now + 8000 && (best < 0 || p > best_pts)){ best = k; best_pts = p; }
    }
    if(best < 0 && earliest >= 0 && (want_first || shown < 0)) best = earliest;
    if(best < 0) return 0;
    best_pts = m->slot[best].pts;
    for(int k = 0; k < REEL_SLOTS; k++){
        if(k == best) continue;
        if(m->slot[k].state == R_SLOT_SHOWN) m->slot[k].state = R_SLOT_FREE;
        else if(m->slot[k].state == R_SLOT_READY && m->slot[k].pts < best_pts) m->slot[k].state = R_SLOT_FREE;
    }
    m->slot[best].state = R_SLOT_SHOWN;
    shown = best;
    want_first = 0;
    proc_wake(reel_pid);            /* a slot is free again */
    return 1;
}

/* the size the picture is drawn at: the size asked for, or the video's own */
int reel_picture(u32 *w, u32 *h){
    volatile reel_shm_t *m = hdr();
    if(!m || shown < 0 || m->slot[shown].state != R_SLOT_SHOWN) return 0;
    if(w) *w = size_w ? size_w : m->slot[shown].w;
    if(h) *h = size_h ? size_h : m->slot[shown].h;
    return 1;
}

void reel_blit(i32 x, i32 y, u32 w, u32 h){
    volatile reel_shm_t *m = hdr();
    u32 dw, dh;
    if(!reel_picture(&dw, &dh)) return;
    u32 pw = m->slot[shown].w, ph = m->slot[shown].h;
    u64 first = (REEL_FRAME_OFF + (u64)shown * REEL_FRAME_BYTES) / 4096;
    if(pw == dw && ph == dh && x >= 0 && y >= 0) fb_copy_region_pages((u32)x, (u32)y, w < pw ? w : pw, h < ph ? h : ph, shm_pages + first, pw);
    else fb_scale_region_pages(x, y, w < dw ? w : dw, h < dh ? h : dh, shm_pages + first, pw, ph, pw);
}
