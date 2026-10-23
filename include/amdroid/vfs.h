#ifndef AMDROID_VFS_H
#define AMDROID_VFS_H
#include "amdroid/types.h"

typedef struct vfs_node vfs_node_t;
typedef struct vfs_dirent vfs_dirent_t;

struct vfs_node {
    char name[64];
    int is_dir;
    u64 size;
    u8 *data;            /* for files */
    vfs_node_t *parent;
    vfs_node_t *children; /* linked list */
    vfs_node_t *next;
};

vfs_node_t *vfs_root(void);
vfs_node_t *vfs_lookup(const char *path);
int vfs_read(vfs_node_t *n, u64 off, u64 len, void *buf);
vfs_node_t *vfs_create(const char *path, int is_dir);
int vfs_write(vfs_node_t *n, u64 off, const void *buf, u64 len);
void vfs_list(vfs_node_t *dir);

void vfs_init(void);

#endif
