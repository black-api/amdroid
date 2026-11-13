#include "amdroid/vfs.h"
#include "amdroid/heap.h"
#include "amdroid/types.h"
#include "amdroid/console.h"
#include <string.h>

static vfs_node_t root;

static vfs_node_t *alloc_node(const char *name, int is_dir){
    vfs_node_t *n = kcalloc(1, sizeof(vfs_node_t));
    /* copy name */
    int i=0; while(name[i] && i<63){ n->name[i]=name[i]; i++; } n->name[i]=0;
    n->is_dir = is_dir;
    return n;
}

static vfs_node_t *find_child(vfs_node_t *dir, const char *name){
    vfs_node_t *c = dir->children;
    while(c){ if(strcmp(c->name,name)==0) return c; c=c->next; }
    return NULL;
}

vfs_node_t *vfs_lookup(const char *path){
    if(!path || path[0]=='\0') return &root;
    vfs_node_t *cur = &root;
    /* skip leading / */
    const char *p = path;
    char buf[64];
    while(*p){
        while(*p=='/') p++;
        if(!*p) break;
        int i=0;
        while(*p && *p!='/' && i<63) buf[i++]=*p++;
        buf[i]=0;
        if(!cur->is_dir) return NULL;
        vfs_node_t *c = find_child(cur, buf);
        if(!c) return NULL;
        cur = c;
    }
    return cur;
}

vfs_node_t *vfs_create(const char *path, int is_dir){
    /* find parent */
    char parentpath[256]; 
    /* split */
    int len=0; while(path[len]) len++;
    /* find last slash */
    int last=-1;
    for(int i=0;i<len;i++) if(path[i]=='/') last=i;
    vfs_node_t *parent;
    char name[64];
    if(last<0){ parent=&root; int i=0; while(path[i]&&i<63){name[i]=path[i];i++;} name[i]=0; }
    else {
        for(int i=0;i<last;i++) parentpath[i]=path[i];
        parentpath[last]=0;
        if(last==0) parent=&root;
        else { vfs_node_t *pp = vfs_lookup(parentpath); if(!pp) return NULL; parent=pp; }
        int i=0; int j=last+1;
        while(path[j] && i<63){ name[i++]=path[j++]; } name[i]=0;
    }
    if(!parent->is_dir) return NULL;
    if(find_child(parent,name)) return find_child(parent,name);
    vfs_node_t *n = alloc_node(name, is_dir);
    n->parent = parent;
    n->next = parent->children;
    parent->children = n;
    return n;
}

int vfs_read(vfs_node_t *n, u64 off, u64 len, void *buf){
    if(!n || n->is_dir) return -1;
    if(off >= n->size) return 0;
    u64 avail = n->size - off;
    if(len > avail) len = avail;
    for(u64 i=0;i<len;i++) ((char*)buf)[i] = n->data[off+i];
    return (int)len;
}

int vfs_write(vfs_node_t *n, u64 off, const void *buf, u64 len){
    if(!n || n->is_dir) return -1;
    u64 end = off + len;
    if(end > n->size){
        u8 *nd = kmalloc(end);
        if(n->data){ for(u64 i=0;i<n->size;i++) nd[i]=n->data[i]; }
        for(u64 i=n->size;i<end;i++) nd[i]=0;
        /* free old? skip */
        n->data = nd;
        n->size = end;
    }
    for(u64 i=0;i<len;i++) n->data[off+i] = ((const char*)buf)[i];
    return (int)len;
}

void vfs_list(vfs_node_t *dir){
    if(!dir || !dir->is_dir) return;
    vfs_node_t *c = dir->children;
    while(c){
        console_printf("%s%s  ", c->name, c->is_dir?"/":"");
        c = c->next;
    }
    console_puts("\n");
}

void vfs_init(void){
    /* root */
    root.name[0]='/'; root.name[1]=0;
    root.is_dir = 1;
    /* create some default files */
    vfs_create("/etc",1);
    vfs_node_t *f = vfs_create("/etc/motd",0);
    const char *msg = "Welcome to am-droid OS!\nA lightweight operating system.\n";
    vfs_write(f,0,msg,strlen(msg));
    vfs_create("/bin",1);
    vfs_create("/tmp",1);
    vfs_create("/home",1);
    vfs_create("/disk",1);
}
