#ifndef AMDROID_HUSSER_PROTO_H
/* The wire format between the browser (kernel side) and the Husser renderer,
 * a ring-3 process. The whole shared region is mapped into Husser at
 * HUSSER_SHM_VA; the kernel reaches it through the page array it allocated.
 * Field-for-field compatible on both sides; never change without bumping
 * HUSSER_VER. */
#define AMDROID_HUSSER_PROTO_H
#include "amdroid/types.h"

#define HUSSER_SHM_VA       0x60000000ULL
#define HUSSER_MAX_W        1600
#define HUSSER_MAX_H        768
#define HUSSER_VRAM_OFF     16384ULL            /* the header fits below this (it is bigger than one page) */
#define HUSSER_VRAM_BYTES   ((u64)HUSSER_MAX_W * HUSSER_MAX_H * 4)
#define HUSSER_PAGE_OFF     (HUSSER_VRAM_OFF + HUSSER_VRAM_BYTES)
#define HUSSER_PAGE_BYTES   (4u * 1024 * 1024)          /* the page being shown */
#define HUSSER_RES_OFF      (HUSSER_PAGE_OFF + HUSSER_PAGE_BYTES)
#define HUSSER_RES_BYTES    (8u * 1024 * 1024)          /* one fetched subresource at a time */
#define HUSSER_SHM_BYTES    (HUSSER_RES_OFF + HUSSER_RES_BYTES)

#define HUSSER_MAGIC        0x52555348u                 /* 'HUSR' */
#define HUSSER_VER          1u

/* cmd (browser -> husser) */
#define H_CMD_NONE          0
#define H_CMD_LOAD          1    /* parse + lay out the page at HUSSER_PAGE_OFF (cmd_i = len) */
#define H_CMD_PAINT         2    /* repaint at the current viewport (cmd_i = scroll) */
#define H_CMD_KEY           3    /* cmd_i = the character (ENTER/DEL/arrows as codes below) */
#define H_CMD_CLICK         4    /* cmd_i = x, cmd_j = y (viewport coords) */
#define H_CMD_MOVE          5    /* cmd_i = x, cmd_j = y (hover) */
#define H_CMD_RESIZE        6    /* cmd_i = width, cmd_j = height: lay out for the new size */
#define H_CMD_SHUTDOWN      7    /* husser exits cleanly */
#define H_CMD_RES_DONE      8    /* a fetched subresource is at HUSSER_RES_OFF:
                                    cmd_i = kind (1 css, 2 image), cmd_j = idx (image index; css: -1),
                                    res_len = bytes */
#define H_CMD_LINK_URL      9    /* cmd_i = link index: husser resolves web_link(idx) into link_url */

/* keys above 255 use these */
#define H_KEY_UP            0x100
#define H_KEY_DOWN          0x101
#define H_KEY_PGUP          0x102
#define H_KEY_PGDN          0x103
#define H_KEY_TAB           0x104

/* state (husser -> browser) */
#define H_STATE_BOOT        0
#define H_STATE_READY       1    /* a full frame is in the viewport */
#define H_STATE_FAILED      2    /* the engine could not load the page */

#define HUSSER_TITLE_MAX    160
#define HUSSER_MSG_MAX      120
#define HUSSER_URL_MAX      512
#define HUSSER_NEED_MAX     16
#define HUSSER_URL_NEED     640

typedef struct husser_shm {
    /* set by the kernel before husser starts; husser only checks */
    u32 magic;
    u32 ver;
    u32 vw, vh;                 /* viewport size in pixels (<= HUSSER_MAX_*) */
    u32 pitch;                  /* pixels per row * 4 (vw * 4) */

    /* husser -> browser */
    u32 state;                  /* H_STATE_* */
    u32 input_focus;            /* 1 while an <input> has focus (typing goes there) */
    u32 frame_seq;              /* bumped each finished frame */
    u32 page_len;               /* bytes at HUSSER_PAGE_OFF to parse on H_CMD_LOAD */
    u32 page_h;                 /* the laid-out height (for the scrollbar) */
    char page_url[HUSSER_URL_MAX];  /* window.location, set before H_CMD_LOAD */
    u32 link_count;             /* links in the page (1-based for the footer) */
    u32 title_seq;              /* bumped when title changes */
    u32 console_seq;            /* bumped for each console.log line */
    u32 js_error_seq;           /* bumped when an uncaught script error appears */
    char title[HUSSER_TITLE_MAX];
    char console[HUSSER_MSG_MAX];
    char js_error[HUSSER_MSG_MAX];
    char link_url[HUSSER_URL_MAX];   /* answer to H_CMD_LINK_URL */
    u32 link_url_seq;
    char nav_url[HUSSER_URL_MAX];    /* a link was clicked: the href to navigate to */
    u32 nav_seq;                     /* bumped per navigation request */

    /* resource requests (husser needs subresources fetched) */
    u32 need_seq;               /* bumped when the list below changes */
    u32 need_count;             /* entries in need[] (0 when satisfied) */
    struct husser_need { u32 kind; u32 idx; char url[HUSSER_URL_NEED]; } need[HUSSER_NEED_MAX];

    /* resource results (browser -> husser, via H_CMD_RES_DONE) */
    u32 res_kind;               /* 1 css, 2 image */
    u32 res_idx;                /* image index, or 0xffffffff for a stylesheet */
    u32 res_len;                /* bytes at HUSSER_RES_OFF */
    u32 res_done_seq;           /* bumped by the kernel per delivered resource */

    /* command channel (browser -> husser) */
    u32 cmd;                    /* H_CMD_* */
    u32 cmd_seq;                /* bumped by the kernel to submit */
    u32 ack_seq;                /* husser sets it to the last seq it finished */
    i32 cmd_i, cmd_j;

    u32 scroll;                 /* current scroll (kept by husser; PAINT carries it) */
} husser_shm_t;

#endif
