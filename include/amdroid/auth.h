#ifndef AMDROID_AUTH_H
#define AMDROID_AUTH_H
#include "amdroid/types.h"

/* Accounts and file permissions. Root (uid 0) may do anything. Other users
 * are checked against the owner and mode of each file (Unix style, 3 bits
 * for owner and for everyone else). The database files live in the disk root:
 * USERS.DB (accounts) and PERMS.DB (file owners and modes). */
void auth_init(void);                       /* loads the databases; first boot asks for a root password */
int  auth_login(const char *name, const char *pass);   /* 0 on success */
int  auth_user_count(void);                            /* accounts in the database */
const char *auth_user_at(int i, int *uid);             /* the i-th account's name and uid, or NULL */
void auth_logout(void);                       /* back to nobody */
int  auth_uid(void);                        /* current user (-1 when nobody is logged in) */
const char *auth_name(void);
int  auth_adduser(const char *name, const char *pass); /* root only */
int  auth_setpass(const char *name, const char *pass); /* self, or root for anyone */
int  auth_may(const char *path, int write);            /* permission to read (write=0) or write (write=1) */
int  auth_may_as(int uid, const char *path, int write);
void auth_claim_as(int uid, const char *path);
int  auth_chmod(const char *path, int mode);           /* owner or root */
void auth_claim(const char *path);                     /* a new file belongs to the current user */
void auth_read_secret(char *buf, int max);             /* reads a line with no echo */
void auth_read_line(char *buf, int max);               /* reads a line with echo */

#endif
