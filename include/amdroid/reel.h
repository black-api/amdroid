#ifndef AMDROID_REEL_H
#define AMDROID_REEL_H
/* The kernel side of Reel, the media process (reel_proto.h says how the work is split).
 *
 * One presentation plays at a time; whoever called reel_begin last owns it (the browser's
 * <video>, or MVP). A client appends coded bytes to source buffers the way a page does with
 * MSE (fragmented MP4 or WebM, an init segment first), calls reel_play, and draws
 * reel_blit wherever the picture goes. reel_tick, from the owner's tick, keeps the sound
 * card fed and picks the picture that is due. */
#include "amdroid/types.h"
#include "amdroid/reel_proto.h"

/* Starts a new presentation for `owner` (any unique pointer): spawns Reel if needed and
 * drops what the last presentation had. 0, or -1 when the process cannot start. */
int  reel_begin(const void *owner);
/* Ends the presentation if `owner` has it (stops the sound). Reel keeps running. */
void reel_end(const void *owner);
int  reel_owned_by(const void *owner);
/* Ends the process too (frees its memory). */
void reel_shutdown(void);
int  reel_alive(void);
const volatile reel_shm_t *reel_header(void);

/* Queues bytes for source buffer sb (copied; returns at once). -1: out of memory. */
int  reel_append(int sb, const void *data, u32 len);
/* Appends to sb still queued or being parsed (MSE's `updating`). */
int  reel_append_pending(int sb);
u64  reel_queued_bytes(void);
void reel_remove(int sb, i64 t0_us, i64 t1_us);
void reel_abort(int sb);
void reel_eos(void);

/* the size pictures are converted to (the box they are shown in); smooth = bilinear */
void reel_set_size(u32 w, u32 h, int smooth);
void reel_play(void);
void reel_pause(void);
int  reel_playing(void);
void reel_seek(i64 us);
i64  reel_time(void);              /* the playback position, microseconds */
i64  reel_duration(void);          /* -1 when unknown */
int  reel_ended(void);             /* EOS was given and everything has been played */
int  reel_stalled(void);           /* playing, but waiting for data */
void reel_set_volume(int percent); /* 0..200 */

/* Every tick from the owner: commands, sound, the due picture. 1 when the picture changed. */
int  reel_tick(void);
/* The picture on screen now: 1 and its size, or 0 when there is none yet. */
int  reel_picture(u32 *w, u32 *h);
/* Draws it at (x, y), cut to w x h (fb clipping applies too). */
void reel_blit(i32 x, i32 y, u32 w, u32 h);
/* The last error Reel reported ("" if none) and a counter that changes with each. */
const char *reel_error(u32 *seq);

#endif
