/* MVP — Mad Vid Player: a video player.
 *
 * Plays two kinds of file:
 *  - MP4 (fragmented, as video sites serve it) and WebM, with H.264, VP9 or AV1 video and
 *    AAC or Opus sound: MVP feeds the file to Reel, the media process, a piece at a time as
 *    playback goes (the way a page feeds MSE), and Reel decodes it; the kernel's side of
 *    Reel keeps the picture on the sound card's clock. Extensions .MP4 .M4V .M4A .WEB .MKV.
 *  - AVI with Motion-JPEG video and PCM audio: every frame decoded by the OS's own JPEG
 *    decoder, the sound to the HD Audio driver, the audio clock keeping them in sync.
 *
 * Library: those files in VIDEOS/ and in the root of the disk.
 * Keys: Space play/pause, Left/Right seek 5 s, Up/Down volume, F fullscreen,
 *       M mute, Esc back to the library. Click the bar to seek. */
#include "amdroid/ui.h"
#include "amdroid/fat32.h"
#include "amdroid/hda.h"
#include "amdroid/heap.h"
#include "amdroid/timer.h"
#include "amdroid/keyboard.h"
#include "amdroid/fb.h"
#include "amdroid/reel.h"
#include "string.h"

int  jpeg_decode_rgb(const u8 *data, u32 len, u32 **px, u32 *w, u32 *h);   /* web/image.c */
void *jpeg_job_start(const u8 *data, u32 len, int scale);                   /* decoding in steps */
void  jpeg_job_size(void *job, u32 *w, u32 *h);
int   jpeg_job_step(void *job, int work);
u32  *jpeg_job_take(void *job, u32 *w, u32 *h);
void  jpeg_job_abort(void *job);
void win_toggle_max(int a);
int  win_is_max(int a);
extern const app_t app_mvp;

/* ---- the library ---- */
#define MAX_FILES 32
static char files[MAX_FILES][24];
static u32 file_size[MAX_FILES];
static int nfiles = 0;
static u32 *thumb[MAX_FILES]; static u32 thumb_w[MAX_FILES], thumb_h[MAX_FILES];
static u32 file_secs[MAX_FILES];
static int thumbs_done = 0;                 /* files looked at so far (one per tick) */
#define LIB_ROW_H 104
static int lib_hover = -1;

/* ---- the open video ---- */
typedef struct { u32 off, len; } chunk_t;
static fat32_file_t vf;
static int have_video = 0;
static char cur_name[24];
static chunk_t *vframes = NULL; static int nvframes = 0, cap_v = 0;
static chunk_t *achunks = NULL; static int nachunks = 0, cap_a = 0;
static u32 vw = 0, vh = 0;                  /* the video's size */
static u32 us_per_frame = 66666;            /* 15 fps */
static int has_audio = 0;
static u32 a_rate = 0, a_ch = 0, a_bits = 0; /* PCM format of the file */
static u64 a_total = 0;                     /* audio bytes in the file */
static char err[96] = "";

/* ---- playback ---- */
static int playing = 0;
static u64 pos_us = 0;                      /* the position when the clock was (re)started */
static u64 clock_t0 = 0;                    /* timer ticks then (no audio) */
static int audio_on = 0;                    /* the sound card is playing our stream */
static u64 a_cursor = 0;                    /* next audio byte of the file to send */
static int a_chunk = 0; static u32 a_in = 0; /* where a_cursor is: chunk and offset */
static int volume = 100;                    /* percent, 0..200 */
static int muted = 0;
static int shown = -1;                      /* the frame on screen */
static u32 *frame_px = NULL;                /* its pixels (video size) */
static u32 *scaled = NULL; static u32 sw = 0, sh = 0;   /* scaled to the window */
static u8 *rdbuf = NULL; static u32 rdcap = 0;
static u32 decode_ms_avg = 0;               /* how long a frame takes to decode */
static int dropped = 0;
static int theater = 0;                     /* controls hidden (F in a maximized window) */
static u32 area_w = 0, area_h = 0;          /* where the picture goes (from the last draw) */
static int shown_scale = 0;                 /* the scale the frame on screen was decoded at */
static int smooth = 0;                      /* 'scaled' is the smooth (bilinear) version */
static void *job = NULL;                    /* the frame being decoded, a little each tick */
static int job_scale = 0;
static int job_frame = -1;
static u64 job_cycles = 0;                  /* CPU cycles spent on it */
static u32 fps_shown = 0, fps_count = 0; static u64 fps_t0 = 0;
static u64 last_clock = 0;                  /* the clock never runs backwards */
static int audio_done = 0;                  /* the file's audio is all sent: silence follows */
static int *xmap = NULL; static u32 xmap_w = 0, xmap_src = 0;
/* ---- an MP4/WebM through Reel ---- */
static int rm = 0;                          /* the open file plays through Reel */
static u32 rm_off = 0;                      /* bytes of it handed to Reel so far */
static int rm_eos = 0;                      /* all of it handed over */
static u32 rm_err_seq = 0;
static rect_t rm_pic = { 0, 0, 0, 0 };      /* where the last picture was drawn */
#define RM_PIECE (512u * 1024)              /* read and appended at a time */
#define RM_AHEAD 30000000                   /* keep this much (us) buffered ahead */

/* CPU cycles (for the decode time budget) */
static inline u64 rdtsc(void){ u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
static u64 tsc_per_tick = 0;               /* measured against the 100 Hz timer */
static u64 tsc_mark = 0, tick_mark = 0;
static void calibrate(void){
    u64 now = timer_ticks();
    if(!tick_mark){ tick_mark = now; tsc_mark = rdtsc(); return; }
    if(now >= tick_mark + 20){ tsc_per_tick = (rdtsc() - tsc_mark) / (now - tick_mark); tick_mark = now; tsc_mark = rdtsc(); }
}

static int decode_frame(int i);
static void set_playing(int p);
static u32 rd32(const u8 *p){ return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p){ return (u16)(p[0] | (p[1] << 8)); }
static int is4(const u8 *p, const char *s){ return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3]; }

static void *grow(void *old, int *cap, int need, u32 elem){
    if(need <= *cap) return old;
    int nc = *cap ? *cap * 2 : 256;
    while(nc < need) nc *= 2;
    void *n = kmalloc((u32)nc * elem);
    if(!n) return NULL;
    if(old){ memcpy(n, old, (u32)*cap * elem); kfree(old); }
    *cap = nc;
    return n;
}

/* ---- library ---- */
static int ext_is(const char *n, const char *e){
    u32 L = ui_len(n);
    if(L < 5 || n[L - 4] != '.') return 0;
    for(int i = 0; i < 3; i++) if((n[L - 3 + i] | 32) != e[i]) return 0;
    return 1;
}
static int has_avi_ext(const char *n){ return ext_is(n, "avi"); }
/* the containers Reel plays */
static int has_media_ext(const char *n){
    return ext_is(n, "mp4") || ext_is(n, "m4v") || ext_is(n, "m4a") || ext_is(n, "web") || ext_is(n, "mkv");
}
static const char *list_prefix;
static void add_file(const char *name, int is_dir, u32 size){
    if(is_dir || !(has_avi_ext(name) || has_media_ext(name)) || nfiles >= MAX_FILES) return;
    char full[24] = "";
    ui_cat(full, list_prefix);
    ui_cat(full, name);
    ui_copy(files[nfiles], full, sizeof files[0]);
    file_size[nfiles] = size;
    nfiles++;
}
static void scan_library(void){
    for(int i = 0; i < nfiles; i++) if(thumb[i]){ kfree(thumb[i]); thumb[i] = NULL; }
    thumbs_done = 0;
    nfiles = 0;
    list_prefix = "VIDEOS/";
    fat32_list_path("VIDEOS", add_file);
    list_prefix = "";
    fat32_list_root(add_file);
}

/* ---- thumbnails: the first frame at 1/8 (or so) size, and the length ---- */
static void make_thumb(int i){
    if(has_media_ext(files[i])) return;     /* no thumbnail without decoding: a play badge */
    fat32_file_t f;
    if(fat32_open(files[i], &f) != 0) return;
    u8 h[12];
    u32 off = 12, frames = 0, usf = 66666;
    /* top-level chunks: hdrl (with avih) and movi (with the first frame) */
    while(off + 12 <= f.size && fat32_read_at(&f, off, h, 12) == 12){
        u32 size = (u32)h[4] | ((u32)h[5] << 8) | ((u32)h[6] << 16) | ((u32)h[7] << 24);
        if(h[0] == 'L' && h[1] == 'I' && h[2] == 'S' && h[3] == 'T'){
            if(h[8] == 'h' && h[9] == 'd' && h[10] == 'r' && h[11] == 'l'){
                u8 a[48];
                if(fat32_read_at(&f, off + 12, a, 48) == 48 && a[0] == 'a' && a[1] == 'v' && a[2] == 'i' && a[3] == 'h'){
                    usf = (u32)a[8] | ((u32)a[9] << 8) | ((u32)a[10] << 16) | ((u32)a[11] << 24);
                    frames = (u32)a[24] | ((u32)a[25] << 8) | ((u32)a[26] << 16) | ((u32)a[27] << 24);
                }
            } else if(h[8] == 'm' && h[9] == 'o' && h[10] == 'v' && h[11] == 'i'){
                /* the first video chunk in movi */
                u32 o = off + 12, e = off + 8 + size;
                while(o + 8 <= e && fat32_read_at(&f, o, h, 8) == 8){
                    u32 cs = (u32)h[4] | ((u32)h[5] << 8) | ((u32)h[6] << 16) | ((u32)h[7] << 24);
                    if(h[2] == 'd' && (h[3] == 'c' || h[3] == 'b') && cs > 0 && cs < 4u << 20){
                        u8 *buf = kmalloc(cs);
                        if(buf && fat32_read_at(&f, o + 8, buf, cs) == (int)cs){
                            void *j = jpeg_job_start(buf, cs, 0);
                            if(j){
                                u32 W, H; jpeg_job_size(j, &W, &H);
                                jpeg_job_abort(j);
                                int sc = 0; while(sc < 3 && (W >> (sc + 1)) >= 140) sc++;
                                j = jpeg_job_start(buf, cs, sc);
                                if(j){
                                    int r; while((r = jpeg_job_step(j, 1 << 20)) == 0) ;
                                    if(r == 1) thumb[i] = jpeg_job_take(j, &thumb_w[i], &thumb_h[i]);
                                    else jpeg_job_abort(j);
                                }
                            }
                        }
                        if(buf) kfree(buf);
                        break;
                    }
                    if(h[0] == 'L' && h[1] == 'I' && h[2] == 'S' && h[3] == 'T') o += 12;
                    else o += 8 + cs + (cs & 1);
                }
                break;
            }
        }
        off += 8 + size + (size & 1);
    }
    file_secs[i] = (u32)((u64)frames * usf / 1000000);
    fat32_close(&f);
}

/* ---- opening an AVI ---- */
static void close_video(void){
    if(rm){ reel_end(&app_mvp); rm = 0; }
    if(audio_on){ hda_stream_stop(); audio_on = 0; }
    if(job){ jpeg_job_abort(job); job = NULL; job_frame = -1; }
    if(have_video) fat32_close(&vf);
    have_video = 0; playing = 0;
    if(scaled && scaled != frame_px) kfree(scaled);
    scaled = NULL; sw = sh = 0;
    if(frame_px){ kfree(frame_px); frame_px = NULL; }
    nvframes = nachunks = 0;
    shown = -1;
}

static int read_at(u32 off, void *buf, u32 len){ return fat32_read_at(&vf, off, buf, len) == (int)len ? 0 : -1; }

/* Walks the chunks in [off, end): headers, stream formats, and the movi list's frames. */
static int stream_kind[8];                  /* 1 video, 2 audio, per stream number */
static int cur_stream = -1;
static void walk(u32 off, u32 end, int depth){
    u8 h[12];
    while(off + 8 <= end && depth < 6){
        if(read_at(off, h, 12) != 0) return;
        u32 size = rd32(h + 4);
        if(is4(h, "LIST")){
            walk(off + 12, off + 8 + size, depth + 1);
        } else if(is4(h, "avih") && size >= 40){
            u8 a[40]; if(read_at(off + 8, a, 40) == 0){ us_per_frame = rd32(a); vw = rd32(a + 32); vh = rd32(a + 36); }
        } else if(is4(h, "strh") && size >= 32){
            u8 a[32];
            if(read_at(off + 8, a, 32) == 0){
                cur_stream++;
                if(cur_stream < 8) stream_kind[cur_stream] = is4(a, "vids") ? 1 : is4(a, "auds") ? 2 : 0;
                if(is4(a, "vids") && rd32(a + 20)) us_per_frame = (u32)((u64)1000000 * rd32(a + 20) / rd32(a + 24));
            }
        } else if(is4(h, "strf") && cur_stream >= 0 && cur_stream < 8 && stream_kind[cur_stream] == 2 && size >= 16){
            u8 a[16];
            if(read_at(off + 8, a, 16) == 0 && rd16(a) == 1){       /* WAVE_FORMAT_PCM */
                a_ch = rd16(a + 2); a_rate = rd32(a + 4); a_bits = rd16(a + 14);
                has_audio = a_bits == 16 && (a_ch == 1 || a_ch == 2) && a_rate >= 8000;
            }
        } else if(h[0] >= '0' && h[0] <= '9' && h[1] >= '0' && h[1] <= '9'){
            int sn = (h[0] - '0') * 10 + (h[1] - '0');
            int kind = sn < 8 ? stream_kind[sn] : 0;
            if(kind == 1 && size){
                chunk_t *n = grow(vframes, &cap_v, nvframes + 1, sizeof(chunk_t));
                if(n){ vframes = n; vframes[nvframes].off = off + 8; vframes[nvframes].len = size; nvframes++; }
            } else if(kind == 2 && size){
                chunk_t *n = grow(achunks, &cap_a, nachunks + 1, sizeof(chunk_t));
                if(n){ achunks = n; achunks[nachunks].off = off + 8; achunks[nachunks].len = size; nachunks++; a_total += size; }
            }
        }
        off += 8 + size + (size & 1);
    }
}

static void open_media(void);
static void open_video(const char *name){
    close_video();
    err[0] = 0;
    if(fat32_open(name, &vf) != 0){ ui_copy(err, "Can't open that file.", sizeof err); return; }
    have_video = 1;
    ui_copy(cur_name, name, sizeof cur_name);
    if(has_media_ext(name)){ open_media(); return; }
    u8 h[12];
    if(read_at(0, h, 12) != 0 || !is4(h, "RIFF") || !is4(h + 8, "AVI ")){
        ui_copy(err, "Not an AVI file.", sizeof err); close_video(); return;
    }
    for(int i = 0; i < 8; i++) stream_kind[i] = 0;
    cur_stream = -1; has_audio = 0; a_total = 0; vw = vh = 0; us_per_frame = 66666;
    walk(12, vf.size, 0);
    if(!nvframes){ ui_copy(err, "No video frames (MVP plays Motion-JPEG AVI).", sizeof err); close_video(); return; }
    if(us_per_frame < 1000) us_per_frame = 66666;
    pos_us = 0; a_cursor = 0; a_chunk = 0; a_in = 0; dropped = 0; decode_ms_avg = 0;
    playing = 0;
    decode_frame(0);
    set_playing(1);
}

/* ---- time ---- */
static u64 duration_us(void){
    if(rm){ i64 d = reel_duration(); return d > 0 ? (u64)d : 0; }
    return (u64)nvframes * us_per_frame;
}
static u64 audio_bps(void){ return (u64)a_rate * a_ch * 2; }
static u64 now_us(void){
    if(rm) return (u64)reel_time();
    if(!playing) return pos_us;
    u64 t;
    if(audio_on){
        /* the audio clock: what the speaker has played (48 kHz stereo: 192000 bytes/s) */
        t = pos_us + hda_stream_played() * 1000000 / 192000;
    } else t = pos_us + (timer_ticks() - clock_t0) * 10000;
    if(t < last_clock) t = last_clock;          /* never backwards */
    last_clock = t;
    return t;
}

/* moves the audio cursor to time t */
static void audio_seek(u64 t){
    if(!has_audio) return;
    u64 want = t * audio_bps() / 1000000;
    want &= ~(u64)(a_ch * 2 - 1);
    a_cursor = 0; a_chunk = 0; a_in = 0;
    while(a_chunk < nachunks && a_cursor + achunks[a_chunk].len <= want){ a_cursor += achunks[a_chunk].len; a_chunk++; }
    if(a_chunk < nachunks){ a_in = (u32)(want - a_cursor); a_cursor = want; }
}

/* Converts the file's PCM to 48 kHz stereo at the volume and sends what fits. */
static void feed_audio(void){
    if(!audio_on) return;
    static int16_t in[4096], out[8192];
    for(int rounds = 0; rounds < 16; rounds++){
        u32 room = hda_stream_room();
        if(room < 4096) return;
        if(a_chunk >= nachunks){
            /* the soundtrack is over: keep the ring quiet (it would replay old sound) */
            audio_done = 1;
            static const int16_t zero[2048] = {0};
            hda_stream_write(zero, room < sizeof zero ? room : sizeof zero);
            continue;
        }
        /* how many input bytes make about 'room' output bytes */
        u32 in_frame = a_ch * 2;
        u32 want_out_frames = room / 4; if(want_out_frames > 2048) want_out_frames = 2048;
        u32 want_in_frames = (u32)((u64)want_out_frames * a_rate / 48000);
        if(want_in_frames == 0) want_in_frames = 1;
        u32 left_in_chunk = achunks[a_chunk].len - a_in;
        u32 bytes = want_in_frames * in_frame;
        if(bytes > left_in_chunk) bytes = left_in_chunk;
        if(bytes > sizeof in) bytes = sizeof in;
        bytes -= bytes % in_frame;
        if(bytes == 0){ a_chunk++; a_in = 0; continue; }
        if(fat32_read_at(&vf, achunks[a_chunk].off + a_in, in, bytes) != (int)bytes) return;
        u32 nin = bytes / in_frame;
        u32 nout = (u32)((u64)nin * 48000 / a_rate);
        if(nout > 4096) nout = 4096;
        int vol = (muted ? 0 : volume) * ui_prefs.volume / 100;   /* the system volume (Control Center) scales it */
        for(u32 i = 0; i < nout; i++){
            u32 j = (u32)((u64)i * a_rate / 48000);
            if(j >= nin) j = nin - 1;
            int l = in[j * a_ch], r = a_ch == 2 ? in[j * 2 + 1] : l;
            l = l * vol / 100; r = r * vol / 100;
            if(l > 32767) l = 32767; if(l < -32768) l = -32768;
            if(r > 32767) r = 32767; if(r < -32768) r = -32768;
            out[i * 2] = (int16_t)l; out[i * 2 + 1] = (int16_t)r;
        }
        hda_stream_write(out, nout * 4);
        a_in += bytes; a_cursor += bytes;
        if(a_in >= achunks[a_chunk].len){ a_chunk++; a_in = 0; }
    }
}

static void start_clock(void){
    clock_t0 = timer_ticks();
    last_clock = pos_us;
    audio_done = 0;
    audio_on = 0;
    if(has_audio && hda_present() && hda_stream_start() == 0){
        audio_on = 1;
        audio_seek(pos_us);
        feed_audio();
    }
}
static void pause_clock(void){
    pos_us = now_us();
    if(audio_on){ hda_stream_stop(); audio_on = 0; }
}
static void set_playing(int p){
    if(p == playing) return;
    if(rm){
        if(p){ if(reel_ended()) reel_seek(0); reel_play(); }
        else reel_pause();
        playing = p;
        ui_redraw();
        return;
    }
    if(p){ if(pos_us >= duration_us()) pos_us = 0; playing = 1; start_clock(); }
    else {
        pause_clock(); playing = 0;
        if(job){ jpeg_job_abort(job); job = NULL; job_frame = -1; }
        if(shown >= 0 && shown_scale > 0) decode_frame(shown);   /* a sharp still */
    }
    ui_redraw();
}
static void rm_seek(i64 t);
static void seek_to(i64 t){
    if(t < 0) t = 0;
    if(duration_us() && (u64)t > duration_us()) t = (i64)duration_us();
    if(rm){ rm_seek(t); ui_redraw(); return; }
    int was = playing;
    if(was) pause_clock();
    pos_us = (u64)t;
    last_clock = pos_us;
    if(job){ jpeg_job_abort(job); job = NULL; job_frame = -1; }
    playing = was;
    if(was) start_clock();
    shown = -1;
    ui_redraw();
}

/* ---- frames ---- */
static int decode_frame(int i){
    chunk_t *c = &vframes[i];
    if(c->len > rdcap){
        if(rdbuf) kfree(rdbuf);
        rdcap = c->len + 65536;
        rdbuf = kmalloc(rdcap);
        if(!rdbuf){ rdcap = 0; return -1; }
    }
    if(fat32_read_at(&vf, c->off, rdbuf, c->len) != (int)c->len) return -1;
    u32 *px = NULL, w = 0, h = 0;
    u64 t0 = timer_ticks();
    if(jpeg_decode_rgb(rdbuf, c->len, &px, &w, &h) != 0 || !px) return -1;
    u32 ms = (u32)((timer_ticks() - t0) * 10);
    decode_ms_avg = decode_ms_avg ? (decode_ms_avg * 7 + ms) / 8 : ms;
    if(scaled == frame_px) scaled = NULL;
    if(frame_px) kfree(frame_px);
    frame_px = px; vw = w; vh = h;
    sw = sh = 0; smooth = 0;         /* scale again */
    shown = i;
    shown_scale = 0;
    return 0;
}

/* a finished frame from the stepping decoder */
static void show_decoded(u32 *px, u32 w, u32 h, int i, int sc){
    if(scaled == frame_px) scaled = NULL;
    if(frame_px) kfree(frame_px);
    frame_px = px; vw = w; vh = h;
    sw = sh = 0; smooth = 0;
    shown = i;
    shown_scale = sc;
    fps_count++;
}

/* scales the frame to fit w x h (keeping its shape) into 'scaled' */
/* bilinear: smooth, for a still picture (too slow to do for every frame) */
static void scale_smooth(u32 dw, u32 dh){
    for(u32 y = 0; y < dh; y++){
        u32 fy = (u32)(((u64)y * (vh - 1) << 8) / (dh > 1 ? dh - 1 : 1));
        u32 y0 = fy >> 8, y1 = y0 + 1 < vh ? y0 + 1 : y0, wy = fy & 255;
        const u32 *r0 = frame_px + (u64)y0 * vw, *r1 = frame_px + (u64)y1 * vw;
        u32 *o = scaled + (u64)y * dw;
        for(u32 x = 0; x < dw; x++){
            u32 fx = (u32)(((u64)x * (vw - 1) << 8) / (dw > 1 ? dw - 1 : 1));
            u32 x0 = fx >> 8, x1 = x0 + 1 < vw ? x0 + 1 : x0, wx = fx & 255;
            u32 a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1], out = 0;
            for(int sh8 = 0; sh8 < 24; sh8 += 8){
                u32 ca = (a >> sh8) & 255, cb = (b >> sh8) & 255, cc = (c >> sh8) & 255, cd = (d >> sh8) & 255;
                u32 top = ca * (256 - wx) + cb * wx, bot = cc * (256 - wx) + cd * wx;
                out |= (((top * (256 - wy) + bot * wy) >> 16) & 255) << sh8;
            }
            o[x] = out;
        }
    }
}
static void scale_to(u32 w, u32 h){
    if(!frame_px || !vw || !vh) return;
    u32 dw = w, dh = (u32)((u64)w * vh / vw);
    if(dh > h){ dh = h; dw = (u32)((u64)h * vw / vh); }
    int want_smooth = !playing;
    if(dw == sw && dh == sh && scaled && (smooth || !want_smooth)) return;
    if(dw == vw && dh == vh){                 /* the window fits the video: no scaling */
        if(scaled && scaled != frame_px) kfree(scaled);
        scaled = frame_px; sw = dw; sh = dh;
        return;
    }
    if(scaled && scaled != frame_px) kfree(scaled);
    scaled = kmalloc(dw * dh * 4);
    if(!scaled){ sw = sh = 0; return; }
    if(want_smooth){ scale_smooth(dw, dh); sw = dw; sh = dh; smooth = 1; return; }
    smooth = 0;
    if(xmap_w != dw || xmap_src != vw){
        if(xmap) kfree(xmap);
        xmap = kmalloc(sizeof(int) * dw);
        if(!xmap){ xmap_w = 0; return; }
        for(u32 x = 0; x < dw; x++) xmap[x] = (int)((u64)x * vw / dw);
        xmap_w = dw; xmap_src = vw;
    }
    for(u32 y = 0; y < dh; y++){
        const u32 *row = frame_px + (u64)(y * vh / dh) * vw;
        u32 *o = scaled + (u64)y * dw;
        for(u32 x = 0; x < dw; x++) o[x] = row[xmap[x]];
    }
    sw = dw; sh = dh;
}

/* ---- MP4/WebM: the file goes to Reel as playback needs it ---- */
static i64 buffered_end(i64 at){
    const volatile reel_shm_t *m = reel_header();
    if(!m) return -1;
    const volatile reel_sb_status_t *st = &m->sb[0];
    for(u32 k = 0; k < st->nranges && k < REEL_RANGES; k++)
        if(at >= st->ranges[k * 2] - 200000 && at < st->ranges[k * 2 + 1]) return st->ranges[k * 2 + 1];
    return -1;
}
static i64 buffered_start(void){
    const volatile reel_shm_t *m = reel_header();
    return m && m->sb[0].nranges ? m->sb[0].ranges[0] : -1;
}

static void feed_media(void){
    if(!rm) return;
    if(rm_off >= vf.size){
        if(!rm_eos && !reel_append_pending(0)){ reel_eos(); rm_eos = 1; }
        return;
    }
    if(reel_queued_bytes() >= 2 * RM_PIECE) return;       /* Reel is still on the last pieces */
    const volatile reel_shm_t *m = reel_header();
    if(m && m->sb[0].have_init){
        i64 now = reel_time(), end = buffered_end(now);
        if(end >= 0 && end - now > RM_AHEAD) return;
    }
    u32 n = vf.size - rm_off;
    if(n > RM_PIECE) n = RM_PIECE;
    if(n > rdcap){
        if(rdbuf) kfree(rdbuf);
        rdcap = RM_PIECE;
        rdbuf = kmalloc(rdcap);
        if(!rdbuf){ rdcap = 0; return; }
    }
    if(fat32_read_at(&vf, rm_off, rdbuf, n) != (int)n){ ui_copy(err, "Reading the file failed.", sizeof err); return; }
    if(reel_append(0, rdbuf, n) != 0) return;               /* no memory now: next tick */
    rm_off += n;
}

/* inside what is buffered: at once. Ahead of it: the data comes as the file is read on.
 * Behind it (Reel forgets what is long played): the file is fed again from the start. */
static void rm_seek(i64 t){
    i64 start = buffered_start();
    if(start >= 0 && t + 200000 < start){
        reel_abort(0);
        rm_off = 0;
        rm_eos = 0;
    }
    reel_seek(t);
}

static void open_media(void){
    if(reel_begin(&app_mvp) != 0){
        ui_copy(err, "The media process could not start (out of memory?).", sizeof err);
        close_video();
        return;
    }
    rm = 1; rm_off = 0; rm_eos = 0;
    reel_error(&rm_err_seq);
    playing = 0;
    feed_media();
    set_playing(1);
}

static void rm_tick(void){
    reel_set_volume((muted ? 0 : volume) * ui_prefs.volume / 100);
    feed_media();
    if(reel_tick()){
        fps_count++;
        u32 pw, ph;
        /* a new picture of the same size: its box only (the controls change once a second) */
        if(rm_pic.w && reel_picture(&pw, &ph) && pw == rm_pic.w && ph == rm_pic.h) ui_redraw_rect(rm_pic);
        else ui_redraw();
    }
    if(timer_ticks() >= fps_t0 + 100){ fps_shown = fps_count; fps_count = 0; fps_t0 = timer_ticks(); if(playing) ui_redraw(); }
    u32 seq;
    const char *e = reel_error(&seq);
    if(seq != rm_err_seq){ rm_err_seq = seq; ui_copy(err, e, sizeof err); ui_redraw(); }
    if(!reel_alive()){
        if(!err[0]) ui_copy(err, "The media process stopped.", sizeof err);
        close_video(); scan_library(); ui_redraw();
        return;
    }
    if(playing && reel_ended()){ reel_pause(); playing = 0; ui_redraw(); }
}

static void mvp_tick(void){
    calibrate();
    if(have_video && rm){ rm_tick(); return; }
    if(!have_video && thumbs_done < nfiles){ make_thumb(thumbs_done++); ui_redraw(); return; }
    if(!have_video || !playing) return;
    feed_audio();
    u64 t = now_us();
    if(t >= duration_us()){
        pos_us = duration_us(); playing = 0;
        if(audio_on){ hda_stream_stop(); audio_on = 0; }
        if(job){ jpeg_job_abort(job); job = NULL; job_frame = -1; }
        ui_redraw();
        return;
    }
    int want = (int)(t / us_per_frame);
    if(want >= nvframes) want = nvframes - 1;
    if(timer_ticks() >= fps_t0 + 100){ fps_shown = fps_count; fps_count = 0; fps_t0 = timer_ticks(); }
    if(!job && want != shown){
        /* start on the frame that is due now (frames in between are skipped) */
        if(shown >= 0 && want > shown + 1) dropped += want - shown - 1;
        chunk_t *c = &vframes[want];
        if(c->len > rdcap){
            if(rdbuf) kfree(rdbuf);
            rdcap = c->len + 65536;
            rdbuf = kmalloc(rdcap);
            if(!rdbuf){ rdcap = 0; return; }
        }
        if(fat32_read_at(&vf, c->off, rdbuf, c->len) != (int)c->len) return;
        /* decode only as big as the window needs: 1/2, 1/4 size is far less work */
        int sc = 0;
        if(area_w && vw){
            u32 fullw = vw << shown_scale, fullh = vh << shown_scale;
            /* the size the picture is shown at (it keeps its shape) */
            u32 dw = area_w, dh = (u32)((u64)area_w * fullh / fullw);
            if(dh > area_h){ dh = area_h; dw = (u32)((u64)area_h * fullw / fullh); }
            /* smaller is fine while it is stretched by no more than 1.4x */
            while(sc < 2 && (fullw >> (sc + 1)) * 10 >= dw * 7) sc++;
        }
        job = jpeg_job_start(rdbuf, c->len, sc);
        job_scale = sc;
        job_frame = want;
        job_cycles = 0;
        if(!job){ shown = want; return; }       /* a broken frame: skip it */
    }
    if(job){
        /* decode for up to 3/4 of a tick, then give the GUI its turn */
        u64 budget = tsc_per_tick ? tsc_per_tick * 3 / 4 : 0;
        u64 t0 = rdtsc();
        for(;;){
            int r = jpeg_job_step(job, 2);
            if(r != 0){
                if(r == 1){
                    u32 w, h; u32 *px = jpeg_job_take(job, &w, &h);
                    if(tsc_per_tick){ u32 ms = (u32)((job_cycles + rdtsc() - t0) * 10 / tsc_per_tick); decode_ms_avg = decode_ms_avg ? (decode_ms_avg * 7 + ms) / 8 : ms; }
                    show_decoded(px, w, h, job_frame, job_scale);
                    ui_redraw();
                } else jpeg_job_abort(job);
                job = NULL; job_frame = -1;
                break;
            }
            if(!budget || rdtsc() - t0 > budget) break;
        }
        if(job) job_cycles += rdtsc() - t0;
    }
}

/* ---- drawing ---- */
#define BAR_H 72
static void fmt_time(u64 us, char *out){
    u32 s = (u32)(us / 1000000), m = s / 60;
    char n[12];
    out[0] = 0;
    ui_num(m, n); ui_cat(out, n); ui_cat(out, ":");
    if(s % 60 < 10) ui_cat(out, "0");
    ui_num(s % 60, n); ui_cat(out, n);
}
static rect_t seek_rect(rect_t b){ return (rect_t){ b.x + 16, b.y + b.h - BAR_H + 10, b.w - 32, 10 }; }
static rect_t play_rect(rect_t b){ return (rect_t){ b.x + 16, b.y + b.h - 44, 40, 36 }; }

static void draw_library(rect_t b){
    if(!ui_prefs.glass) ui_fill(b, C_WIN);       /* (glass: the window is already drawn) */
    ui_text(b.x + UI_PAD, b.y + 16, "MVP", ui_accent(), 3);
    ui_text(b.x + UI_PAD + 3 * 8 * 3 + 16, b.y + 30, "Mad Vid Player", C_MUTED, UI_SCALE);
    u32 y = b.y + 88;
    if(err[0]){ ui_text(b.x + UI_PAD, y, err, C_WARN, UI_SCALE); y += UI_ROW_H; }
    if(!nfiles){
        ui_text(b.x + UI_PAD, y, "No videos yet.", C_TEXT, UI_SCALE);
        /* the hint, wrapped at the window's edge */
        const char *hint = "Put .MP4 / .WEB (H.264, VP9, AV1 + AAC, Opus) or .AVI (Motion-JPEG) files in VIDEOS/.";
        u32 per = (b.w - 2 * UI_PAD) / UI_CW;           /* characters a line holds */
        if(per < 8) per = 8;
        y += 40;
        while(*hint){
            u32 n = 0, brk = 0;
            while(hint[n] && n < per){ if(hint[n] == ' ') brk = n; n++; }
            if(hint[n] && brk) n = brk;                  /* break at the last space that fits */
            char line[96];
            u32 m = n < sizeof line - 1 ? n : (u32)sizeof line - 1;
            for(u32 i = 0; i < m; i++) line[i] = hint[i];
            line[m] = 0;
            ui_text(b.x + UI_PAD, y, line, C_MUTED, UI_SCALE);
            y += UI_ROW_H - 4;
            hint += n;
            while(*hint == ' ') hint++;
        }
        return;
    }
    ui_text(b.x + UI_PAD, y, "Your videos", C_MUTED, UI_SCALE);
    y += UI_ROW_H;
    for(int i = 0; i < nfiles && y + LIB_ROW_H <= b.y + b.h; i++, y += LIB_ROW_H + 8){
        rect_t row = { b.x + UI_PAD, y, b.w - 2 * UI_PAD, LIB_ROW_H };
        ui_rrect(row.x, row.y, row.w, row.h, 10, i == lib_hover ? C_HOVER : C_CARD);
        /* the thumbnail (or a play badge while it is being made) */
        u32 tx = row.x + 8, ty = row.y + 7, tw = 160, th = 90;
        ui_fill((rect_t){ tx, ty, tw, th }, 0x000000);
        if(thumb[i]){
            u32 w = thumb_w[i] < tw ? thumb_w[i] : tw, h = thumb_h[i] < th ? thumb_h[i] : th;
            fb_blit_over((i32)(tx + (tw - w) / 2), (i32)(ty + (th - h) / 2), w, h, thumb[i]);
        } else {
            ui_disc((int)(tx + tw / 2), (int)(ty + th / 2), 16, ui_accent());
            ui_line((int)(tx + tw / 2 - 5), (int)(ty + th / 2 - 8), (int)(tx + tw / 2 - 5), (int)(ty + th / 2 + 8), 3, C_WIN);
            ui_line((int)(tx + tw / 2 - 5), (int)(ty + th / 2 - 8), (int)(tx + tw / 2 + 8), (int)(ty + th / 2), 3, C_WIN);
            ui_line((int)(tx + tw / 2 - 5), (int)(ty + th / 2 + 8), (int)(tx + tw / 2 + 8), (int)(ty + th / 2), 3, C_WIN);
        }
        char num[4] = { (char)('1' + i), 0 };
        ui_text(tx + tw + 20, row.y + 14, i < 9 ? num : "", ui_accent(), UI_SCALE);
        ui_text(tx + tw + 52, row.y + 14, files[i], C_TEXT, UI_SCALE);
        char meta[40] = "", n[12];
        if(file_secs[i]){ fmt_time((u64)file_secs[i] * 1000000, meta); ui_cat(meta, "   "); }
        ui_num(file_size[i] / (1024 * 1024), n); ui_cat(meta, n); ui_cat(meta, " MB");
        ui_text(tx + tw + 52, row.y + 54, meta, C_MUTED, UI_SCALE);
    }
}

static const char *codec_name(u32 c){
    switch(c){ case 1: return "H.264"; case 2: return "VP9"; case 3: return "AV1"; case 4: return "AAC"; case 5: return "Opus"; }
    return "";
}

static void draw_player(rect_t b){
    ui_fill(b, 0x000000);
    u32 pic_h = theater ? b.h : b.h - BAR_H;
    area_w = b.w; area_h = pic_h;
    if(rm){
        const volatile reel_shm_t *m = reel_header();
        u32 vw2 = m ? m->video_w : 0, vh2 = m ? m->video_h : 0;
        if(vw2 && vh2){
            u32 dw = b.w, dh = (u32)((u64)b.w * vh2 / vw2);
            if(dh > pic_h){ dh = pic_h; dw = (u32)((u64)pic_h * vw2 / vh2); }
            reel_set_size(dw, dh, !playing);         /* a paused picture is drawn smooth */
        }
        u32 pw, ph;
        if(reel_picture(&pw, &ph)){
            rm_pic = (rect_t){ b.x + (b.w > pw ? (b.w - pw) / 2 : 0), b.y + (pic_h > ph ? (pic_h - ph) / 2 : 0), pw, ph };
            reel_blit(rm_pic.x, rm_pic.y, pw, ph);
        } else ui_text(b.x + b.w / 2 - 80, b.y + pic_h / 2 - 16, m && !m->has_video && m->has_audio ? "(sound only)" : "Loading...", C_MUTED, UI_SCALE);
    } else if(frame_px){
        scale_to(b.w, pic_h);
        if(scaled) fb_blit_over((i32)(b.x + (b.w - sw) / 2), (i32)(b.y + (pic_h - sh) / 2), sw, sh, scaled);
    } else {
        ui_text(b.x + b.w / 2 - 80, b.y + pic_h / 2 - 16, "Loading...", C_MUTED, UI_SCALE);
    }
    if(theater) return;
    /* the control bar */
    rect_t bar = { b.x, b.y + b.h - BAR_H, b.w, BAR_H };
    ui_fill(bar, 0x111417);
    rect_t sr = seek_rect(b);
    u64 t = now_us(), d = duration_us();
    u32 done = d ? (u32)((u64)sr.w * t / d) : 0;
    if(done > sr.w) done = sr.w;
    ui_rrect(sr.x, sr.y, sr.w, sr.h, 5, 0x3A4450);
    ui_rrect(sr.x, sr.y, done ? done : 1, sr.h, 5, 0xFF7A00);
    ui_disc((int)(sr.x + done), (int)(sr.y + 5), 8, 0xFFFFFF);
    rect_t pr = play_rect(b);
    if(playing){
        ui_rrect(pr.x + 10, pr.y + 6, 7, 24, 2, C_TEXT);
        ui_rrect(pr.x + 23, pr.y + 6, 7, 24, 2, C_TEXT);
    } else {
        ui_line((int)pr.x + 12, (int)pr.y + 6, (int)pr.x + 12, (int)pr.y + 30, 3, C_TEXT);
        ui_line((int)pr.x + 12, (int)pr.y + 6, (int)pr.x + 32, (int)pr.y + 18, 3, C_TEXT);
        ui_line((int)pr.x + 12, (int)pr.y + 30, (int)pr.x + 32, (int)pr.y + 18, 3, C_TEXT);
    }
    char tm[40], dd[16];
    fmt_time(t, tm); ui_cat(tm, " / "); fmt_time(d, dd); ui_cat(tm, dd);
    ui_text(pr.x + 56, pr.y + 2, tm, C_TEXT, UI_SCALE);

    /* volume, and what the player is doing */
    char info[64] = "";
    if(muted) ui_cat(info, "Muted");
    else { char n[8]; ui_num((u32)volume, n); ui_cat(info, "Vol "); ui_cat(info, n); ui_cat(info, "%"); }
    if(rm){
        const volatile reel_shm_t *m = reel_header();
        char n[12];
        if(m){
            if(m->has_video){ ui_cat(info, "   "); ui_cat(info, codec_name(m->sb[0].kind == 1 ? m->sb[0].codec : 0)); }
            if(m->has_video){ ui_cat(info, " "); ui_num(m->video_h, n); ui_cat(info, n); ui_cat(info, "p "); ui_num(fps_shown, n); ui_cat(info, n); ui_cat(info, " fps"); }
            if(m->dropped){ ui_cat(info, " "); ui_num((u32)m->dropped, n); ui_cat(info, n); ui_cat(info, " late"); }
            if(!m->has_audio) ui_cat(info, " (no audio)");
            else if(!hda_present()) ui_cat(info, " (no sound card)");
            if(reel_stalled()) ui_cat(info, " buffering");
        }
    } else if(!has_audio) ui_cat(info, " (no audio)");
    else if(!audio_on && playing) ui_cat(info, " (no sound card)");
    if(!rm){   /* what the decoder is doing */
        char n[12];
        ui_cat(info, "   ");
        ui_num(vh << shown_scale, n); ui_cat(info, n); ui_cat(info, "p");
        if(shown_scale){ ui_cat(info, "@1/"); ui_num(1u << shown_scale, n); ui_cat(info, n); }
        ui_cat(info, " ");
        ui_num(decode_ms_avg, n); ui_cat(info, n); ui_cat(info, " ms ");
        ui_num(fps_shown, n); ui_cat(info, n); ui_cat(info, " fps");
    }
    rect_t right = { bar.x, pr.y, bar.w - 16, 32 };
    ui_text_right(right, pr.y + 2, info, C_MUTED, UI_SCALE);
    /* the file name in whatever room is left between the time and the info */
    u32 name_x = pr.x + 56 + ui_text_w(tm, UI_SCALE) + 24;
    u32 info_x = bar.x + bar.w - 16 - ui_text_w(info, UI_SCALE);
    if(info_x > name_x + 24){
        u32 room = (info_x - name_x - 24) / UI_CW;
        char nm[24]; ui_copy(nm, cur_name, sizeof nm);
        if(ui_len(nm) > room){ if(room >= 3){ nm[room - 2] = '.'; nm[room - 1] = '.'; nm[room] = 0; } else nm[0] = 0; }
        ui_text(name_x, pr.y + 2, nm, C_MUTED, UI_SCALE);
    }
}

static void mvp_draw(rect_t body, int focused){
    (void)focused;
    if(have_video) draw_player(body); else draw_library(body);
}

/* ---- input ---- */
static void mvp_open(void){ scan_library(); err[0] = 0; theater = 0; }
static void mvp_close(void){ close_video(); }

static int mvp_key(int c){
    if(!have_video){
        if(c >= '1' && c <= '9' && c - '1' < nfiles){ open_video(files[c - '1']); ui_redraw(); return 1; }
        return 0;
    }
    int app = ui_app_index(&app_mvp);
    switch(c){
        case ' ': set_playing(!playing); return 1;
        case KEY_LEFT: seek_to((i64)now_us() - 5000000); return 1;
        case KEY_RIGHT: seek_to((i64)now_us() + 5000000); return 1;
        case KEY_UP: volume = volume + 10 > 200 ? 200 : volume + 10; ui_redraw(); return 1;
        case KEY_DOWN: volume = volume - 10 < 0 ? 0 : volume - 10; ui_redraw(); return 1;
        case 'm': case 'M': muted = !muted; ui_redraw(); return 1;
        case 'f': case 'F':
            if(app >= 0){
                if(!win_is_max(app)){ win_toggle_max(app); theater = 1; }
                else { theater = !theater; if(!theater) win_toggle_max(app); }
            }
            sw = sh = 0; ui_redraw(); return 1;
        case 27:
            if(theater){ theater = 0; if(app >= 0 && win_is_max(app)) win_toggle_max(app); sw = sh = 0; ui_redraw(); return 1; }
            close_video(); scan_library(); ui_redraw(); return 1;
    }
    return 0;
}

static void mvp_click(rect_t b, int x, int y){
    if(!have_video){
        u32 top = b.y + 88 + (err[0] ? UI_ROW_H : 0) + UI_ROW_H;
        for(int i = 0; i < nfiles; i++){
            rect_t row = { b.x + UI_PAD, top + (u32)i * (LIB_ROW_H + 8), b.w - 2 * UI_PAD, LIB_ROW_H };
            if(ui_contains(row, x, y)){ open_video(files[i]); ui_redraw(); return; }
        }
        return;
    }
    if(theater){ theater = 0; sw = sh = 0; ui_redraw(); return; }
    rect_t sr = seek_rect(b);
    rect_t hit = { sr.x, sr.y - 10, sr.w, sr.h + 20 };
    if(ui_contains(hit, x, y)){ seek_to((i64)((u64)(x - (int)sr.x) * duration_us() / sr.w)); return; }
    if(ui_contains(play_rect(b), x, y)){ set_playing(!playing); return; }
    if(y < (int)(b.y + b.h - BAR_H)) set_playing(!playing);     /* a click on the picture */
}

static void mvp_move(rect_t b, int x, int y){
    if(have_video) return;
    u32 top = b.y + 88 + (err[0] ? UI_ROW_H : 0) + UI_ROW_H;
    int h = -1;
    for(int i = 0; i < nfiles; i++){
        rect_t row = { b.x + UI_PAD, top + (u32)i * (LIB_ROW_H + 8), b.w - 2 * UI_PAD, LIB_ROW_H };
        if(ui_contains(row, x, y)) h = i;
    }
    if(h != lib_hover){ lib_hover = h; ui_redraw(); }
}

const app_t app_mvp = {
    .name = "MVP",
    .icon = UI_ICO_VIDEO,
    .open = mvp_open,
    .close = mvp_close,
    .draw = mvp_draw,
    .key = mvp_key,
    .click = mvp_click,
    .move = mvp_move,
    .tick = mvp_tick,
};
