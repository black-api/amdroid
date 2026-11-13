/* ext2.c — read-only ext2 file system on the secondary IDE disk.
 * Supports direct, single and double indirect blocks, and linear directories. */
#include "amdroid/ext2.h"
#include "amdroid/ata.h"
#include "amdroid/heap.h"
#include "amdroid/console.h"
#include "amdroid/types.h"
#include <string.h>

#define CHAN 1                    /* secondary IDE channel */
#define SLAVE 1                   /* the slave drive on it (the CD-ROM is the master) */
#define EXT2_MAGIC 0xEF53
#define S_IFDIR 0x4000
#define S_IFREG 0x8000

static int mounted;
static u32 block_size, inode_size, inodes_per_group, blocks_per_group, first_data, inode_count;
static u32 gdt_block;

static u32 rd32(const u8 *p){ return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p){ return (u16)(p[0] | (p[1] << 8)); }

static int read_block(u32 blk, void *buf){
    u32 sectors = block_size / 512;
    return ata_dev_read(CHAN, SLAVE, blk * sectors, (u8)sectors, buf) == 0 ? 0 : -1;
}

int ext2_mounted(void){ return mounted; }

int ext2_mount(void){
    mounted = 0;
    if(!ata_dev_present(CHAN, SLAVE)){ console_puts("[ext2] no secondary IDE disk\n"); return -1; }
    u8 sb[1024];
    if(ata_dev_read(CHAN, SLAVE, 2, 2, sb) != 0){ console_puts("[ext2] cannot read the disk\n"); return -1; }
    if(rd16(sb + 56) != EXT2_MAGIC){ console_puts("[ext2] secondary disk has no ext2 file system\n"); return -1; }
    u32 log_bs = rd32(sb + 24);
    if(log_bs > 2) return -1;
    block_size = 1024u << log_bs;
    inode_count = rd32(sb + 0);
    first_data = rd32(sb + 20);
    blocks_per_group = rd32(sb + 32);
    inodes_per_group = rd32(sb + 40);
    u32 rev = rd32(sb + 76);
    inode_size = rev == 0 ? 128 : rd16(sb + 88);
    if(!inodes_per_group || !blocks_per_group || inode_size < 128) return -1;
    gdt_block = first_data + 1;
    mounted = 1;
    return 0;
}

/* Reads inode number ino (1-based) into out (128 bytes is enough). */
static int read_inode(u32 ino, u8 *out){
    if(!mounted || ino == 0 || ino > inode_count) return -1;
    u32 group = (ino - 1) / inodes_per_group;
    u32 index = (ino - 1) % inodes_per_group;
    u8 *blk = kmalloc(block_size);
    if(!blk) return -1;
    /* group descriptor: 32 bytes; inode table block number at offset 8 */
    u32 gd_per_block = block_size / 32;
    if(read_block(gdt_block + group / gd_per_block, blk) != 0){ kfree(blk); return -1; }
    u32 itable = rd32(blk + (group % gd_per_block) * 32 + 8);
    u32 off = index * inode_size;
    if(read_block(itable + off / block_size, blk) != 0){ kfree(blk); return -1; }
    memcpy(out, blk + off % block_size, 128);
    kfree(blk);
    return 0;
}

/* Calls fn(blockno, index) for each data block of the inode, in order.
 * Returns the number of blocks visited, or -1. */
static int inode_blocks(const u8 *in, u32 *list, u32 max_list){
    u32 n = 0;
    u32 per = block_size / 4;
    for(int i = 0; i < 12 && n < max_list; i++){
        u32 b = rd32(in + 40 + i * 4);
        if(b) list[n++] = b;
    }
    /* indirect blocks: read the pointer tables, then their entries */
    u8 *ind = kmalloc(block_size);
    if(!ind) return -1;
    u32 single = rd32(in + 40 + 12 * 4);
    if(single && n < max_list){
        if(read_block(single, ind) == 0)
            for(u32 i = 0; i < per && n < max_list; i++){
                u32 b = rd32(ind + i * 4);
                if(b) list[n++] = b;
            }
    }
    u32 dbl = rd32(in + 40 + 13 * 4);
    if(dbl && n < max_list){
        u8 *outer = kmalloc(block_size);
        if(outer && read_block(dbl, outer) == 0){
            for(u32 i = 0; i < per && n < max_list; i++){
                u32 mid = rd32(outer + i * 4);
                if(!mid) continue;
                if(read_block(mid, ind) != 0) break;
                for(u32 j = 0; j < per && n < max_list; j++){
                    u32 b = rd32(ind + j * 4);
                    if(b) list[n++] = b;
                }
            }
        }
        if(outer) kfree(outer);
    }
    kfree(ind);
    return (int)n;
}

/* Walks the directory `dir` looking for name. Fills out_ino. Returns 1 if found. */
static int dir_find(u32 dir, const char *name, size_t nlen, u32 *out_ino){
    u8 in[128];
    if(read_inode(dir, in) != 0) return -1;
    u32 *blocks = kmalloc(sizeof(u32) * 1024);
    if(!blocks) return -1;
    int nb = inode_blocks(in, blocks, 1024);
    u8 *buf = kmalloc(block_size);
    int found = 0;
    for(int b = 0; b < nb && buf && !found; b++){
        if(read_block(blocks[b], buf) != 0) break;
        u32 off = 0;
        while(off + 8 <= block_size){
            u32 ino = rd32(buf + off);
            u16 rec = rd16(buf + off + 4);
            u8 nl = buf[off + 6];
            if(rec < 8) break;
            if(ino && nl == nlen && !memcmp(buf + off + 8, name, nlen)){
                *out_ino = ino; found = 1; break;
            }
            off += rec;
        }
    }
    if(buf) kfree(buf);
    kfree(blocks);
    return found;
}

/* Resolves "a/b/c" from the root. Returns the inode, or 0. */
static u32 resolve(const char *path){
    u32 ino = 2;                               /* the root directory */
    const char *p = path;
    while(*p == '/') p++;
    while(*p){
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if(len == 0){ p = slash + 1; continue; }
        u32 next;
        if(dir_find(ino, p, len, &next) != 1) return 0;
        ino = next;
        p = slash ? slash + 1 : p + len;
    }
    return ino;
}

int ext2_list(const char *path, ext2_list_fn cb){
    if(!mounted) return -1;
    u32 dir = resolve(path);
    if(!dir) return -1;
    u8 in[128];
    if(read_inode(dir, in) != 0) return -1;
    if((rd16(in) & 0xF000) != S_IFDIR) return -1;
    u32 *blocks = kmalloc(sizeof(u32) * 1024);
    if(!blocks) return -1;
    int nb = inode_blocks(in, blocks, 1024);
    u8 *buf = kmalloc(block_size);
    for(int b = 0; b < nb && buf; b++){
        if(read_block(blocks[b], buf) != 0) break;
        u32 off = 0;
        while(off + 8 <= block_size){
            u32 ino = rd32(buf + off);
            u16 rec = rd16(buf + off + 4);
            u8 nl = buf[off + 6];
            if(rec < 8) break;
            if(ino && nl){
                char name[256];
                memcpy(name, buf + off + 8, nl);
                name[nl] = 0;
                u8 child[128];
                int is_dir = 0; u32 size = 0;
                if(read_inode(ino, child) == 0){
                    is_dir = (rd16(child) & 0xF000) == S_IFDIR;
                    size = rd32(child + 4);
                }
                if(strcmp(name, ".") && strcmp(name, "..")) cb(name, is_dir, size);
            }
            off += rec;
        }
    }
    if(buf) kfree(buf);
    kfree(blocks);
    return 0;
}

int ext2_read(const char *path, void *buf, u32 max, u32 *size){
    if(!mounted) return -1;
    u32 ino = resolve(path);
    if(!ino) return -1;
    u8 in[128];
    if(read_inode(ino, in) != 0) return -1;
    if((rd16(in) & 0xF000) != S_IFREG) return -1;
    u32 fsize = rd32(in + 4);
    if(size) *size = fsize;
    u32 want = fsize < max ? fsize : max;
    u32 *blocks = kmalloc(sizeof(u32) * 4096);
    if(!blocks) return -1;
    int nb = inode_blocks(in, blocks, 4096);
    u8 *tmp = kmalloc(block_size);
    u32 done = 0;
    for(int b = 0; b < nb && done < want && tmp; b++){
        if(read_block(blocks[b], tmp) != 0) break;
        u32 chunk = block_size < want - done ? block_size : want - done;
        memcpy((u8*)buf + done, tmp, chunk);
        done += chunk;
    }
    if(tmp) kfree(tmp);
    kfree(blocks);
    return done == want ? 0 : -1;
}
