#ifndef AMDROID_HUSSER_H
#define AMDROID_HUSSER_H
/* The kernel side of the Husser renderer. The browser app owns the network
 * and the window; Husser (a ring-3 process) owns the page. All bytes and
 * frames cross through one shared region, mapped into Husser at
 * HUSSER_SHM_VA and reached here through the page array below. */
#include "amdroid/types.h"
#include "amdroid/husser_proto.h"

/* Allocates the shared region once (first call) and spawns the renderer
 * for a fresh page. Returns the pid, or -1. vw/vh clamp to HUSSER_MAX_*. */
int  husser_open(u32 vw, u32 vh);
/* Ends the renderer. */
void husser_close(void);
/* Sets window.location for the next load. */
void husser_set_url(const char *url);
/* The calls below queue a command and return at once; Husser answers through the shared
 * header (frame_seq, need_seq, nav_seq, link_url_seq...). husser_pump, called every tick,
 * hands the queue over. */
void husser_pump(void);
int  husser_busy(void);          /* 1 while commands are queued or being worked on */
/* Stages the page bytes and asks Husser to parse, lay out and paint it. */
int  husser_load(const void *html, u32 len, u32 scroll);
/* Repaints at the given scroll offset. */
void husser_paint(u32 scroll);
void husser_key(u32 code);
void husser_click(i32 x, i32 y);
void husser_move(i32 x, i32 y);
void husser_resize(u32 vw, u32 vh);
/* Delivers a fetched subresource (kind 1 css, 2 image; bytes NULL = failed). */
void husser_res(u32 kind, u32 idx, const void *bytes, u32 len);
/* Asks for the href at idx; the answer arrives via the link_url/link_url_seq pair. */
void husser_link_url(u32 idx);
/* 1 when the renderer process is still up. */
int  husser_alive(void);
/* The header of the shared region (kernel pointer). NULL before husser_open. */
const volatile husser_shm_t *husser_header(void);
/* Copies the finished frame into the back buffer at (x, y), cut to w x h. */
void husser_blit(u32 x, u32 y, u32 w, u32 h);

#endif
