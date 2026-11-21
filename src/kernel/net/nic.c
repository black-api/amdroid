/* nic.c — picks the network card. Virtual machines first (virtio-net, then the Intel e1000
 * family that QEMU, VirtualBox and VMware emulate), then the Realtek cards most real PCs
 * have. Waits a few seconds for the cable link so DHCP does not start on a dead wire. */
#include "amdroid/nic.h"
#include "amdroid/console.h"
#include "amdroid/timer.h"
#include "amdroid/types.h"

int e1000_init(void);   nic_t *e1000_nic(void);
int virtio_net_init(void); nic_t *virtio_nic(void);
int rtl8169_init(void); nic_t *rtl8169_nic(void);
int rtl8139_init(void); nic_t *rtl8139_nic(void);

static const struct { int (*init)(void); nic_t *(*get)(void); } drivers[] = {
    { e1000_init, e1000_nic },
    { virtio_net_init, virtio_nic },
    { rtl8169_init, rtl8169_nic },
    { rtl8139_init, rtl8139_nic },
};

static nic_t *active;

int nic_init(void){
    for(u32 i = 0; i < sizeof drivers / sizeof drivers[0] && !active; i++)
        if(drivers[i].init() == 0) active = drivers[i].get();
    if(!active){ console_puts("[net] no network card\n"); return -1; }
    if(active->link && !active->link()){
        /* auto-negotiation on real hardware takes 2-3 s after a reset */
        u64 until = timer_ticks() + 500;
        while(timer_ticks() < until && !active->link()) timer_sleep(50);
        if(!active->link()) console_printf("[net] %s: no cable link\n", active->name);
    }
    return 0;
}

nic_t *nic_get(void){ return active; }
