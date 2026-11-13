/* virtio_net.c — virtio network device, legacy (0.9.5) interface over PCI I/O
 * space, as QEMU provides it with "-device virtio-net-pci". Polled. */
#include "amdroid/nic.h"
#include "amdroid/pci.h"
#include "amdroid/pmm.h"
#include "amdroid/console.h"
#include "amdroid/timer.h"
#include "amdroid/types.h"
#include <string.h>

/* legacy register offsets from the I/O BAR */
#define VIO_DEVICE_FEATURES 0x00
#define VIO_GUEST_FEATURES  0x04
#define VIO_QUEUE_PFN       0x08
#define VIO_QUEUE_SIZE      0x0C
#define VIO_QUEUE_SEL       0x0E
#define VIO_QUEUE_NOTIFY    0x10
#define VIO_STATUS          0x12
#define VIO_MAC             0x14

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define F_WRITE 2

#define NHDR 10                 /* struct virtio_net_hdr (legacy) */
#define QSIZE 256               /* largest queue we accept */
#define BUFSZ 2048
#define QMEM (16 * 1024)        /* room for one queue's rings (QSIZE 256 needs 12 KB) */

typedef struct { u64 addr; u32 len; u16 flags; u16 next; } __attribute__((packed)) vdesc_t;

typedef struct {
    vdesc_t *desc;
    u16 *avail;                 /* flags, idx, ring[QSIZE] */
    u8 *used;                   /* flags, idx, ring[QSIZE] of {id, len} */
    u16 size;
    u16 last_used;
    u16 free_head;
} vq_t;

static u16 iobase;
static vq_t rxq, txq;
static u8 *rxbuf[QSIZE], *txbuf;
/* ring memory must be physically contiguous: static, page aligned storage */
static u8 ringmem[2][QMEM] __attribute__((aligned(4096)));
static nic_t nic_vnet;

static inline void outb8(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw16(u16 p, u16 v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outl32(u16 p, u32 v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u8 inb8(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u16 inw16(u16 p){ u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u32 inl32(u16 p){ u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

/* sets up one virtqueue: descriptors, available ring and used ring in one page-aligned block */
static int vq_setup(vq_t *q, u16 index, u8 *mem){
    outw16(iobase + VIO_QUEUE_SEL, index);
    u16 size = inw16(iobase + VIO_QUEUE_SIZE);
    if(size == 0 || size > 256) return -1;
    q->size = size;
    /* legacy layout: desc table, then avail ring, then used ring page-aligned */
    u32 avail_end = (u32)size * 16 + 6 + (u32)size * 2;
    u32 used_off = (avail_end + 4095) & ~4095u;
    u32 total = used_off + 6 + (u32)size * 8;
    if(total > QMEM) return -1;
    memset(mem, 0, QMEM);
    q->desc = (vdesc_t*)mem;
    q->avail = (u16*)(mem + (u32)size * 16);
    q->used = mem + used_off;
    q->last_used = 0;
    outl32(iobase + VIO_QUEUE_PFN, (u32)((unsigned long)mem >> 12));
    return 0;
}

static void notify(u16 index){ outw16(iobase + VIO_QUEUE_NOTIFY, index); }

static int vnet_send(const u8 *frame, u32 len){
    if(len > BUFSZ - NHDR) return -1;
    /* one descriptor: header + frame, device reads it */
    memset(txbuf, 0, NHDR);
    memcpy(txbuf + NHDR, frame, len);
    txq.desc[0].addr = (u64)(unsigned long)txbuf;
    txq.desc[0].len = len + NHDR;
    txq.desc[0].flags = 0;
    txq.desc[0].next = 0;
    u16 idx = txq.avail[1];
    txq.avail[2 + (idx % txq.size)] = 0;
    __asm__ volatile("" ::: "memory");
    txq.avail[1] = idx + 1;
    notify(1);
    /* wait until the device has used it */
    u16 *used_idx = (u16*)(txq.used + 2);
    for(int t = 0; t < 1000000 && *used_idx == txq.last_used; t++) ;
    txq.last_used = *used_idx;
    return 0;
}

static int vnet_recv(u8 *frame, u32 max, u32 *len){
    u16 *used_idx = (u16*)(rxq.used + 2);
    if(*used_idx == rxq.last_used) return 0;
    u16 slot = rxq.last_used % rxq.size;
    u32 *ring = (u32*)(rxq.used + 4);
    u32 id = ring[slot * 2];
    u32 n = ring[slot * 2 + 1];
    if(n <= NHDR) { rxq.last_used++; return 0; }
    n -= NHDR;
    if(n > max) n = max;
    memcpy(frame, rxbuf[id] + NHDR, n);
    *len = n;
    /* give the buffer back */
    u16 aidx = rxq.avail[1];
    rxq.avail[2 + (aidx % rxq.size)] = (u16)id;
    __asm__ volatile("" ::: "memory");
    rxq.avail[1] = aidx + 1;
    notify(0);
    rxq.last_used++;
    return 1;
}

int virtio_net_init(void){
    pci_dev_t *d = pci_find(0x1AF4, 0x1000);       /* transitional virtio-net */
    if(!d) return -1;
    pci_enable(d);
    /* legacy virtio uses an I/O port BAR: the base is the BAR with its low bits cleared */
    iobase = (u16)(d->bar[0] & ~3u);
    if(!iobase || !(d->bar[0] & 1)) return -1;
    outb8(iobase + VIO_STATUS, 0);                  /* reset */
    outb8(iobase + VIO_STATUS, ST_ACK);
    outb8(iobase + VIO_STATUS, ST_ACK | ST_DRIVER);
    (void)inl32(iobase + VIO_DEVICE_FEATURES);
    outl32(iobase + VIO_GUEST_FEATURES, 0);         /* no optional features */

    if(vq_setup(&rxq, 0, ringmem[0]) != 0 || vq_setup(&txq, 1, ringmem[1]) != 0){ console_puts("[virtio-net] queues failed\n"); return -1; }
    /* RX: every buffer is posted to the device, write-only for it */
    for(u16 i = 0; i < rxq.size; i++){
        rxbuf[i] = pmm_alloc_page();
        if(!rxbuf[i]) return -1;
        rxq.desc[i].addr = (u64)(unsigned long)rxbuf[i];
        rxq.desc[i].len = BUFSZ;
        rxq.desc[i].flags = F_WRITE;
        rxq.desc[i].next = 0;
        rxq.avail[2 + i] = i;
    }
    rxq.avail[1] = rxq.size;
    txbuf = pmm_alloc_page();
    if(!txbuf) return -1;
    notify(0);
    outb8(iobase + VIO_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);

    for(int i = 0; i < 6; i++) nic_vnet.mac[i] = inb8(iobase + VIO_MAC + i);
    nic_vnet.name = "virtio-net";
    nic_vnet.send = vnet_send;
    nic_vnet.recv = vnet_recv;
    console_printf("[virtio-net] %02x:%02x:%02x:%02x:%02x:%02x\n", nic_vnet.mac[0], nic_vnet.mac[1],
                   nic_vnet.mac[2], nic_vnet.mac[3], nic_vnet.mac[4], nic_vnet.mac[5]);
    return 0;
}

nic_t *virtio_nic(void){ return &nic_vnet; }
