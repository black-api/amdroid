/* scribe_bridge.c — the kernel side of Scribe, the speech-to-text process.
 *
 * Scribe is built like Reel (a flat ring-3 image embedded in the kernel) and its model is
 * embedded right after it, page aligned, and mapped read-only into Scribe: nothing is copied.
 * A request copies samples into the shared region, bumps req_seq and waits for done_seq. */
#include "amdroid/scribe.h"
#include "amdroid/pmm.h"
#include "amdroid/vm.h"
#include "amdroid/sched.h"
#include "amdroid/heap.h"
#include "amdroid/timer.h"
#include "amdroid/serial.h"
#include <string.h>

#define SHM_PAGES   ((SCRIBE_SHM_BYTES + 4095) / 4096)
#define STACK_TOP   0x7F000000ULL
#define STACK_PAGES 256                      /* 1 MiB */

extern u8 scribe_blob_start[], scribe_blob_end[];
extern u8 scribe_model_start[], scribe_model_end[];
extern u8 scribe_needle_start[], scribe_needle_end[];
extern char scribe_bss_end[];
extern char scribe_entry[];

static u64 *shm_pages;
static int pid = -1;

static void log_line(const char *a, const char *b){
    for(const char *p = "scribe: "; *p; p++) serial_putc(*p);
    for(const char *p = a; p && *p; p++) serial_putc(*p);
    for(const char *p = b; p && *p; p++) serial_putc(*p);
    serial_putc('\n');
}
static volatile scribe_hdr_t *hdr(void){ return shm_pages ? (volatile scribe_hdr_t *)(unsigned long)shm_pages[0] : NULL; }
const volatile scribe_hdr_t *scribe_header(void){ return hdr(); }

static int has_sse41(void){
    u32 a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d) : "c"(0));
    return (c >> 19) & 1;
}

static void cpy(char *out, u32 cap, const char *s){
    u32 i = 0;
    if(!cap) return;
    for(; s[i] && i + 1 < cap; i++) out[i] = s[i];
    out[i] = 0;
}

static int spawn(char *why, u32 cap){
    if(!has_sse41()){ cpy(why, cap, "this CPU has no SSE4.1 (run with KVM / -cpu host)"); return -1; }
    if(!shm_pages){
        shm_pages = kmalloc(SHM_PAGES * sizeof(u64));
        if(!shm_pages){ cpy(why, cap, "out of memory"); return -1; }
        memset(shm_pages, 0, SHM_PAGES * sizeof(u64));
        for(u64 i = 0; i < SHM_PAGES; i++){
            u64 p = (u64)(unsigned long)pmm_alloc_page();
            if(!p){ cpy(why, cap, "out of memory"); return -1; }
            shm_pages[i] = p;
        }
    }
    volatile scribe_hdr_t *m = hdr();
    memset((void *)m, 0, SCRIBE_HDR_BYTES);
    m->magic = SCRIBE_MAGIC;
    m->model_bytes = (u32)(scribe_model_end - scribe_model_start);
    m->needle_bytes = (u32)(scribe_needle_end - scribe_needle_start);
    u32 heap = SCRIBE_HEAP_BYTES;
    u64 free_mb = pmm_free_pages() * 4096 / (1024 * 1024);
    if(free_mb < 64 + (heap >> 20)) heap = free_mb > 112 ? (u32)((free_mb - 64) << 20) : (48u << 20);
    m->heap_bytes = heap;

    u64 cr3 = vm_new_space();
    if(!cr3){ cpy(why, cap, "no address space"); return -1; }
    u64 va = VM_USER_BASE;
    const u8 *src = scribe_blob_start;
    u64 left = (u64)(scribe_blob_end - scribe_blob_start);
    while(left){
        u64 pa = vm_alloc_user_page(cr3, va);
        if(!pa) goto oom;
        u32 n = left > 4096 ? 4096 : (u32)left;
        memcpy((void *)(unsigned long)pa, src, n);
        src += n; left -= n; va += 4096;
    }
    while(va < (u64)(unsigned long)scribe_bss_end){ if(!vm_alloc_user_page(cr3, va)) goto oom; va += 4096; }
    vm_lazy_range(cr3, SCRIBE_HEAP_VA, SCRIBE_HEAP_VA + heap);
    vm_lazy_range(cr3, STACK_TOP - (u64)STACK_PAGES * 4096, STACK_TOP);
    for(u64 i = 0; i < SHM_PAGES; i++)
        if(vm_map_user(cr3, SCRIBE_SHM_VA + i * 4096, shm_pages[i], PTE_W | PTE_U | PTE_KEEP) != 0) goto oom;
    /* the model: the kernel image's own pages (identity mapped), read-only */
    for(u64 off = 0; off < m->model_bytes; off += 4096)
        if(vm_map_user(cr3, SCRIBE_MODEL_VA + off, (u64)(unsigned long)scribe_model_start + off, PTE_U | PTE_KEEP) != 0) goto oom;
    for(u64 off = 0; off < m->needle_bytes; off += 4096)
        if(vm_map_user(cr3, SCRIBE_NEEDLE_VA + off, (u64)(unsigned long)scribe_needle_start + off, PTE_U | PTE_KEEP) != 0) goto oom;
    pid = proc_create_user("scribe", VM_USER_BASE + ((u64)(unsigned long)scribe_entry - 0x40000000ULL), STACK_TOP, cr3, 0);
    if(pid < 0){ vm_free_space(cr3); cpy(why, cap, "cannot create the process"); return -1; }
    proc_detach(pid);
    u64 deadline = timer_ticks() + 1000;
    while(m->state == SC_ST_BOOT && proc_alive(pid) && timer_ticks() < deadline) proc_yield();
    if(m->state != SC_ST_READY){
        if(m->state == SC_ST_FAILED && m->err[0]) cpy(why, cap, (const char *)m->err);
        else cpy(why, cap, "the speech process did not start");
        log_line("failed: ", why);
        if(proc_alive(pid)) proc_kill(pid);
        pid = -1;
        return -1;
    }
    log_line("started", 0);
    return 0;
oom:
    vm_free_space(cr3);
    cpy(why, cap, "out of memory");
    return -1;
}

void scribe_shutdown(void){ if(pid >= 0){ proc_kill(pid); pid = -1; } }

static u32 pending_seq;          /* nonzero while a request is out */

/* Drops silence before and after the speech (keeping 0.3 s around it): Whisper tends to
 * invent or repeat words over long quiet stretches. Loudness is measured in 20 ms windows. */
static void trim(const float **pcm, u32 *n){
    const u32 W = 320, keep = SCRIBE_RATE * 3 / 10;
    u32 nw = *n / W;
    if(nw < 3) return;
    float peak = 0;
    for(u32 i = 0; i < *n; i++){ float v = (*pcm)[i] < 0 ? -(*pcm)[i] : (*pcm)[i]; if(v > peak) peak = v; }
    float thr = peak * 0.08f;
    if(thr < 0.004f) thr = 0.004f;
    u32 first = nw, last = 0;
    for(u32 w = 0; w < nw; w++){
        float m = 0;
        for(u32 i = w * W; i < w * W + W; i++){ float v = (*pcm)[i] < 0 ? -(*pcm)[i] : (*pcm)[i]; if(v > m) m = v; }
        if(m >= thr){ if(first == nw) first = w; last = w; }
    }
    if(first == nw) return;
    u32 a = first * W > keep ? first * W - keep : 0;
    u32 b = (last + 1) * W + keep;
    if(b > *n) b = *n;
    *pcm += a; *n = b - a;
}

static int ensure(char *why, u32 cap){
    if(pid >= 0 && !proc_alive(pid)) pid = -1;
    if(pid < 0 && spawn(why, cap) != 0) return -1;
    return 0;
}

int scribe_prepare_commands(char *why, u32 cap){
    if(ensure(why, cap) != 0) return -1;
    volatile scribe_hdr_t *m = hdr();
    m->want_needle = 1;
    proc_wake(pid);
    return 0;
}

static int submit_mode(const float *pcm, u32 n, u32 mode, char *why, u32 cap){
    if(pending_seq){ cpy(why, cap, "busy"); return -1; }
    trim(&pcm, &n);
    if(ensure(why, cap) != 0) return -1;
    volatile scribe_hdr_t *m = hdr();
    m->mode = mode;
    if(n > SCRIBE_MAX_SAMPLES) n = SCRIBE_MAX_SAMPLES;
    const u8 *s = (const u8 *)pcm;
    for(u64 off = 0, left = (u64)n * 4; left; ){
        u64 page = (SCRIBE_PCM_OFF + off) / 4096;
        u32 take = 4096 - (u32)((SCRIBE_PCM_OFF + off) % 4096);
        if(take > left) take = (u32)left;
        memcpy((void *)(unsigned long)(shm_pages[page] + (SCRIBE_PCM_OFF + off) % 4096), s + off, take);
        off += take; left -= take;
    }
    m->n_samples = n;
    __asm__ volatile("" ::: "memory");
    u32 seq = m->req_seq + 1;
    if(!seq) seq = 1;
    m->req_seq = seq;
    pending_seq = seq;
    proc_wake(pid);
    return 0;
}

int scribe_submit(const float *pcm, u32 n, char *why, u32 cap){ return submit_mode(pcm, n, SC_MODE_TRANSCRIBE, why, cap); }
int scribe_submit_command(const float *pcm, u32 n, char *why, u32 cap){ return submit_mode(pcm, n, SC_MODE_COMMAND, why, cap); }

int scribe_submit_ask(const char *text, char *why, u32 cap){
    if(pending_seq){ cpy(why, cap, "busy"); return -1; }
    if(ensure(why, cap) != 0) return -1;
    volatile scribe_hdr_t *m = hdr();
    u32 i = 0;
    for(; text[i] && i + 1 < SCRIBE_TEXT_BYTES; i++){ u64 o = SCRIBE_TEXT_OFF + i; *(char *)(unsigned long)(shm_pages[o / 4096] + o % 4096) = text[i]; }
    { u64 o = SCRIBE_TEXT_OFF + i; *(char *)(unsigned long)(shm_pages[o / 4096] + o % 4096) = 0; }
    m->text_len = i;
    m->mode = SC_MODE_ASK;
    m->n_samples = 0;
    __asm__ volatile("" ::: "memory");
    u32 seq = m->req_seq + 1;
    if(!seq) seq = 1;
    m->req_seq = seq;
    pending_seq = seq;
    proc_wake(pid);
    return 0;
}

static void shm_str(u64 off, u32 maxlen, char *out, u32 cap){
    u32 i = 0;
    for(; i + 1 < cap && i < maxlen; i++){
        u64 o = off + i;
        char c = *(const char *)(unsigned long)(shm_pages[o / 4096] + o % 4096);
        if(!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

void scribe_command_result(scribe_cmd_t *r){
    volatile scribe_hdr_t *m = hdr();
    memset(r, 0, sizeof *r);
    if(!m) return;
    shm_str(SCRIBE_TEXT_OFF, SCRIBE_TEXT_BYTES, r->heard, sizeof r->heard);
    shm_str(SCRIBE_CALLS_OFF, SCRIBE_CALLS_BYTES, r->calls, sizeof r->calls);
    shm_str(SCRIBE_REASON_OFF, SCRIBE_REASON_BYTES, r->reasoning, sizeof r->reasoning);
    r->confidence = m->confidence; r->withheld = m->withheld; r->n_calls = (int)m->n_calls;
    r->needle_ms = m->needle_us / 1000; r->load_ms = m->needle_load_us / 1000;
    r->stt_ms = (m->mel_us + m->enc_us + m->dec_us) / 1000;
}

int scribe_poll(char *out, u32 cap){
    if(!pending_seq){ cpy(out, cap, "nothing asked"); return -1; }
    volatile scribe_hdr_t *m = hdr();
    if(m->done_seq != pending_seq){
        if(proc_alive(pid)) return -2;
        pid = -1; pending_seq = 0;
        cpy(out, cap, m->err[0] ? (const char *)m->err : "the speech process crashed");
        return -1;
    }
    pending_seq = 0;
    u32 len = m->text_len;
    if(len >= cap) len = cap - 1;
    for(u32 i = 0; i < len; i++){
        u64 o = SCRIBE_TEXT_OFF + i;
        out[i] = *(const char *)(unsigned long)(shm_pages[o / 4096] + o % 4096);
    }
    out[len] = 0;
    return (int)len;
}

int scribe_busy(void){ return pending_seq != 0; }

int scribe_transcribe(const float *pcm, u32 n, char *out, u32 cap){
    if(scribe_submit(pcm, n, out, cap) != 0) return -1;
    int r;
    while((r = scribe_poll(out, cap)) == -2) proc_yield();
    return r;
}

/* ---- WAV ---- */
static u32 rd32(const u8 *p){ return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p){ return (u16)(p[0] | p[1] << 8); }

int scribe_transcribe_wav(const u8 *w, u32 len, char *out, u32 cap){
    if(len < 12 || memcmp(w, "RIFF", 4) || memcmp(w + 8, "WAVE", 4)){ cpy(out, cap, "not a WAV file"); return -1; }
    u16 fmt = 0, ch = 0, bits = 0; u32 rate = 0;
    const u8 *data = NULL; u32 dlen = 0;
    for(u32 o = 12; o + 8 <= len; ){
        u32 sz = rd32(w + o + 4);
        if(!memcmp(w + o, "fmt ", 4) && sz >= 16){
            fmt = rd16(w + o + 8); ch = rd16(w + o + 10); rate = rd32(w + o + 12); bits = rd16(w + o + 22);
            if(fmt == 0xFFFE && sz >= 26) fmt = rd16(w + o + 32);    /* WAVE_FORMAT_EXTENSIBLE: the sub-format */
        } else if(!memcmp(w + o, "data", 4)){
            data = w + o + 8; dlen = sz > len - o - 8 ? len - o - 8 : sz;
            break;
        }
        o += 8 + sz + (sz & 1);
    }
    if(!data || !ch || !rate || !(fmt == 1 || fmt == 3)){ cpy(out, cap, "unsupported WAV (need PCM or float)"); return -1; }
    if(!(bits == 8 || bits == 16 || bits == 24 || bits == 32) || (fmt == 3 && bits != 32)){ cpy(out, cap, "unsupported WAV sample size"); return -1; }
    u32 bps = bits / 8, frames = dlen / (bps * ch);
    u64 outn = (u64)frames * SCRIBE_RATE / rate;
    if(outn > SCRIBE_MAX_SAMPLES) outn = SCRIBE_MAX_SAMPLES;
    float *pcm = kmalloc(outn * 4 + 4);
    if(!pcm){ cpy(out, cap, "out of memory"); return -1; }
    for(u64 i = 0; i < outn; i++){
        u64 f = i * rate / SCRIBE_RATE;                    /* nearest-lower resample: fine for speech */
        if(f >= frames) f = frames - 1;
        float acc = 0;
        for(u32 c = 0; c < ch; c++){
            const u8 *p = data + (f * ch + c) * bps;
            float v;
            if(fmt == 3){ union { u32 u; float f; } x; x.u = rd32(p); v = x.f; }
            else if(bits == 8) v = ((int)p[0] - 128) / 128.0f;
            else if(bits == 16) v = (i16)rd16(p) / 32768.0f;
            else if(bits == 24) v = (float)((i32)((u32)p[0] << 8 | (u32)p[1] << 16 | (u32)p[2] << 24) >> 8) / 8388608.0f;
            else v = (float)(i32)rd32(p) / 2147483648.0f;
            acc += v;
        }
        pcm[i] = acc / ch;
    }
    int r = scribe_transcribe(pcm, (u32)outn, out, cap);
    kfree(pcm);
    return r;
}
