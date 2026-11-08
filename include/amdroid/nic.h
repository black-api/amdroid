#ifndef AMDROID_NIC_H
#define AMDROID_NIC_H
#include "amdroid/types.h"

/* A network interface card. The drivers (e1000, virtio-net) fill this in. */
typedef struct {
    const char *name;
    u8 mac[6];
    int (*send)(const u8 *frame, u32 len);          /* 0 on success */
    int (*recv)(u8 *frame, u32 max, u32 *len);       /* 1 if a frame was received */
} nic_t;

int nic_init(void);                 /* finds and starts a supported card; 0 if one is up */
nic_t *nic_get(void);
#endif
