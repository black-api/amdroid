#ifndef AMDROID_HUSSER_H
/* The kernel side of the Husser renderer. The browser app owns the network
 * and the window; Husser (a ring-3 process) owns the page. All bytes and
 * screenshots cross through one shared region, mapped into Husser at
 * HUSSER_SHM_VA and reached here through the page array below. */
#include "amdroid/types.h"
#include "amdroid/husser_proto.h"

/* Allocates the shared region once (first call) and spawns the renderer
 * for a fresh page. Returns the pid, or -1. vw/vh clamp to HUSSER_MAX_*. */
int  husser_open(u32 vw, u32 vh);
/* Asks the renderer to leave; the process is detached and reaped on exit. */
void husser_close(void);
/* Sets window.location for the next load. */
void husser_set_url(const char *url);
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
/* Copies the viewport into the back buffer at (x, y); outside the region is ignored. */
void husser_blit(u32 x, u32 y, u32 w, u32 h);
/* Kernel test helper: counts non-zero pixels in the viewport (0 on failure). */
u32  husser_vram_nonzero(void);

#endif
