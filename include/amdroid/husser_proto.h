#ifndef AMDROID_HUSSER_PROTO_H
/* The wire format between the browser (kernel side) and the Husser renderer,
 * a ring-3 process. The whole shared region is mapped into Husser at
 * HUSSER_SHM_VA; the kernel reaches it through the page array it allocated.
 * Field-for-field compatible on both sides; never change without bumping
 * HUSSER_VER. */
#define AMDROID_HUSSER_PROTO_H
#include "amdroid/types.h"

#define HUSSER_SHM_VA       0x60000000ULL
#define HUSSER_HEAP_VA      0x48000000ULL       /* Husser's heap; the kernel sizes it by RAM (below the signal trampoline) */
#define HUSSER_HEAP_NORMAL  (208u * 1024 * 1024)
#define HUSSER_HEAP_ULTRA   (352u * 1024 * 1024)  /* ultra mode: more than 900 MB of RAM */
#define HUSSER_ULTRA_RAM_MB 900
#define HUSSER_MAX_W        1920            /* a maximized window on a full-HD screen */
#define HUSSER_MAX_H        1080
#define HUSSER_VRAM_OFF     16384ULL            /* the header fits below this (it is bigger than one page) */
#define HUSSER_VRAM_BYTES   ((u64)HUSSER_MAX_W * HUSSER_MAX_H * 4)
#define HUSSER_PAGE_OFF     (HUSSER_VRAM_OFF + HUSSER_VRAM_BYTES)
#define HUSSER_PAGE_BYTES   (4u * 1024 * 1024)          /* the page being shown */
#define HUSSER_RES_OFF      (HUSSER_PAGE_OFF + HUSSER_PAGE_BYTES)
#define HUSSER_RES_BYTES    (16u * 1024 * 1024)         /* one fetched subresource at a time (YouTube's app is ~11 MB) */
/* script fetches (fetch()): the addresses can be long (a media URL is ~1 KB), so they live
 * here rather than in need[] */
#define HUSSER_FETCHQ_OFF   (HUSSER_RES_OFF + HUSSER_RES_BYTES)
#define HUSSER_FETCH_MAX    8
#define HUSSER_FETCH_URL    4092
#define HUSSER_FETCH_ENT    16384u          /* each: u32 id, URL\0, method\0, content type\0, u32 body length, body */
#define HUSSER_FETCH_BODY   (HUSSER_FETCH_ENT - HUSSER_FETCH_URL - 4u - 128u - 4u)
#define HUSSER_FETCHQ_BYTES (HUSSER_FETCH_MAX * HUSSER_FETCH_ENT)
/* cookies: the page's (browser -> husser, written before H_CMD_LOAD, 0-terminated "a=b; c=d"),
 * then what document.cookie was set to (husser -> browser, one assignment per line, cookie_out_seq) */
#define HUSSER_COOKIE_OFF   (HUSSER_FETCHQ_OFF + HUSSER_FETCHQ_BYTES)
#define HUSSER_COOKIE_IN    4096u
#define HUSSER_COOKIE_BYTES 8192u
/* media bytes a page appends to a SourceBuffer, on their way to Reel */
#define HUSSER_MEDIA_OFF    (HUSSER_COOKIE_OFF + HUSSER_COOKIE_BYTES)
#define HUSSER_MEDIA_BYTES  (4u * 1024 * 1024)
#define HUSSER_SHM_BYTES    (HUSSER_MEDIA_OFF + HUSSER_MEDIA_BYTES)

#define HUSSER_MAGIC        0x52555348u                 /* 'HUSR' */
#define HUSSER_VER          4u

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
                                    cmd_i = kind (1 css, 2 image, 3 script), cmd_j = idx,
                                    res_len = bytes */
#define H_CMD_LINK_URL      9    /* cmd_i = link index: husser resolves web_link(idx) into link_url */
/* H_CMD_RES_DONE with res_kind 4: the answer to script fetch res_idx (res_len bytes at
 * HUSSER_RES_OFF, or 0xFFFFFFFF when it failed); res_status is the HTTP status */

/* media commands (husser -> browser), one at a time: media_seq bumped, the browser sets
 * media_ack to it when done */
#define M_CMD_OPEN      1    /* a MediaSource was attached to a <video>: a new presentation */
#define M_CMD_APPEND    2    /* media_a = source buffer, media_len bytes at HUSSER_MEDIA_OFF */
#define M_CMD_REMOVE    3    /* media_a, media_t0..media_t1 (us) */
#define M_CMD_ABORT     4    /* media_a */
#define M_CMD_EOS       5
#define M_CMD_PLAY      6
#define M_CMD_PAUSE     7
#define M_CMD_SEEK      8    /* media_t0 */
#define M_CMD_CLOSE     9    /* the element went away */
#define M_CMD_VOLUME    10   /* media_a = percent */

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
    u32 vw, vh;                 /* viewport size in pixels (<= HUSSER_MAX_*): the kernel sets the
                                   first, H_CMD_RESIZE the later ones */
    u32 heap_bytes;             /* the heap mapped at HUSSER_HEAP_VA */
    u32 ultra;                  /* 1: ultra-fast mode (lots of RAM): big script heap, resource cache */
    u32 ram_mb;                 /* the machine's memory */
    u32 adblock;                /* 1: hide ad boxes (the browser blocks the ad servers themselves) */

    /* husser -> browser */
    u32 state;                  /* H_STATE_* */
    u32 input_focus;            /* 1 while an <input> has focus (typing goes there) */
    u32 frame_seq;              /* bumped each finished frame */
    u32 frame_w, frame_h;       /* the size of the frame in the viewport area; 0 while a frame
                                   of another size is being copied in */
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

    /* command channel (browser -> husser) */
    u32 cmd;                    /* H_CMD_* */
    u32 cmd_seq;                /* bumped by the kernel to submit */
    u32 ack_seq;                /* husser sets it to the last seq it finished */
    i32 cmd_i, cmd_j;

    /* script fetches (husser -> browser): fetch_count requests at HUSSER_FETCHQ_OFF */
    u32 fetch_seq;
    u32 fetch_count;
    u32 res_status;             /* kind 4 answers: the HTTP status */
    u32 cookie_out_seq;         /* bumped when document.cookie writes wait at HUSSER_COOKIE_OFF + HUSSER_COOKIE_IN */

    /* media (husser -> browser) */
    u32 media_seq, media_ack;
    u32 media_cmd;
    i32 media_a;
    u32 media_len;
    i64 media_t0, media_t1;
    /* the <video> box in the viewport (from the last frame); video_w 0 when there is none */
    i32 video_x, video_y, video_w, video_h;
    u32 video_seq;
    /* media state (browser -> husser), refreshed every tick while a presentation is open */
    u32 ms_seq;
    i64 ms_time_us, ms_duration_us;
    u32 ms_playing, ms_ended, ms_stalled, ms_ready;
    u32 ms_video_w, ms_video_h;
    u32 ms_pending[2];          /* appends to each source buffer not finished yet */
    u32 ms_nranges[2];
    i64 ms_ranges[2][16];       /* buffered [start, end) pairs, us */
    u32 ms_error_seq;
    char ms_error[120];

} husser_shm_t;

#endif
