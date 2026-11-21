/* dictation.c — the microphone -> Scribe, for dictation (Ctrl+Space) and voice commands (Alt+Space).
 *
 * Capture is drained every GUI turn (the sound card's ring holds 1.36 s); stopping turns 48 kHz
 * stereo into 16 kHz mono and hands it to Scribe without waiting.
 *  - Dictation: the text comes back and is typed into the focused window through keyboard_inject,
 *    a few dozen characters a turn so the key queue never fills.
 *  - Commands: Scribe starts loading Needle the moment the mic opens (it is ready by the time you
 *    stop talking), listening ends by itself after a short silence, Whisper writes it down and
 *    Needle turns it into calls. Then three bands, as Cactus recommends for Needle's confidence:
 *    act (>= 0.3), ask on the Island first (0.1 .. 0.3, or an app name the request never said),
 *    refuse (below, a negated request, a number or text the request did not contain, or []). */
#include "amdroid/dictation.h"
#include "amdroid/hda.h"
#include "amdroid/scribe.h"
#include "amdroid/heap.h"
#include "amdroid/keyboard.h"
#include "amdroid/timer.h"
#include "amdroid/ui.h"
#include "amdroid/voicecmd.h"
#include "amdroid/serial.h"
#include <stdint.h>

#define DMAX (48000u * SCRIBE_MAX_SECONDS)
#define ACT_AT 0.30f
/* voice activity, in frames of 48 kHz: speech is a peak over SPEECH, the end is QUIET for END_SIL */
#define SPEECH   1500u
#define QUIET     700u
#define END_SIL  (48000u * 8 / 10)
#define NO_SPEECH (48000u * 6)
#define CMD_MAX  (48000u * 10)

static int state = DICT_IDLE, command_mode;
static int16_t *buf;
static u32 frames, peak, level, speech_at, loud_at, heard_speech;
static char out[4096];
static u32 out_len, out_pos;       /* typing what came back */
/* a command waiting for a yes */
static char pend_calls[1024], pend_heard[160], pend_what[160];
static u64 pend_until;

static void notify(const char *title, const char *body){
    int was = ui_prefs.dnd; ui_prefs.dnd = 0;       /* an answer to what you just asked: always shown */
    ui_notify(-1, title, body);
    ui_prefs.dnd = was;
}
static void fail(const char *msg){ notify(command_mode ? "Voice command" : "Dictation", msg); }
static void quoted(char *dst, u32 cap, const char *s){
    ui_copy(dst, "\"", cap);
    u32 l = ui_len(dst);
    for(; *s && l + 2 < cap; s++) dst[l++] = *s;
    dst[l++] = '"'; dst[l] = 0;
}

static void start(int cmd){
    command_mode = cmd;
    if(!hda_mic_present()){ fail("No microphone on this computer"); return; }
    buf = kmalloc((u64)DMAX * 4);
    if(!buf){ fail("Out of memory"); return; }
    if(hda_capture_start() != 0){ kfree(buf); buf = 0; fail("The microphone did not start"); return; }
    frames = peak = level = speech_at = loud_at = heard_speech = 0;
    out_len = out_pos = 0;
    state = DICT_LISTEN;
    if(cmd){ char why[96]; scribe_prepare_commands(why, sizeof why); }   /* Needle loads while you talk */
}

static void stop(void){
    hda_capture_stop();
    u32 m = frames / 3;
    float *mono = (float *)buf;            /* in place: each output lies behind its input */
    for(u32 i = 0; i < m; i++){
        int acc = 0;
        for(int k = 0; k < 3; k++) acc += buf[(i*3+k)*2] + buf[(i*3+k)*2+1];
        mono[i] = (float)acc / (6.0f * 32768.0f);
    }
    state = DICT_IDLE;
    if(peak < 300){ kfree(buf); buf = 0; fail("Heard only silence - is the microphone on?"); return; }
    char why[160];
    int r = command_mode ? scribe_submit_command(mono, m, why, sizeof why) : scribe_submit(mono, m, why, sizeof why);
    kfree(buf); buf = 0;
    if(r != 0){ fail(why); return; }
    state = DICT_WRITE;
}

void dict_toggle(void){
    if(state == DICT_IDLE) start(0);
    else if(state == DICT_LISTEN) stop();
}
void dict_command(void){
    if(state == DICT_IDLE) start(1);
    else if(state == DICT_LISTEN) stop();
    else if(state == DICT_CONFIRM){ state = DICT_IDLE; start(1); }
}
void dict_cancel(void){
    if(state == DICT_LISTEN){ hda_capture_stop(); kfree(buf); buf = 0; state = DICT_IDLE; }
    else if(state == DICT_CONFIRM) state = DICT_IDLE;
}
int dict_is_command(void){ return command_mode; }

void dict_confirm(int yes, int *next_vt){
    if(state != DICT_CONFIRM) return;
    state = DICT_IDLE;
    if(!yes){ notify(pend_heard, "OK, did nothing"); return; }
    char done[200];
    vc_describe(pend_calls, done, sizeof done, 1);
    if(vc_execute(pend_calls, next_vt)) notify(pend_heard, done);
}

/* what came back for a command: act, ask or refuse */
static void slog(const char *a, const char *b){ for(; a && *a; a++) serial_putc(*a); for(; b && *b; b++) serial_putc(*b); serial_putc('\n'); }

static void command_result(int *next_vt){
    scribe_cmd_t r;
    scribe_command_result(&r);
    {
        char n[16];
        slog("voice: heard: ", r.heard); slog("voice: calls: ", r.calls); slog("voice: reasoning: ", r.reasoning);
        char l[96] = "voice: confidence% "; ui_num((u32)(r.confidence * 100), n); ui_cat(l, n);
        ui_cat(l, " withheld "); ui_num((u32)(r.withheld < 0 ? 99 : r.withheld), n); ui_cat(l, n);
        ui_cat(l, " stt ms "); ui_num(r.stt_ms, n); ui_cat(l, n);
        ui_cat(l, " needle ms "); ui_num(r.needle_ms, n); ui_cat(l, n);
        slog(l, 0);
    }
    if(r.withheld < 0){ fail("Voice commands are not available (the model did not load)"); return; }
    if(!r.heard[0]){ fail("Didn't catch that"); return; }
    char title[160];
    quoted(title, sizeof title, r.heard);
    char what[200];
    int valid = r.n_calls > 0 && vc_describe(r.calls, what, sizeof what, 0);
    if(!valid || r.withheld == 1 || r.withheld == 3 || r.withheld == 4){
        notify(title, r.n_calls == 0 && !r.withheld ? "That's not something I can do here" : "Not sure what you meant - try again?");
        return;
    }
    if(r.withheld == 2){ notify(title, "You said not to - doing nothing"); return; }
    if(r.withheld == 5 || r.confidence < ACT_AT){
        ui_copy(pend_calls, r.calls, sizeof pend_calls);
        ui_copy(pend_heard, title, sizeof pend_heard);
        ui_copy(pend_what, what, sizeof pend_what);
        pend_until = timer_ticks() + 1000;
        state = DICT_CONFIRM;
        return;
    }
    char done[200];
    vc_describe(r.calls, done, sizeof done, 1);
    if(vc_execute(r.calls, next_vt)) notify(title, done);
}

void dict_tick(int *next_vt){
    /* 15 s after the desktop comes up, Scribe loads Needle in the background (preempted user
     * code: the desktop does not wait), so even the first command answers at once */
    static int warmed;
    if(!warmed && timer_ticks() > 1500){ warmed = 1; if(hda_mic_present()){ char why[96]; scribe_prepare_commands(why, sizeof why); } }
    static int shown = DICT_IDLE;
    if(state != shown){ shown = state; ui_redraw(); }      /* buttons that show the state */
    if(state == DICT_LISTEN){
        u32 got = hda_capture_read(buf + frames * 2, DMAX - frames);
        if(got){
            u32 lv = 0;
            for(u32 i = 0; i < got * 2; i++){ int v = buf[frames * 2 + i]; if(v < 0) v = -v; if((u32)v > lv) lv = (u32)v; }
            frames += got; level = lv;
            if(lv > peak) peak = lv;
            if(lv >= SPEECH){ if(!heard_speech){ heard_speech = 1; speech_at = frames; } loud_at = frames; }
            else if(lv >= QUIET && heard_speech) loud_at = frames;
        }
        if(command_mode){
            if(heard_speech && frames - loud_at >= END_SIL) stop();          /* you stopped talking */
            else if(!heard_speech && frames >= NO_SPEECH){ dict_cancel(); fail("Didn't hear a command"); }
            else if(frames >= CMD_MAX) stop();
        } else if(frames >= DMAX) stop();
    } else if(state == DICT_WRITE){
        int r = scribe_poll(out, sizeof out);
        if(r == -2) return;
        state = DICT_IDLE;
        if(command_mode){
            if(r < 0){ fail(out); return; }
            command_result(next_vt);
            return;
        }
        if(r < 0){ fail(out); return; }
        if(r == 0){ fail("No words heard"); return; }
        out_len = (u32)r; out_pos = 0;
        keyboard_inject(' ');                              /* apart from what was typed before */
    } else if(state == DICT_CONFIRM){
        if(timer_ticks() > pend_until){ state = DICT_IDLE; }
    }
    /* type the answer: printable ASCII; curly quotes and dashes become plain ones */
    for(int k = 0; k < 24 && out_pos < out_len; k++){
        u8 c = (u8)out[out_pos];
        if(c < 0x80){ out_pos++; if(c >= 32 && c <= 126) keyboard_inject(c); continue; }
        u32 n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        if(n == 3 && out_pos + 2 < out_len && c == 0xE2 && (u8)out[out_pos + 1] == 0x80){
            u8 d = (u8)out[out_pos + 2];
            if(d == 0x98 || d == 0x99) keyboard_inject('\'');
            else if(d == 0x9C || d == 0x9D) keyboard_inject('"');
            else if(d == 0x93 || d == 0x94) keyboard_inject('-');
            else if(d == 0xA6){ keyboard_inject('.'); keyboard_inject('.'); keyboard_inject('.'); }
        }
        out_pos += n;
    }
}

void dict_face(char *title, u32 tcap, char *body, u32 bcap){
    title[0] = body[0] = 0;
    if(state == DICT_LISTEN){
        if(command_mode){
            ui_copy(title, heard_speech ? "Listening..." : "Say a command", tcap);
            ui_copy(body, "It stops when you do. Esc to cancel", bcap);
        } else {
            u32 s10 = dict_tenths() / 10; char n[12];
            ui_copy(title, "Listening  ", tcap); ui_num(s10 / 60, n); ui_cat(title, n); ui_cat(title, ":");
            if(s10 % 60 < 10) ui_cat(title, "0");
            ui_num(s10 % 60, n); ui_cat(title, n);
            ui_copy(body, "Ctrl+Space to finish, Esc to cancel", bcap);
        }
    } else if(state == DICT_WRITE){
        ui_copy(title, command_mode ? "Thinking..." : "Writing it down...", tcap);
        ui_copy(body, command_mode ? "Working out what to do" : "The text goes to the window in front", bcap);
    } else if(state == DICT_CONFIRM){
        ui_copy(title, pend_what, tcap); ui_cat(title, "?");
        ui_copy(body, pend_heard, bcap);
        u32 l = ui_len(body);
        const char *k = "  Enter = yes, Esc = no";
        for(; *k && l + 1 < bcap; k++) body[l++] = *k;
        body[l] = 0;
    }
}

int dict_state(void){ return state; }
u32 dict_tenths(void){ return frames / 4800; }
u32 dict_level(void){ return level; }
