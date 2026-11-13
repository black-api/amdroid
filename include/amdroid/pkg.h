#ifndef AMDROID_PKG_H
#define AMDROID_PKG_H
/* The Am-Droid package manager. Installs ".adp" packages (built with
 * tools/adp-build.py) onto the FAT32 disk under /APPS/<ID>/, lists what is
 * installed, launches apps and removes them. An installed app is a Fuss
 * program that draws with the fb_* builtins and reads input with poll_key(). */
#include "amdroid/types.h"

#define PKG_MAX_APPS   32          /* most installed apps we enumerate */
#define PKG_ID_MAX     12
#define PKG_NAME_MAX   48

/* This build's platform version. A package's "requires" field is checked
 * against it (e.g. "amdroid1+", "amdroid.any", "amdroid2.5"). */
#define AMD_VER_MAJOR  1
#define AMD_VER_MINOR  0

typedef struct {
    char id[PKG_ID_MAX];           /* 8.3 dir name, e.g. "HELLO" */
    char name[PKG_NAME_MAX];       /* human name */
    char version[16];
    char author[32];
    char desc[96];
    char perms[64];                /* comma-separated, e.g. "framebuffer,storage" */
    char requires[16];             /* platform version, e.g. "amdroid1+" */
    char entry[16];                /* 8.3 program file, e.g. "MAIN.FAS" */
    int  icon;                     /* UI_ICO_* index to show */
} pkg_meta_t;

/* The result of inspecting a package before install: structural integrity,
 * platform-version compatibility and a security scan of its code. */
typedef struct {
    int  ok;                       /* 1 if the package is safe to install */
    int  blocking;                 /* 1 if install must be refused */
    char error[104];               /* structural/version problem, or "" */
    int  version_ok;
    int  dangerous;                /* requests or uses a dangerous capability */
    int  nfindings;                /* number of security findings */
    char findings[256];            /* newline-separated security findings */
} pkg_report_t;

/* Parse just the header of a .adp blob into *meta. 0 on success, -1 on a bad blob. */
int pkg_parse_header(const char *data, u32 len, pkg_meta_t *meta);

/* Inspect a package: structural validity, version compatibility and a security
 * scan. Fills *meta and *rep. Returns 0 if safe to install, -1 otherwise. */
int pkg_check(const char *data, u32 len, pkg_meta_t *meta, pkg_report_t *rep);

/* Install a .adp blob to /APPS/<ID>/. 0 on success; on failure returns -1 and
 * writes a reason into err (if err != NULL). Fills *out on success. */
int pkg_install(const char *data, u32 len, pkg_meta_t *out, char *err, u32 errcap);

/* Read a .adp file from the FAT32 disk (e.g. "HELLO.ADP") and install it. */
int pkg_install_file(const char *fat_path, pkg_meta_t *out, char *err, u32 errcap);

/* Download a .adp over HTTP from host:port/path and install it. host may be a
 * name (resolved by DNS) or a dotted IP. 0 on success. */
int pkg_install_url(const char *host, u16 port, const char *path, pkg_meta_t *out, char *err, u32 errcap);

/* Fill out[] with the installed apps (scans /APPS). Returns the count (<= max). */
int pkg_list(pkg_meta_t *out, int max);

/* Fill names[] with the .adp files on the disk root. Returns the count (<= max). */
int pkg_available(char names[][16], int max);

/* Build "APPS/<ID>/<ENTRY>" for launching. 0 on success. */
int pkg_entry_path(const char *id, char *out, u32 n);

/* 1 if an app with this id is installed. */
int pkg_installed(const char *id);

/* Remove an installed app and its files. 0 on success. */
int pkg_remove(const char *id);

#endif
