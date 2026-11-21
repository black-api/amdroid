/* e1000.c — Intel gigabit Ethernet: the classic 8254x "e1000" (QEMU, VirtualBox, VMware),
 * the PCIe 8257x/82583 and ICH/PCH chipset NICs "e1000e" (82574L, 82579, I217, I218, I219
 * on most Intel desktops and laptops) and the "igb" I210/I211/I350/8257[56]/82580.
 * All three speak the same legacy descriptor format; the differences are small and are
 * handled by the family flag below. Polled, no IRQs. */
#include "amdroid/nic.h"
#include "amdroid/pci.h"
#include "amdroid/vmm.h"
#include "amdroid/pmm.h"
#include "amdroid/console.h"
#include "amdroid/timer.h"
#include "amdroid/types.h"
#include <string.h>

#define CTRL     0x0000
#define STATUS   0x0008
#define EECD     0x0010
#define EERD     0x0014
#define CTRL_EXT 0x0018
#define MDIC     0x0020
#define FEXTNVM3 0x003C
#define ICR      0x00C0
#define IMC      0x00D8
#define RCTL     0x0100
#define TCTL     0x0400
#define TIPG     0x0410
#define EXTCNF   0x0F00
#define SRRCTL0  0x0C00         /* igb: receive buffer size / descriptor type */
#define RDBAL    0x2800
#define RDBAH    0x2804
#define RDLEN    0x2808
#define RDH      0x2810
#define RDT      0x2818
#define RXDCTL   0x2828
#define TDBAL    0x3800
#define TDBAH    0x3804
#define TDLEN    0x3808
#define TDH      0x3810
#define TDT      0x3818
#define TXDCTL   0x3828
#define MTA      0x5200
#define RAL0     0x5400
#define RAH0     0x5404

#define CTRL_SLU   (1u << 6)
#define CTRL_ASDE  (1u << 5)
#define CTRL_LPC_OVR (1u << 16) /* LANPHYPC override / value: power-cycles the PCH PHY */
#define CTRL_LPC_VAL (1u << 17)
#define CTRL_RST   (1u << 26)
#define CTRL_PHYRST (1u << 31)
#define STATUS_LU  (1u << 1)
#define QUEUE_EN   (1u << 25)   /* RXDCTL/TXDCTL enable on igb */

#define NRX 256
#define NTX 64
#define BUFSZ 2048

enum { FAM_E1000, FAM_E1000E, FAM_PCH, FAM_IGB };

typedef struct { u16 id; u8 fam; const char *name; } e1k_id_t;

static const e1k_id_t ids[] = {
    /* 8254x (PCI/PCI-X); QEMU "e1000" is 100E, VMware 100F, VirtualBox 100E/1004/100F */
    {0x1000,FAM_E1000,"82542"}, {0x1001,FAM_E1000,"82543GC"}, {0x1004,FAM_E1000,"82543GC"},
    {0x1008,FAM_E1000,"82544EI"}, {0x1009,FAM_E1000,"82544EI"}, {0x100C,FAM_E1000,"82544GC"},
    {0x100D,FAM_E1000,"82544GC"}, {0x100E,FAM_E1000,"82540EM"}, {0x100F,FAM_E1000,"82545EM"},
    {0x1010,FAM_E1000,"82546EB"}, {0x1011,FAM_E1000,"82545EM"}, {0x1012,FAM_E1000,"82546EB"},
    {0x1013,FAM_E1000,"82541EI"}, {0x1015,FAM_E1000,"82540EM"}, {0x1016,FAM_E1000,"82540EP"},
    {0x1017,FAM_E1000,"82540EP"}, {0x1018,FAM_E1000,"82541EI"}, {0x1019,FAM_E1000,"82547EI"},
    {0x101A,FAM_E1000,"82547EI"}, {0x101D,FAM_E1000,"82546EB"}, {0x101E,FAM_E1000,"82540EP"},
    {0x1026,FAM_E1000,"82545GM"}, {0x1027,FAM_E1000,"82545GM"}, {0x1028,FAM_E1000,"82545GM"},
    {0x1075,FAM_E1000,"82547GI"}, {0x1076,FAM_E1000,"82541GI"}, {0x1077,FAM_E1000,"82541GI"},
    {0x1078,FAM_E1000,"82541ER"}, {0x1079,FAM_E1000,"82546GB"}, {0x107A,FAM_E1000,"82546GB"},
    {0x107B,FAM_E1000,"82546GB"}, {0x107C,FAM_E1000,"82541PI"}, {0x108A,FAM_E1000,"82546GB"},
    {0x1099,FAM_E1000,"82546GB"}, {0x10B5,FAM_E1000,"82546GB"},
    /* 8257x / 82583 (PCIe); QEMU "e1000e" is 10D3 */
    {0x105E,FAM_E1000E,"82571EB"}, {0x105F,FAM_E1000E,"82571EB"}, {0x1060,FAM_E1000E,"82571EB"},
    {0x107D,FAM_E1000E,"82572EI"}, {0x107E,FAM_E1000E,"82572EI"}, {0x107F,FAM_E1000E,"82572EI"},
    {0x108B,FAM_E1000E,"82573V"}, {0x108C,FAM_E1000E,"82573E"}, {0x109A,FAM_E1000E,"82573L"},
    {0x10A4,FAM_E1000E,"82571EB"}, {0x10A5,FAM_E1000E,"82571EB"}, {0x10B9,FAM_E1000E,"82572EI"},
    {0x10BC,FAM_E1000E,"82571EB"}, {0x10D3,FAM_E1000E,"82574L"}, {0x10D5,FAM_E1000E,"82571PT"},
    {0x10F6,FAM_E1000E,"82574L"}, {0x150C,FAM_E1000E,"82583V"},
    /* ICH8/9/10 and PCH chipset LANs: 82566/82567/82577/82578/82579, I217, I218, I219 */
    {0x1049,FAM_PCH,"82566MM"}, {0x104A,FAM_PCH,"82566DM"}, {0x104B,FAM_PCH,"82566DC"},
    {0x104C,FAM_PCH,"82562V"}, {0x104D,FAM_PCH,"82566MC"}, {0x10BD,FAM_PCH,"82566DM-2"},
    {0x10BF,FAM_PCH,"82567LF"}, {0x10CB,FAM_PCH,"82567V"}, {0x10CC,FAM_PCH,"82567LM-2"},
    {0x10CD,FAM_PCH,"82567LF-2"}, {0x10CE,FAM_PCH,"82567V-2"}, {0x10DE,FAM_PCH,"82567LM-3"},
    {0x10DF,FAM_PCH,"82567LF-3"}, {0x10E5,FAM_PCH,"82567LM-4"}, {0x10EA,FAM_PCH,"82577LM"},
    {0x10EB,FAM_PCH,"82577LC"}, {0x10EF,FAM_PCH,"82578DM"}, {0x10F0,FAM_PCH,"82578DC"},
    {0x10F5,FAM_PCH,"82567LM"}, {0x294C,FAM_PCH,"82566DC-2"}, {0x1501,FAM_PCH,"82567V-3"},
    {0x1502,FAM_PCH,"82579LM"}, {0x1503,FAM_PCH,"82579V"},
    {0x153A,FAM_PCH,"I217-LM"}, {0x153B,FAM_PCH,"I217-V"},
    {0x1559,FAM_PCH,"I218-V"}, {0x155A,FAM_PCH,"I218-LM"}, {0x15A0,FAM_PCH,"I218-LM"},
    {0x15A1,FAM_PCH,"I218-V"}, {0x15A2,FAM_PCH,"I218-LM"}, {0x15A3,FAM_PCH,"I218-V"},
    {0x156F,FAM_PCH,"I219-LM"}, {0x1570,FAM_PCH,"I219-V"}, {0x15B7,FAM_PCH,"I219-LM"},
    {0x15B8,FAM_PCH,"I219-V"}, {0x15B9,FAM_PCH,"I219-LM"}, {0x15BB,FAM_PCH,"I219-LM"},
    {0x15BC,FAM_PCH,"I219-V"}, {0x15BD,FAM_PCH,"I219-LM"}, {0x15BE,FAM_PCH,"I219-V"},
    {0x15D6,FAM_PCH,"I219-V"}, {0x15D7,FAM_PCH,"I219-LM"}, {0x15D8,FAM_PCH,"I219-V"},
    {0x15DF,FAM_PCH,"I219-LM"}, {0x15E0,FAM_PCH,"I219-V"}, {0x15E1,FAM_PCH,"I219-LM"},
    {0x15E2,FAM_PCH,"I219-V"}, {0x15E3,FAM_PCH,"I219-LM"}, {0x0D4C,FAM_PCH,"I219-LM"},
    {0x0D4D,FAM_PCH,"I219-V"}, {0x0D4E,FAM_PCH,"I219-LM"}, {0x0D4F,FAM_PCH,"I219-V"},
    {0x0D53,FAM_PCH,"I219-LM"}, {0x0D55,FAM_PCH,"I219-V"}, {0x15F9,FAM_PCH,"I219-LM"},
    {0x15FA,FAM_PCH,"I219-V"}, {0x15FB,FAM_PCH,"I219-LM"}, {0x15FC,FAM_PCH,"I219-V"},
    {0x1A1C,FAM_PCH,"I219-LM"}, {0x1A1D,FAM_PCH,"I219-V"}, {0x1A1E,FAM_PCH,"I219-LM"},
    {0x1A1F,FAM_PCH,"I219-V"}, {0x0DC5,FAM_PCH,"I219-LM"}, {0x0DC6,FAM_PCH,"I219-V"},
    {0x0DC7,FAM_PCH,"I219-LM"}, {0x0DC8,FAM_PCH,"I219-V"}, {0x550A,FAM_PCH,"I219-LM"},
    {0x550B,FAM_PCH,"I219-V"}, {0x550C,FAM_PCH,"I219-LM"}, {0x550D,FAM_PCH,"I219-V"},
    {0x550E,FAM_PCH,"I219-LM"}, {0x550F,FAM_PCH,"I219-V"}, {0x5510,FAM_PCH,"I219-LM"},
    {0x5511,FAM_PCH,"I219-V"}, {0x57A0,FAM_PCH,"I219-LM"}, {0x57A1,FAM_PCH,"I219-V"},
    {0x57B3,FAM_PCH,"I219-LM"}, {0x57B4,FAM_PCH,"I219-V"}, {0x57B5,FAM_PCH,"I219-LM"},
    {0x57B6,FAM_PCH,"I219-V"}, {0x57B7,FAM_PCH,"I219-LM"}, {0x57B8,FAM_PCH,"I219-V"},
    {0x57B9,FAM_PCH,"I219-LM"}, {0x57BA,FAM_PCH,"I219-V"},
    /* igb: 82575/82576/82580, I350, I354, I210, I211 */
    {0x10A7,FAM_IGB,"82575EB"}, {0x10A9,FAM_IGB,"82575EB"}, {0x10D6,FAM_IGB,"82575GB"},
    {0x10C9,FAM_IGB,"82576"}, {0x10E6,FAM_IGB,"82576"}, {0x10E7,FAM_IGB,"82576"},
    {0x10E8,FAM_IGB,"82576"}, {0x1526,FAM_IGB,"82576"}, {0x150A,FAM_IGB,"82576NS"},
    {0x1518,FAM_IGB,"82576NS"}, {0x150D,FAM_IGB,"82576"}, {0x150E,FAM_IGB,"82580"},
    {0x150F,FAM_IGB,"82580"}, {0x1510,FAM_IGB,"82580"}, {0x1511,FAM_IGB,"82580"},
    {0x1516,FAM_IGB,"82580"}, {0x1527,FAM_IGB,"82580"}, {0x1521,FAM_IGB,"I350"},
    {0x1522,FAM_IGB,"I350"}, {0x1523,FAM_IGB,"I350"}, {0x1524,FAM_IGB,"I350"},
    {0x1F40,FAM_IGB,"I354"}, {0x1F41,FAM_IGB,"I354"}, {0x1F45,FAM_IGB,"I354"},
    {0x1533,FAM_IGB,"I210"}, {0x1536,FAM_IGB,"I210"}, {0x1537,FAM_IGB,"I210"},
    {0x1538,FAM_IGB,"I210"}, {0x157B,FAM_IGB,"I210"}, {0x157C,FAM_IGB,"I210"},
    {0x1531,FAM_IGB,"I210"}, {0x1539,FAM_IGB,"I211"},
};

typedef struct { u64 addr; u16 len; u16 csum; u8 status; u8 errors; u16 special; } __attribute__((packed)) rxdesc_t;
typedef struct { u64 addr; u16 len; u8 cso; u8 cmd; u8 status; u8 css; u16 special; } __attribute__((packed)) txdesc_t;

static volatile u8 *mmio;
static int fam;
static rxdesc_t *rx;
static txdesc_t *tx;
static u8 *rxbuf[NRX], *txbuf[NTX];
static int rx_next, tx_next;
static nic_t nic_e1000;
static char nic_name[24];
static int eerd_new;        /* 82541/82547 use the newer EERD layout */

static inline void wr(u32 off, u32 v){ *(volatile u32*)(mmio + off) = v; }
static inline u32 rd(u32 off){ return *(volatile u32*)(mmio + off); }
static inline void flush(void){ (void)rd(STATUS); }
static void udelay(u32 us){ while(us--) __asm__ volatile("outb %%al,$0x80"::"a"(0)); }   /* ~1 us per port write */

/* ---------- PHY access (MDIO through MDIC) ---------- */
static int phy_addr(void){ return fam == FAM_PCH ? 2 : 1; }

/* the PCH PHY is shared with the management engine: take the software flag first */
static int pch_lock(void){
    if(fam != FAM_PCH) return 1;
    for(int t = 0; t < 1000; t++){
        wr(EXTCNF, rd(EXTCNF) | (1u << 5));
        if(rd(EXTCNF) & (1u << 5)) return 1;
        udelay(100);
    }
    return 0;
}
static void pch_unlock(void){ if(fam == FAM_PCH) wr(EXTCNF, rd(EXTCNF) & ~(1u << 5)); }

static int mdic(u32 op, int reg, u16 val, u16 *out){
    if(!pch_lock()) return -1;
    wr(MDIC, (u32)val | ((u32)reg << 16) | ((u32)phy_addr() << 21) | (op << 26));
    u32 v = 0;
    int t;
    for(t = 0; t < 2000; t++){ udelay(5); v = rd(MDIC); if(v & (1u << 28)) break; }
    pch_unlock();
    if(t == 2000 || (v & (1u << 30))) return -1;
    if(out) *out = (u16)v;
    return 0;
}
static int phy_read(int reg, u16 *v){ return mdic(2, reg, 0, v); }
static int phy_write(int reg, u16 v){ return mdic(1, reg, v, 0); }

/* I217/I218/I219 can be left in a low-power state by the firmware or another OS, in which
 * case the PHY does not answer. Toggling LANPHYPC power-cycles it (as Linux e1000e does). */
static void pch_phy_wake(void){
    wr(FEXTNVM3, (rd(FEXTNVM3) & ~(7u << 4)) | (1u << 4));   /* PHY config counter: 50 ms */
    wr(CTRL, (rd(CTRL) | CTRL_LPC_OVR) & ~CTRL_LPC_VAL);
    flush(); udelay(10);
    wr(CTRL, rd(CTRL) & ~CTRL_LPC_OVR);
    flush();
    for(int t = 0; t < 20 && !(rd(CTRL_EXT) & (1u << 2)); t++) timer_sleep(5);   /* LPCD */
    timer_sleep(30);
}

/* make sure the copper PHY is powered and auto-negotiating */
static void phy_start(void){
    u16 id = 0;
    if(fam == FAM_PCH && (phy_read(2, &id) != 0 || id == 0 || id == 0xFFFF)){
        pch_phy_wake();
        timer_sleep(10);
    }
    u16 bmcr;
    if(phy_read(0, &bmcr) != 0) return;
    /* restart auto-negotiation unless the MAC already sees a link: after a MAC reset some
     * parts (igb, and QEMU's model of it) only raise STATUS.LU when a negotiation completes */
    if((bmcr & (1u << 11)) || !(bmcr & (1u << 12)) || !(rd(STATUS) & STATUS_LU)){
        bmcr &= (u16)~(1u << 11);                   /* power up */
        bmcr |= (1u << 12) | (1u << 9);             /* auto-negotiate, restart */
        phy_write(0, bmcr);
    }
}

/* ---------- MAC address ---------- */
static int eeprom_word(int addr, u16 *out){
    /* 8254x-era parts: address at bit 8, done at bit 4; later parts: bit 2 / bit 1 */
    int newer = fam != FAM_E1000 || eerd_new;
    u32 cmd = newer ? (((u32)addr << 2) | 1) : (((u32)addr << 8) | 1);
    u32 done = newer ? (1u << 1) : (1u << 4);
    wr(EERD, cmd);
    for(int t = 0; t < 10000; t++){
        u32 v = rd(EERD);
        if(v & done){ *out = (u16)(v >> 16); return 0; }
        udelay(5);
    }
    return -1;
}

static int mac_ok(const u8 *m){
    int zero = 1, ones = 1;
    for(int i = 0; i < 6; i++){ if(m[i]) zero = 0; if(m[i] != 0xFF) ones = 0; }
    return !zero && !ones && !(m[0] & 1);
}

static void mac_from_ra(u8 *m){
    u32 ral = rd(RAL0), rah = rd(RAH0);
    m[0] = (u8)ral; m[1] = (u8)(ral >> 8); m[2] = (u8)(ral >> 16); m[3] = (u8)(ral >> 24);
    m[4] = (u8)rah; m[5] = (u8)(rah >> 8);
}

/* ---------- data path ---------- */
static int e1000_send(const u8 *frame, u32 len){
    if(len > BUFSZ) return -1;
    int i = tx_next;
    /* wait until this slot is free (the device sets DD when it is done with it) */
    for(int t = 0; t < 1000000 && !(tx[i].status & 1) && tx[i].cmd; t++) ;
    memcpy(txbuf[i], frame, len);
    if(len < 60){ memset(txbuf[i] + len, 0, 60 - len); len = 60; }
    tx[i].addr = (u64)(unsigned long)txbuf[i];
    tx[i].len = (u16)len;
    tx[i].cso = 0; tx[i].css = 0; tx[i].special = 0;
    tx[i].cmd = 0x0B;               /* EOP | IFCS | RS */
    tx[i].status = 0;
    tx_next = (i + 1) % NTX;
    __asm__ volatile("sfence" ::: "memory");
    wr(TDT, (u32)tx_next);
    return 0;
}

/* igb receive: advanced one-buffer descriptors. The card overwrites the buffer address with
 * {info, rss, status/error, length, vlan} when it is done, so the address is put back. */
static int igb_recv(u8 *frame, u32 max, u32 *len){
    for(;;){
        volatile u32 *d = (volatile u32*)&rx[rx_next];
        u32 st = d[2];
        if(!(st & 1)) return 0;                         /* DD */
        __asm__ volatile("lfence" ::: "memory");
        int good = (st & 2) && !(st & (1u << 31));      /* EOP, no RXE */
        u32 n = d[3] & 0xFFFF;
        if(good){
            if(n > max) n = max;
            memcpy(frame, rxbuf[rx_next], n);
            *len = n;
        }
        rx[rx_next].addr = (u64)(unsigned long)rxbuf[rx_next];
        d[2] = 0; d[3] = 0;                             /* header address: none */
        __asm__ volatile("sfence" ::: "memory");
        wr(RDT, (u32)rx_next);
        rx_next = (rx_next + 1) % NRX;
        if(good) return 1;
    }
}

static int e1000_recv(u8 *frame, u32 max, u32 *len){
    for(;;){
        volatile rxdesc_t *d = &rx[rx_next];
        u8 st = d->status;
        if(!(st & 1)) return 0;
        __asm__ volatile("lfence" ::: "memory");
        int good = (st & 2) && !d->errors;          /* EOP, no errors (frames fit one buffer) */
        u32 n = d->len;
        if(good){
            if(n > max) n = max;
            memcpy(frame, rxbuf[rx_next], n);
            *len = n;
        }
        d->status = 0;
        wr(RDT, (u32)rx_next);          /* give the slot back to the card */
        rx_next = (rx_next + 1) % NRX;
        if(good) return 1;
    }
}

static int e1000_link(void){ return (rd(STATUS) & STATUS_LU) ? 1 : 0; }

static const e1k_id_t *match(pci_dev_t **out){
    for(int i = 0; i < pci_count(); i++){
        pci_dev_t *d = pci_get(i);
        if(d->vendor != 0x8086) continue;
        for(u32 k = 0; k < sizeof ids / sizeof ids[0]; k++)
            if(ids[k].id == d->device){ *out = d; return &ids[k]; }
    }
    return 0;
}

int e1000_init(void){
    pci_dev_t *d = 0;
    const e1k_id_t *id = match(&d);
    if(!id) return -1;
    fam = id->fam;
    eerd_new = id->name[3] == '4' && (id->name[4] == '1' || id->name[4] == '7');
    pci_enable(d);
    u64 base = pci_bar_base(d, 0);
    if(!base) return -1;
    vmm_map_region(base, base, 0x20000);
    mmio = (volatile u8*)(unsigned long)base;
    strcpy(nic_name, "e1000 ");
    strcat(nic_name, id->name);
    nic_e1000.name = nic_name;

    /* the address the firmware already loaded: kept in case a reset wipes it (PCH parts) */
    u8 early[6];
    mac_from_ra(early);

    wr(IMC, 0xFFFFFFFF);                        /* no interrupts, we poll */
    wr(RCTL, 0); wr(TCTL, 0);
    flush();
    timer_sleep(10);                            /* let in-flight DMA drain */

    u32 rst = CTRL_RST;
    if(fam == FAM_PCH){ pch_lock(); rst |= CTRL_PHYRST; }
    wr(CTRL, rd(CTRL) | rst);
    timer_sleep(20);
    for(int t = 0; t < 1000 && (rd(CTRL) & CTRL_RST); t++) udelay(10);
    if(fam == FAM_PCH) pch_unlock();
    timer_sleep(fam == FAM_E1000 ? 2 : 20);    /* NVM auto-read after reset */
    wr(IMC, 0xFFFFFFFF);
    (void)rd(ICR);

    u32 ctrl = rd(CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~((1u << 3) | (1u << 7) | (1u << 30) | (1u << 31));   /* LRST, ILOS, VME, PHY_RST off */
    wr(CTRL, ctrl);
    phy_start();

    /* MAC: receive-address register, else EEPROM words 0..2, else what firmware left */
    u8 *m = nic_e1000.mac;
    mac_from_ra(m);
    if(!mac_ok(m) && fam != FAM_PCH){
        u16 w[3];
        if(eeprom_word(0, &w[0]) == 0 && eeprom_word(1, &w[1]) == 0 && eeprom_word(2, &w[2]) == 0)
            for(int i = 0; i < 3; i++){ m[2*i] = (u8)w[i]; m[2*i + 1] = (u8)(w[i] >> 8); }
    }
    if(!mac_ok(m)) memcpy(m, early, 6);
    if(!mac_ok(m)){ static const u8 fb[6] = {0x02,0x41,0x4D,0x44,0x52,0x01}; memcpy(m, fb, 6); }
    wr(RAL0, (u32)m[0] | ((u32)m[1] << 8) | ((u32)m[2] << 16) | ((u32)m[3] << 24));
    wr(RAH0, (u32)m[4] | ((u32)m[5] << 8) | (1u << 31));   /* address valid */
    for(int i = 0; i < 128; i++) wr(MTA + 4*i, 0);

    rx = pmm_alloc_page();
    tx = pmm_alloc_page();
    if(!rx || !tx) return -1;
    memset(rx, 0, 4096); memset(tx, 0, 4096);
    for(int i = 0; i < NRX; i++){
        if(!rxbuf[i]) rxbuf[i] = pmm_alloc_page();
        if(!rxbuf[i]) return -1;
        rx[i].addr = (u64)(unsigned long)rxbuf[i];
    }
    for(int i = 0; i < NTX; i++){
        if(!txbuf[i]) txbuf[i] = pmm_alloc_page();
        if(!txbuf[i]) return -1;
        tx[i].status = 1;           /* free */
    }

    /* receive ring */
    /* igb: 2 KB buffers, advanced one-buffer descriptors, drop when the ring is full */
    if(fam == FAM_IGB) wr(SRRCTL0, 2 | (1u << 25) | (1u << 31));
    wr(RDBAL, (u32)(unsigned long)rx); wr(RDBAH, (u32)((u64)(unsigned long)rx >> 32));
    wr(RDLEN, NRX * sizeof(rxdesc_t));
    wr(RDH, 0); wr(RDT, 0);
    rx_next = 0;
    if(fam == FAM_IGB){
        wr(RXDCTL, rd(RXDCTL) | QUEUE_EN);
        for(int t = 0; t < 1000 && !(rd(RXDCTL) & QUEUE_EN); t++) udelay(10);
    }
    /* enable, accept broadcast, 2 KB buffers, strip CRC */
    wr(RCTL, (1u << 1) | (1u << 15) | (1u << 26));
    wr(RDT, NRX - 1);

    /* transmit ring */
    wr(TDBAL, (u32)(unsigned long)tx); wr(TDBAH, (u32)((u64)(unsigned long)tx >> 32));
    wr(TDLEN, NTX * sizeof(txdesc_t));
    wr(TDH, 0); wr(TDT, 0);
    tx_next = 0;
    if(fam == FAM_IGB){
        wr(TXDCTL, rd(TXDCTL) | QUEUE_EN);
        for(int t = 0; t < 1000 && !(rd(TXDCTL) & QUEUE_EN); t++) udelay(10);
    } else {
        wr(TIPG, 0x0060200A);                   /* copper inter-packet gap (IEEE 802.3) */
    }
    /* enable, pad short packets, collision threshold, collision distance */
    wr(TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12) | (1u << 24));

    nic_e1000.send = e1000_send;
    nic_e1000.recv = fam == FAM_IGB ? igb_recv : e1000_recv;
    nic_e1000.link = e1000_link;
    console_printf("[e1000] %s %04x  %02x:%02x:%02x:%02x:%02x:%02x\n", id->name, id->id,
                   m[0], m[1], m[2], m[3], m[4], m[5]);
    return 0;
}

nic_t *e1000_nic(void){ return &nic_e1000; }
