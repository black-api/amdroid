#include "amdroid/fat32.h"
#include "amdroid/ata.h"
#include "amdroid/console.h"
#include "amdroid/heap.h"
#include "amdroid/types.h"
#include <string.h>

static u8 sector_buf[512];
static u32 bytes_per_sector, sectors_per_cluster, reserved_sectors, num_fats, fat_size, root_cluster;
static u32 fat_start, data_start;

static int read_sector(u32 lba){ return ata_read_sectors(lba,1,sector_buf); }

int fat32_init(void){
    if(ata_init()!=0) return -1;
    if(read_sector(0)!=0) return -1;
    /* BPB */
    bytes_per_sector = *(u16*)(sector_buf+11);
    sectors_per_cluster = sector_buf[13];
    reserved_sectors = *(u16*)(sector_buf+14);
    num_fats = sector_buf[16];
    /* total sectors 16/32 */
    u32 total16 = *(u16*)(sector_buf+19);
    u32 total32 = *(u32*)(sector_buf+32);
    u32 total = total16 ? total16 : total32;
    fat_size = *(u32*)(sector_buf+36);
    root_cluster = *(u32*)(sector_buf+44);
    if(bytes_per_sector != 512){ console_printf("[fat32] bad bps %u\n", bytes_per_sector); return -1; }
    fat_start = reserved_sectors;
    data_start = reserved_sectors + num_fats * fat_size;
    console_printf("[fat32] mounted, root cluster %u\n", root_cluster);
    return 0;
}

static u32 cluster_to_lba(u32 c){ return data_start + (c-2)*sectors_per_cluster; }

static u32 fat_next(u32 c){
    /* read FAT entry (4 bytes per cluster in FAT32) */
    u32 fat_offset = c*4;
    u32 fat_sector = fat_start + fat_offset/512;
    u32 ent_offset = fat_offset % 512;
    if(read_sector(fat_sector)!=0) return 0x0FFFFFFF;
    u32 val = *(u32*)(sector_buf + ent_offset);
    return val & 0x0FFFFFFF;
}

static void read_cluster(u32 c, u8 *dst){
    u32 lba = cluster_to_lba(c);
    for(u32 i=0;i<sectors_per_cluster;i++){
        read_sector(lba+i);
        memcpy(dst + i*512, sector_buf, 512);
    }
}

int fat32_list_root(void (*cb)(const char*,int,u32)){
    /* iterate root cluster chain */
    u32 c = root_cluster;
    u8 *buf = kmalloc(sectors_per_cluster*512);
    while(c < 0x0FFFFFF8){
        read_cluster(c, buf);
        /* directory entries: 32 bytes each */
        u32 entries = sectors_per_cluster*512/32;
        for(u32 i=0;i<entries;i++){
            u8 *e = buf + i*32;
            if(e[0]==0) { /* end */ 
                /* still continue cluster? break out */
                i=entries; continue;
            }
            if(e[0]==0xE5) continue; /* deleted */
            if(e[11]==0x0F) continue; /* LFN */
            if(e[11]&0x08) continue;  /* volume label */
            /* name */
            char name[13];
            int ni=0;
            for(int k=0;k<8;k++){ if(e[k]!=' ') name[ni++]=e[k]; }
            if(e[8]!=' '){ name[ni++]='.'; for(int k=8;k<11;k++){ if(e[k]!=' ') name[ni++]=e[k]; } }
            name[ni]=0;
            int is_dir = (e[11]&0x10)!=0;
            u32 size = *(u32*)(e+28);
            cb(name, is_dir, size);
        }
        c = fat_next(c);
    }
    kfree(buf);
    return 0;
}

int fat32_read_file(const char *name, void *buf, u32 max, u32 *out_size){
    u32 c = root_cluster;
    u8 *clbuf = kmalloc(sectors_per_cluster*512);
    /* find entry */
    u32 first_cluster=0; u32 fsize=0; int found=0;
    u32 cc = root_cluster;
    while(cc < 0x0FFFFFF8 && !found){
        read_cluster(cc, clbuf);
        u32 entries = sectors_per_cluster*512/32;
        for(u32 i=0;i<entries;i++){
            u8 *e = clbuf + i*32;
            if(e[0]==0) { i=entries; continue; }
            if(e[0]==0xE5) continue;
            if(e[11]==0x0F) continue;
            char ename[13];
            int ni=0;
            for(int k=0;k<8;k++){ if(e[k]!=' ') ename[ni++]=e[k]; }
            if(e[8]!=' '){ ename[ni++]='.'; for(int k=8;k<11;k++){ if(e[k]!=' ') ename[ni++]=e[k]; } }
            ename[ni]=0;
            if(strcmp(ename,name)==0){
                first_cluster = (*(u16*)(e+26)) | ((*(u16*)(e+20))<<16);
                fsize = *(u32*)(e+28);
                found=1; break;
            }
        }
        cc = fat_next(cc);
    }
    if(!found){ kfree(clbuf); return -1; }
    /* read clusters */
    u32 toread = fsize; if(toread>max) toread=max;
    u32 done=0;
    cc = first_cluster;
    while(cc < 0x0FFFFFF8 && done < toread){
        read_cluster(cc, clbuf);
        u32 chunk = sectors_per_cluster*512;
        if(chunk > toread-done) chunk = toread-done;
        memcpy((char*)buf+done, clbuf, chunk);
        done += chunk;
        cc = fat_next(cc);
    }
    if(out_size) *out_size = fsize;
    kfree(clbuf);
    return 0;
}
