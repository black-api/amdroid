/* nic.c — picks the network card: e1000 first, then virtio-net. */
#include "amdroid/nic.h"
#include "amdroid/console.h"
#include "amdroid/types.h"

int e1000_init(void);
nic_t *e1000_nic(void);
int virtio_net_init(void);
nic_t *virtio_nic(void);

static nic_t *active;

int nic_init(void){
    if(e1000_init() == 0) active = e1000_nic();
    else if(virtio_net_init() == 0) active = virtio_nic();
    if(!active){ console_puts("[net] no network card\n"); return -1; }
    return 0;
}

nic_t *nic_get(void){ return active; }
