#include "amdroid/fat32.h"
#include "amdroid/ata.h"
#include "amdroid/console.h"
#include "amdroid/heap.h"
#include "amdroid/types.h"
#include "amdroid/usb.h"
#include <string.h>

/* The volume the driver works on: the boot ATA disk, or a USB disk after
   fat32_mount_usb(). Every disk access goes through these two helpers. */
static int on_usb;
static int vol_read(u32 lba, u32 n, void *buf){
    return on_usb ? usb_disk_read(lba, n, buf) : ata_read_sectors(lba, (u8)n, buf);
}
static int vol_write(u32 lba, u32 n, const void *buf){
    return on_usb ? usb_disk_write(lba, n, buf) : ata_write_sectors(lba, (u8)n, buf);
}

static u8 sector_buf[512];
static u32 bytes_per_sector, sectors_per_cluster, reserved_sectors, num_fats, fat_size, root_cluster;
static u32 fat_start, data_start, total_clusters, fsinfo_sector;

static int read_sector(u32 lba){ return vol_read(lba,1,sector_buf); }

/* reads the BPB of the current volume and sets up the driver's view of it */
static int mount_volume(void){
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
    total_clusters = (total - data_start) / sectors_per_cluster;
    fsinfo_sector = *(u16*)(sector_buf+48);
    console_printf("[fat32] mounted, root cluster %u\n", root_cluster);
    return 0;
}

int fat32_init(void){
    on_usb = 0;
    if(ata_init()!=0) return -1;
    return mount_volume();
}

/* Switches the driver to a FAT32 file system on the USB disk. */
int fat32_mount_usb(void){
    if(!usb_disk_present()) return -1;
    u32 bsz = 0;
    usb_disk_info(0, &bsz, 0, 0);
    if(bsz != 512) return -1;
    on_usb = 1;
    if(mount_volume() != 0){ on_usb = 0; return -1; }
    return 0;
}

/* Switches back to the boot disk. */
int fat32_mount_boot(void){
    on_usb = 0;
    return mount_volume();
}

int fat32_on_usb(void){ return on_usb; }

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

/* ======================= writing =======================
   Only the root directory is supported, with 8.3 names. Every FAT copy is
   updated, so fsck.fat stays clean. A file is written before its directory
   entry, so a failure mid-write leaves the old entry in place. */

static u8 wbuf[512] __attribute__((aligned(4)));

static int write_sector(u32 lba, const void *buf){ return vol_write(lba, 1, buf); }

/* Sets one FAT entry in every FAT copy. */
static int fat_set(u32 c, u32 val){
    u32 off = c * 4;
    for(u32 f = 0; f < num_fats; f++){
        u32 sec = fat_start + f * fat_size + off / 512;
        if(read_sector(sec) != 0) return -1;
        memcpy(wbuf, sector_buf, 512);
        u32 *e = (u32*)(wbuf + off % 512);
        *e = (*e & 0xF0000000u) | (val & 0x0FFFFFFFu);
        if(write_sector(sec, wbuf) != 0) return -1;
    }
    return 0;
}

static u32 fat_get(u32 c){ return fat_next(c); }

static void fat_free_chain(u32 c){
    while(c >= 2 && c < 0x0FFFFFF8){
        u32 nxt = fat_get(c);
        fat_set(c, 0);
        c = nxt;
    }
}

/* Allocates a chain of n clusters. Returns 0 and the first cluster. */
static int fat_alloc_chain(u32 n, u32 *first){
    u32 prev = 0, got = 0;
    *first = 0;
    for(u32 c = 2; got < n && c < total_clusters + 2; c++){
        if(fat_get(c) != 0) continue;
        if(prev) { if(fat_set(prev, c) != 0) goto fail; }
        else *first = c;
        prev = c;
        got++;
    }
    if(got < n) goto fail;
    if(fat_set(prev, 0x0FFFFFFF) != 0) goto fail;
    return 0;
fail:
    if(*first) fat_free_chain(*first);
    *first = 0;
    return -1;
}

static int write_cluster(u32 c, const u8 *src){
    u32 lba = cluster_to_lba(c);
    for(u32 i = 0; i < sectors_per_cluster; i++)
        if(write_sector(lba + i, src + i * 512) != 0) return -1;
    return 0;
}

/* Converts "NAME.EXT" (upper case, 8.3) to the 11-byte directory form. */
static int to_83(const char *name, u8 out[11]){
    memset(out, ' ', 11);
    const char *dot = strchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot ? strlen(dot + 1) : 0;
    if(base == 0 || base > 8 || ext > 3) return -1;
    memcpy(out, name, base);
    if(dot) memcpy(out + 8, dot + 1, ext);
    return 0;
}

struct dirloc { u32 lba, off; };
static struct dirloc dir_found, dir_free;
static int dir_has_found, dir_has_free;

/* Scans the root directory for a name (found) and for a reusable slot (free).
   Extends the directory with a new cluster when it is full. */
static int dir_scan_in(u32 start, const u8 n83[11]){
    dir_has_found = dir_has_free = 0;
    u32 c = start, last = 0;
    u32 per = sectors_per_cluster * 512 / 32;
    while(c >= 2 && c < 0x0FFFFFF8){
        for(u32 s = 0; s < sectors_per_cluster; s++){
            u32 lba = cluster_to_lba(c) + s;
            if(read_sector(lba) != 0) return -1;
            for(u32 i = 0; i < 512 / 32; i++){
                u8 *e = sector_buf + i * 32;
                if(e[0] == 0x00 || e[0] == 0xE5){
                    if(!dir_has_free){ dir_has_free = 1; dir_free.lba = lba; dir_free.off = i * 32; }
                    if(e[0] == 0x00) goto scan_done_chain;   /* rest of directory is empty */
                    continue;
                }
                if(e[11] == 0x0F || (e[11] & 0x08)) continue;
                if(!memcmp(e, n83, 11) && !dir_has_found){
                    dir_has_found = 1; dir_found.lba = lba; dir_found.off = i * 32;
                }
            }
        }
        last = c;
        c = fat_get(c);
    }
scan_done_chain:
    (void)per;
    if(!dir_has_free){
        /* directory full: add a zeroed cluster to the end of the chain */
        u32 nc;
        if(fat_alloc_chain(1, &nc) != 0) return -1;
        if(fat_set(last, nc) != 0) return -1;
        memset(wbuf, 0, 512);
        u32 lba = cluster_to_lba(nc);
        for(u32 s = 0; s < sectors_per_cluster; s++)
            if(write_sector(lba + s, wbuf) != 0) return -1;
        dir_has_free = 1;
        dir_free.lba = lba; dir_free.off = 0;
    }
    (void)last;
    return 0;
}

static int dir_scan(const u8 n83[11]){ return dir_scan_in(root_cluster, n83); }

static int dir_entry_write(struct dirloc *loc, const u8 *entry32){
    if(read_sector(loc->lba) != 0) return -1;
    memcpy(wbuf, sector_buf, 512);
    memcpy(wbuf + loc->off, entry32, 32);
    return write_sector(loc->lba, wbuf);
}

/* The free-cluster hints in FSInfo go stale after writes. 0xFFFFFFFF means
   "unknown", which the FAT32 spec allows, so mark them that way. */
static void fsinfo_invalidate(void){
    if(!fsinfo_sector) return;
    if(read_sector(fsinfo_sector) != 0) return;
    memcpy(wbuf, sector_buf, 512);
    *(u32*)(wbuf + 488) = 0xFFFFFFFFu;
    *(u32*)(wbuf + 492) = 0xFFFFFFFFu;
    write_sector(fsinfo_sector, wbuf);
}

static int delete_entry(void);

/* Writes a file whose directory has been scanned (dir_scan_in). */
static int write_entry(const void *data, u32 size, const u8 n83[11]){
    /* Crash-safe order: write the new data first, switch the directory entry
       (one sector write), and only then free the old clusters. A crash at any
       point leaves either the old file or the new one, never a dangling entry. */
    u32 old_first = 0;
    if(dir_has_found){
        if(read_sector(dir_found.lba) != 0) return -1;
        u8 *e = sector_buf + dir_found.off;
        old_first = (*(u16*)(e + 26)) | ((u32)(*(u16*)(e + 20)) << 16);
    }
    u32 csize = sectors_per_cluster * 512;
    u32 ncl = (size + csize - 1) / csize;
    u32 first = 0;
    if(ncl && fat_alloc_chain(ncl, &first) != 0) return -1;

    u8 *cbuf = kmalloc(csize);
    if(!cbuf){ if(first) fat_free_chain(first); return -1; }
    const u8 *src = data;
    u32 left = size, c = first;
    for(u32 k = 0; k < ncl; k++){
        u32 n = left < csize ? left : csize;
        memset(cbuf, 0, csize);
        memcpy(cbuf, src, n);
        if(write_cluster(c, cbuf) != 0){ kfree(cbuf); if(first) fat_free_chain(first); return -1; }
        src += n; left -= n;
        c = fat_get(c);
    }
    kfree(cbuf);

    struct dirloc loc = dir_has_found ? dir_found : dir_free;
    u8 e[32];
    memset(e, 0, 32);
    memcpy(e, n83, 11);
    e[11] = 0x20;                      /* archive */
    *(u16*)(e + 20) = (u16)(first >> 16);
    *(u16*)(e + 26) = (u16)(first & 0xFFFF);
    *(u32*)(e + 28) = size;
    if(dir_entry_write(&loc, e) != 0){ if(first) fat_free_chain(first); return -1; }
    if(old_first) fat_free_chain(old_first);
    fsinfo_invalidate();
    return 0;
}

int fat32_write_file(const char *name, const void *data, u32 size){
    u8 n83[11];
    if(to_83(name, n83) != 0) return -1;
    if(dir_scan(n83) != 0) return -1;
    return write_entry(data, size, n83);
}

int fat32_delete_file(const char *name){
    u8 n83[11];
    if(to_83(name, n83) != 0) return -1;
    if(dir_scan(n83) != 0 || !dir_has_found) return -1;
    return delete_entry();
}

static int delete_entry(void){
    if(!dir_has_found) return -1;
    if(read_sector(dir_found.lba) != 0) return -1;
    /* copy the entry first: fat_free_chain() reuses sector_buf */
    u8 gone[32];
    memcpy(gone, sector_buf + dir_found.off, 32);
    u32 first = (*(u16*)(gone + 26)) | ((u32)(*(u16*)(gone + 20)) << 16);
    fat_free_chain(first);
    gone[0] = 0xE5;
    if(dir_entry_write(&dir_found, gone) != 0) return -1;
    fsinfo_invalidate();
    return 0;
}

/* ======================= subdirectory reads =======================
   Paths like "LIB/UTIL.FAS" are walked one 8.3 component at a time. Reads
   only; writes stay in the root directory. */

/* Looks for an 8.3 name in one directory. Returns 1 and a copy of the entry,
   0 if absent, -1 on a disk error. */
static int dir_lookup(u32 dir, const u8 n83[11], u8 out[32]){
    u32 c = dir;
    while(c >= 2 && c < 0x0FFFFFF8){
        for(u32 s = 0; s < sectors_per_cluster; s++){
            if(read_sector(cluster_to_lba(c) + s) != 0) return -1;
            for(u32 i = 0; i < 512 / 32; i++){
                u8 *e = sector_buf + i * 32;
                if(e[0] == 0x00) return 0;
                if(e[0] == 0xE5 || e[11] == 0x0F) continue;
                if(!memcmp(e, n83, 11)){ memcpy(out, e, 32); return 1; }
            }
        }
        c = fat_next(c);
    }
    return 0;
}

/* Converts one path component (len bytes, upper case) to the 11-byte form. */
static int comp_to_83(const char *s, size_t len, u8 out[11]){
    memset(out, ' ', 11);
    size_t base = 0;
    while(base < len && s[base] != '.') base++;
    size_t ext = 0;
    if(base < len) ext = len - base - 1;
    if(base == 0 || base > 8 || ext > 3) return -1;
    memcpy(out, s, base);
    if(base < len) memcpy(out + 8, s + base + 1, ext);
    return 0;
}

int fat32_read_path(const char *path, void *buf, u32 max, u32 *out_size){
    u32 dir = root_cluster;
    const char *p = path;
    for(;;){
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        u8 n83[11], ent[32];
        if(comp_to_83(p, len, n83) != 0) return -1;
        if(dir_lookup(dir, n83, ent) != 1) return -1;
        if(!slash){
            if(ent[11] & 0x10) return -1;   /* a directory, not a file */
            u32 first = (*(u16*)(ent + 26)) | ((u32)(*(u16*)(ent + 20)) << 16);
            u32 fsize = *(u32*)(ent + 28);
            if(out_size) *out_size = fsize;
            u32 toread = fsize < max ? fsize : max;
            u32 done = 0;
            u32 c = first;
            while(c >= 2 && c < 0x0FFFFFF8 && done < toread){
                u8 *clbuf = kmalloc(sectors_per_cluster * 512);
                if(!clbuf) return -1;
                read_cluster(c, clbuf);
                u32 chunk = sectors_per_cluster * 512;
                if(chunk > toread - done) chunk = toread - done;
                memcpy((u8*)buf + done, clbuf, chunk);
                kfree(clbuf);
                done += chunk;
                c = fat_next(c);
            }
            return 0;
        }
        if(!(ent[11] & 0x10)) return -1;     /* not a directory */
        dir = (*(u16*)(ent + 26)) | ((u32)(*(u16*)(ent + 20)) << 16);
        p = slash + 1;
    }
}

/* ======================= paths anywhere in the disk =======================
   Walks the directories before the last component. Works for writes,
   deletes, new directories and listings. */

/* Cluster of a directory entry (0 for the root's ".." entries). */
static u32 entry_cluster(const u8 *e){
    return (*(u16*)(e + 26)) | ((u32)(*(u16*)(e + 20)) << 16);
}

/* Finds the directory that holds the last component of path. */
static int resolve_parent(const char *path, u32 *dir, u8 leaf[11]){
    u32 cl = root_cluster;
    const char *p = path;
    while(*p == '/') p++;
    for(;;){
        const char *slash = strchr(p, '/');
        if(!slash){
            if(comp_to_83(p, strlen(p), leaf) != 0) return -1;
            *dir = cl;
            return 0;
        }
        u8 n83[11], ent[32];
        if(comp_to_83(p, (size_t)(slash - p), n83) != 0) return -1;
        if(dir_lookup(cl, n83, ent) != 1 || !(ent[11] & 0x10)) return -1;
        cl = entry_cluster(ent);
        p = slash + 1;
        while(*p == '/') p++;
    }
}

int fat32_write_path(const char *path, const void *data, u32 size){
    u32 dir; u8 leaf[11];
    if(resolve_parent(path, &dir, leaf) != 0) return -1;
    if(dir_scan_in(dir, leaf) != 0) return -1;
    return write_entry(data, size, leaf);
}

int fat32_delete_path(const char *path){
    u32 dir; u8 leaf[11];
    if(resolve_parent(path, &dir, leaf) != 0) return -1;
    if(dir_scan_in(dir, leaf) != 0 || !dir_has_found) return -1;
    return delete_entry();
}

/* Creates an empty directory: one cluster holding "." and "..". */
int fat32_mkdir(const char *path){
    u32 dir; u8 leaf[11];
    if(resolve_parent(path, &dir, leaf) != 0) return -1;
    if(dir_scan_in(dir, leaf) != 0 || dir_has_found || !dir_has_free) return -1;
    u32 nc;
    if(fat_alloc_chain(1, &nc) != 0) return -1;
    /* zero the new directory's clusters, then write its two entries */
    memset(wbuf, 0, 512);
    u32 lba = cluster_to_lba(nc);
    for(u32 s = 0; s < sectors_per_cluster; s++)
        if(write_sector(lba + s, wbuf) != 0){ fat_free_chain(nc); return -1; }
    u8 dot[32], dotdot[32];
    memset(dot, ' ', 11); memset(dotdot, ' ', 11);
    dot[0] = '.';
    dotdot[0] = '.'; dotdot[1] = '.';
    dot[11] = dotdot[11] = 0x10;
    memset(dot + 12, 0, 20); memset(dotdot + 12, 0, 20);
    *(u16*)(dot + 20) = (u16)(nc >> 16);  *(u16*)(dot + 26) = (u16)(nc & 0xFFFF);
    u32 parent = dir == root_cluster ? 0 : dir;
    *(u16*)(dotdot + 20) = (u16)(parent >> 16); *(u16*)(dotdot + 26) = (u16)(parent & 0xFFFF);
    if(read_sector(lba) != 0) return -1;
    memcpy(wbuf, sector_buf, 512);
    memcpy(wbuf, dot, 32);
    memcpy(wbuf + 32, dotdot, 32);
    if(write_sector(lba, wbuf) != 0){ fat_free_chain(nc); return -1; }
    /* the entry in the parent directory */
    u8 e[32];
    memset(e, 0, 32);
    memcpy(e, leaf, 11);
    e[11] = 0x10;
    *(u16*)(e + 20) = (u16)(nc >> 16);
    *(u16*)(e + 26) = (u16)(nc & 0xFFFF);
    if(dir_entry_write(&dir_free, e) != 0){ fat_free_chain(nc); return -1; }
    fsinfo_invalidate();
    return 0;
}

/* Lists the names in a directory given by path ("" or "/" for the root). */
int fat32_list_path(const char *path, void (*cb)(const char *name, int is_dir, u32 size)){
    u32 cl = root_cluster;
    const char *p = path;
    while(*p == '/') p++;
    while(*p){
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        u8 n83[11], ent[32];
        if(comp_to_83(p, len, n83) != 0) return -1;
        if(dir_lookup(cl, n83, ent) != 1 || !(ent[11] & 0x10)) return -1;
        cl = entry_cluster(ent);
        if(!slash) break;
        p = slash + 1;
        while(*p == '/') p++;
    }
    u8 *buf = kmalloc(sectors_per_cluster * 512);
    if(!buf) return -1;
    u32 c = cl;
    while(c >= 2 && c < 0x0FFFFFF8){
        read_cluster(c, buf);
        u32 entries = sectors_per_cluster * 512 / 32;
        for(u32 i = 0; i < entries; i++){
            u8 *e = buf + i * 32;
            if(e[0] == 0) { i = entries; continue; }
            if(e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
            if(e[0] == '.') continue;
            char name[13]; int ni = 0;
            for(int k = 0; k < 8; k++) if(e[k] != ' ') name[ni++] = (char)e[k];
            if(e[8] != ' '){ name[ni++] = '.'; for(int k = 8; k < 11; k++) if(e[k] != ' ') name[ni++] = (char)e[k]; }
            name[ni] = 0;
            cb(name, (e[11] & 0x10) != 0, *(u32*)(e + 28));
        }
        c = fat_next(c);
    }
    kfree(buf);
    return 0;
}

/* ======================= recovery =======================
 * A crash in the middle of a write can leave clusters that are marked as used
 * but belong to no file (the data itself is never lost: the directory entry is
 * written before the old data is released). fat32_recover() walks the whole
 * tree, marks what is referenced, and frees the rest. Returns the count freed,
 * or -1 on a disk error. */

static u8 *used_map;
static int recover_err;

static void mark_chain(u32 c){
    u32 guard = 0;
    while(c >= 2 && c < 0x0FFFFFF8 && guard++ <= total_clusters){
        if(used_map[c >> 3] & (1u << (c & 7))) break;      /* already seen: stop */
        used_map[c >> 3] |= (u8)(1u << (c & 7));
        c = fat_next(c);
    }
}

static void walk_dir(u32 start, int depth){
    if(depth > 32) return;
    mark_chain(start);
    u8 *buf = kmalloc(sectors_per_cluster * 512);
    if(!buf){ recover_err = 1; return; }
    u32 c = start, guard = 0;
    while(c >= 2 && c < 0x0FFFFFF8 && guard++ <= total_clusters){
        read_cluster(c, buf);
        u32 entries = sectors_per_cluster * 512 / 32;
        for(u32 i = 0; i < entries; i++){
            u8 *e = buf + i * 32;
            if(e[0] == 0) break;
            if(e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08) || e[0] == '.') continue;
            u32 first = entry_cluster(e);
            if(e[11] & 0x10){
                walk_dir(first, depth + 1);
            } else {
                mark_chain(first);
            }
        }
        c = fat_next(c);
    }
    kfree(buf);
}

int fat32_recover(void){
    u32 words = (total_clusters + 2) / 8 + 1;
    used_map = kmalloc(words);
    if(!used_map) return -1;
    memset(used_map, 0, words);
    recover_err = 0;
    walk_dir(root_cluster, 0);
    if(recover_err){ kfree(used_map); return -1; }
    int freed = 0;
    for(u32 c = 2; c < total_clusters + 2; c++){
        if(used_map[c >> 3] & (1u << (c & 7))) continue;
        if(fat_get(c) == 0) continue;
        if(fat_set(c, 0) == 0) freed++;
    }
    kfree(used_map);
    if(freed) fsinfo_invalidate();
    return freed;
}
