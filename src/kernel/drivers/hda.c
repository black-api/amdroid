/* hda.c — Intel High Definition Audio driver (output only).
 *
 * Steps: reset the controller, discover the codec's widgets with the
 * immediate command interface, find a DAC wired to a line/speaker/headphone
 * pin, set up one output stream with a buffer descriptor list, and play PCM. */
#include "amdroid/hda.h"
#include "amdroid/pci.h"
#include "amdroid/vmm.h"
#include "amdroid/pmm.h"
#include "amdroid/kmath.h"
#include "amdroid/timer.h"
#include "amdroid/console.h"
#include "amdroid/types.h"
#include <string.h>
#include <stdint.h>

/* controller registers (offsets from BAR0) */
#define GCAP     0x00
#define GCTL     0x08
#define STATESTS 0x0E
#define ICOI     0x60
#define ICII     0x64
#define ICS      0x68
#define SD_BASE  0x80
#define SD_SIZE  0x20
/* stream descriptor fields */
#define SDCTL 0x00
#define SDSTS 0x03
#define SDCBL 0x08
#define SDLVI 0x0C
#define SDFMT 0x12
#define SDBDPL 0x18
#define SDBDPU 0x1C

#define BUF_BYTES 4096           /* one page per buffer */
#define RATE 48000
#define CHANNELS 2

static volatile u8 *mmio;
static int ready;
static int codec_cad;           /* codec address on the link (usually 0) */
static int dac_nid, pin_nid;
static int adc_nid, mic_nid;     /* capture: an input converter and an input pin */
static u32 *cbdl;
static u8 *cbuf[200];

static int stream_index;        /* first output stream descriptor */
static u32 *bdl;                /* buffer descriptor list (physical == virtual) */
static u32 *bufs[64];
static int nbufs;

static inline u32 rd32(u32 off){ return *(volatile u32*)(mmio + off); }
static inline u16 rd16(u32 off){ return *(volatile u16*)(mmio + off); }
static inline u8  rd8(u32 off){ return *(volatile u8*)(mmio + off); }
static inline void wr32(u32 off, u32 v){ *(volatile u32*)(mmio + off) = v; }
static inline void wr16(u32 off, u16 v){ *(volatile u16*)(mmio + off) = v; }
static inline void wr8(u32 off, u8 v){ *(volatile u8*)(mmio + off) = v; }

/* immediate command interface: send one verb, read back the response */
static int send(u32 cmd){
    int t;
    for(t = 0; t < 100000 && (rd16(ICS) & 1); t++) ;
    if(t == 100000) return -1;
    wr16(ICS, 0x2);                 /* clear IRV */
    wr32(ICOI, cmd);
    wr16(ICS, 0x1);                 /* ICB: start */
    for(t = 0; t < 100000; t++){
        u16 s = rd16(ICS);
        if((s & 0x2) && !(s & 0x1)) return (int)rd32(ICII);
    }
    return -1;
}

/* 12-bit verb with an 8-bit payload */
static u32 cmd12(int nid, u32 verb12, u32 payload){
    return ((u32)codec_cad << 28) | ((u32)nid << 20) | (verb12 << 8) | (payload & 0xFF);
}
/* 4-bit verb with a 16-bit payload (set format and amplifier gain) */
static u32 cmd4(int nid, u32 verb4, u32 payload){
    return ((u32)codec_cad << 28) | ((u32)nid << 20) | (verb4 << 16) | (payload & 0xFFFF);
}
static u32 get_param(int nid, u32 param){
    return (u32)send(cmd12(nid, 0xF00, param));
}

static int widget_type(int nid){ return (int)((get_param(nid, 0x09) >> 20) & 0xF); }

int hda_init(void){
    if(ready) return 0;
    pci_dev_t *d = pci_find_class(0x04, 0x03);       /* multimedia / audio */
    if(!d){ console_puts("[hda] no audio controller\n"); return -1; }
    pci_enable(d);
    u64 base = pci_bar_base(d, 0);
    if(!base){ console_puts("[hda] no memory BAR\n"); return -1; }
    vmm_map_region(base, base, 0x4000);
    mmio = (volatile u8*)(unsigned long)base;

    /* reset the controller, then wait for it to come out of reset */
    wr32(GCTL, 0);
    for(int t = 0; t < 1000000 && (rd32(GCTL) & 1); t++) ;
    wr32(GCTL, 1);
    for(int t = 0; t < 1000000 && !(rd32(GCTL) & 1); t++) ;
    timer_sleep(5);
    if(!(rd32(GCTL) & 1)){ console_puts("[hda] controller did not start\n"); return -1; }

    /* wait for a codec to announce itself */
    u16 st = 0;
    for(int t = 0; t < 1000000 && !st; t++) st = rd16(STATESTS);
    if(!st){ console_puts("[hda] no codec\n"); return -1; }
    codec_cad = 0;
    while(!((st >> codec_cad) & 1) && codec_cad < 15) codec_cad++;
    wr16(STATESTS, st);

    /* the audio function group is the root's sub-node; its widgets follow it */
    u32 root = get_param(0, 0x04);
    int fg_start = (int)((root >> 16) & 0xFF), fg_count = (int)(root & 0xFF);
    int afg = -1;
    for(int n = fg_start; n < fg_start + fg_count; n++)
        if((get_param(n, 0x05) & 0xFF) == 0x01){ afg = n; break; }
    if(afg < 0){ console_puts("[hda] no audio function group\n"); return -1; }
    send(cmd12(afg, 0x705, 0));       /* power up */
    u32 wn = get_param(afg, 0x04);
    int w_start = (int)((wn >> 16) & 0xFF), w_count = (int)(wn & 0xFF);

    /* a DAC that feeds an output pin (line out, speaker or headphones) */
    dac_nid = -1; pin_nid = -1;
    for(int n = w_start; n < w_start + w_count && pin_nid < 0; n++){
        if(widget_type(n) != 4) continue;                       /* pin complex */
        u32 cfg = (u32)send(cmd12(n, 0xF1C, 0));                /* config default */
        int dev = (int)((cfg >> 20) & 0xF);
        int connected = (int)((cfg >> 30) & 0x3) != 1;          /* not "no connection" */
        if(!connected || dev > 2) continue;
        if(!((get_param(n, 0x0C) >> 4) & 1)) continue;          /* output capable */
        /* walk the pin's connections to an audio output (DAC) */
        u32 cl = get_param(n, 0x0E);
        int ncons = (int)(cl & 0x7F);
        for(int c = 0; c < ncons && dac_nid < 0; c++){
            int nid = (int)(send(cmd12(n, 0xF02, (u32)c)) & 0xFF);
            if(widget_type(nid) == 0) dac_nid = nid;
        }
        if(dac_nid >= 0) pin_nid = n;
    }
    if(dac_nid < 0){ console_puts("[hda] no output path\n"); return -1; }

    /* the first output stream descriptor comes after the input streams */
    u16 gcap = rd16(GCAP);
    int iss = (gcap >> 8) & 0xF;
    stream_index = iss;

    /* power, format, amplifiers */
    send(cmd12(dac_nid, 0x705, 0));
    send(cmd12(pin_nid, 0x705, 0));
    send(cmd4(dac_nid, 0x2, 0x0011));                            /* 48 kHz, 16-bit, 2 channels */
    send(cmd12(pin_nid, 0x707, 0x40));                           /* pin output enable */
    send(cmd4(dac_nid, 0x3, 0xB000 | 0x3F));                     /* DAC gain, out, left+right */
    send(cmd4(pin_nid, 0x3, 0xB000 | 0x3F));

    /* capture path: an input converter (type 1) fed by an input pin (mic or line in) */
    adc_nid = -1; mic_nid = -1;
    for(int n = w_start; n < w_start + w_count && mic_nid < 0; n++){
        if(widget_type(n) != 4) continue;                       /* candidate input pin */
        u32 cfg2 = (u32)send(cmd12(n, 0xF1C, 0));
        int dev2 = (int)((cfg2 >> 20) & 0xF);
        if(((cfg2 >> 30) & 3) == 1 || !(dev2 == 0x0A || dev2 == 0x08 || dev2 == 0x0E)) continue;
        if(!((get_param(n, 0x0C) >> 5) & 1)) continue;          /* input capable */
        /* the converter that reads this pin lists it in its own connections */
        for(int c = w_start; c < w_start + w_count && adc_nid < 0; c++){
            if(widget_type(c) != 1) continue;
            u32 cl2 = get_param(c, 0x0E);
            int ncons2 = (int)(cl2 & 0x7F);
            for(int k = 0; k < ncons2; k++)
                if((int)(send(cmd12(c, 0xF02, (u32)k)) & 0xFF) == n){ adc_nid = c; break; }
        }
        if(adc_nid >= 0) mic_nid = n;
    }
    if(adc_nid >= 0 && ((rd16(GCAP) >> 8) & 0xF) > 0){
        send(cmd12(adc_nid, 0x705, 0));
        send(cmd12(mic_nid, 0x705, 0));
        send(cmd12(mic_nid, 0x707, 0x20));                       /* pin input enable */
        send(cmd4(mic_nid, 0x3, 0x4000 | 0x3F));                 /* pin amp: input side */
        send(cmd4(adc_nid, 0x3, 0x4000 | 0x3F));                 /* ADC amp: input side */
        cbdl = pmm_alloc_page();
        if(cbdl) memset(cbdl, 0, 4096);
    }

    /* buffers and the buffer descriptor list (one page-sized buffer each) */
    bdl = pmm_alloc_page();
    if(!bdl){ console_puts("[hda] out of memory\n"); return -1; }
    memset(bdl, 0, 4096);
    console_printf("[hda] codec %d, DAC %d, pin %d, stream %d\n", codec_cad, dac_nid, pin_nid, stream_index);
    ready = 1;
    return 0;
}

/* Records ms milliseconds of 16-bit stereo 48 kHz audio into out (ms*192 bytes).
 * Returns 0 on success; *frames gets the number of stereo frames. */
int hda_record(int ms, int16_t *out, u32 *frames){
    if(!ready && hda_init() != 0) return -1;
    if(adc_nid < 0 || !cbdl) return -1;
    u32 want = (u32)(RATE * ms / 1000) * 2;       /* int16 values, stereo */
    u32 nb = (want * 2 + BUF_BYTES - 1) / BUF_BYTES;
    if(nb == 0) nb = 1;
    if(nb > 200) nb = 200;
    for(u32 i = 0; i < nb; i++){
        if(!cbuf[i]) cbuf[i] = pmm_alloc_page();
        if(!cbuf[i]) return -1;
        memset(cbuf[i], 0, BUF_BYTES);
        cbdl[i*4+0] = (u32)(unsigned long)cbuf[i];
        cbdl[i*4+1] = 0;
        cbdl[i*4+2] = BUF_BYTES;
        cbdl[i*4+3] = 0;
    }
    u32 sd = SD_BASE;                             /* input stream 0 */
    wr8(sd + SDCTL, 0);
    for(int t = 0; t < 100000 && (rd8(sd + SDCTL) & 1); t++) ;
    wr8(sd + SDCTL, 1);
    for(int t = 0; t < 100000 && !(rd8(sd + SDCTL) & 1); t++) ;
    wr8(sd + SDCTL, 0);
    for(int t = 0; t < 100000 && (rd8(sd + SDCTL) & 1); t++) ;
    wr32(sd + SDBDPL, (u32)(unsigned long)cbdl);
    wr32(sd + SDBDPU, 0);
    wr32(sd + SDCBL, nb * BUF_BYTES);
    wr16(sd + SDLVI, (u16)(nb - 1));
    wr16(sd + SDFMT, 0x0011);
    send(cmd12(adc_nid, 0x706, (2 << 4) | 0));    /* stream tag 2 on the converter */
    wr8(sd + SDCTL + 2, (u8)(2 << 4));            /* stream tag 2 */
    wr8(sd + SDCTL, 0x2);                         /* run */
    /* wait until the DMA has covered the buffers (LPIB is the byte position) */
    u32 need = nb * BUF_BYTES;
    u64 until = timer_ticks() + ms / 10 + 50;
    while(timer_ticks() < until){
        u32 lpib = *(volatile u32*)(mmio + sd + 0x04);
        if(lpib >= need) break;
        timer_sleep(1);
    }
    wr8(sd + SDCTL, 0);
    u32 n = 0;
    for(u32 b = 0; b < nb && n < want; b++){
        u32 take = BUF_BYTES / 2;
        if(take > want - n) take = want - n;
        memcpy(out + n, cbuf[b], take * 2);
        n += take;
    }
    *frames = n / 2;
    return 0;
}

/* fills the buffers with a sine tone and starts the stream */
int hda_play_tone(int hz, int ms){
    if(!ready && hda_init() != 0) return -1;
    int frames = RATE * ms / 1000;
    int total_bytes = frames * CHANNELS * 2;
    int nb = (total_bytes + BUF_BYTES - 1) / BUF_BYTES;
    if(nb > 64) nb = 64;
    if(nb <= 0) return -1;
    for(int i = nbufs; i < nb; i++){
        bufs[i] = pmm_alloc_page();
        if(!bufs[i]) return -1;
    }
    if(nb > nbufs) nbufs = nb;
    /* sine samples: amplitude 8000 */
    int sample = 0;
    for(int b = 0; b < nb; b++){
        int16_t *p = (int16_t*)bufs[b];
        for(int i = 0; i < BUF_BYTES / 2; i += 2){
            double t = (double)sample / RATE;
            int16_t v = (int16_t)(8000.0 * ksin(2.0 * 3.14159265358979 * hz * t));
            p[i] = v; p[i+1] = v;
            sample++;
            if(sample >= frames) { for(; i < BUF_BYTES / 2; i++) p[i] = 0; break; }
        }
        bdl[b*4 + 0] = (u32)(unsigned long)bufs[b];
        bdl[b*4 + 1] = 0;
        bdl[b*4 + 2] = BUF_BYTES;
        bdl[b*4 + 3] = 0;
    }
    u32 sd = SD_BASE + (u32)stream_index * SD_SIZE;
    /* stop, reset the stream */
    wr8(sd + SDCTL, 0);
    wr8(sd + SDCTL, 1);
    for(int t = 0; t < 100000 && !(rd8(sd + SDCTL) & 1); t++) ;
    wr8(sd + SDCTL, 0);
    for(int t = 0; t < 100000 && (rd8(sd + SDCTL) & 1); t++) ;
    wr32(sd + SDBDPL, (u32)(unsigned long)bdl);
    wr32(sd + SDBDPU, 0);
    wr32(sd + SDCBL, (u32)(nb * BUF_BYTES));
    wr16(sd + SDLVI, (u16)(nb - 1));
    wr16(sd + SDFMT, 0x0011);
    /* stream 1 on channel 0 for the DAC; the stream number sits in SDCTL bits 20-23 */
    send(cmd12(dac_nid, 0x706, (1 << 4) | 0));
    wr8(sd + SDCTL + 2, (u8)(1 << 4));          /* stream tag 1 (bits 20-23) */
    wr8(sd + SDCTL, 0x2);                       /* run */
    return 0;
}
