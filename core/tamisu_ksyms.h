#ifndef __TAMISU_KSYMS_H
#define __TAMISU_KSYMS_H

#include <linux/fs.h>
#include <linux/lsm_hooks.h>
#include <linux/mm_types.h>
#include <linux/pid.h>
#include <linux/task_work.h>
#include <linux/tracepoint.h>
#include <linux/types.h>
#include <linux/version.h>

#include "ss/avtab.h"
#include "ss/ebitmap.h"
#include "ss/policydb.h"
#include "ss/services.h"
#include "ss/sidtab.h"
#include "ss/symtab.h"
#include "objsec.h"
#include "security.h"

struct filename_trans_datum;
struct filename_trans_key;

#ifdef TAMISU_SELINUX_HAS_CRED_SECURITY_STRUCT
typedef struct cred_security_struct tamisu_selinux_cred_security_t;
#else
typedef struct task_security_struct tamisu_selinux_cred_security_t;
#endif // #ifdef TAMISU_SELINUX_HAS_CRED_SECURITY_STRUCT

#ifdef TAMISU_AVTAB_SPECIFIED_U16
typedef u16 tamisu_avtab_specified_t;
#else
typedef int tamisu_avtab_specified_t;
#endif // #ifdef TAMISU_AVTAB_SPECIFIED_U16

#ifdef TAMISU_EBITMAP_BIT_U32
typedef u32 tamisu_ebitmap_bit_t;
#else
typedef unsigned long tamisu_ebitmap_bit_t;
#endif // #ifdef TAMISU_EBITMAP_BIT_U32

#ifdef TAMISU_POLICYDB_RW_USES_POLICY_FILE
typedef struct policy_file tamisu_policydb_file_t;
#else
typedef void tamisu_policydb_file_t;
#endif // #ifdef TAMISU_POLICYDB_RW_USES_POLICY_FILE

struct mm_struct *tamisu_get_init_mm(void);
struct selinux_state *tamisu_get_selinux_state(void);
struct lsm_blob_sizes *tamisu_get_selinux_blob_sizes(void);
tamisu_selinux_cred_security_t *tamisu_selinux_cred(const struct cred *cred);
struct inode_security_struct *tamisu_selinux_inode(const struct inode *inode);
u32 tamisu_current_sid(void);

int tamisu_kallsyms_lookup_size_offset(unsigned long addr,
				       unsigned long *symbolsize,
				       unsigned long *offset);

int tamisu_task_work_add(struct task_struct *task, struct callback_head *work,
			 enum task_work_notify_mode mode);
void tamisu_change_pid(struct task_struct *task, enum pid_type type,
		       struct pid *pid);
#ifdef TAMISU_CHANGE_PID_HAS_PID_LINKS
void tamisu_change_pid_new(struct pid **pids, struct task_struct *task,
			   enum pid_type type, struct pid *pid);
#endif // #ifdef TAMISU_CHANGE_PID_HAS_PID_LINKS

int tamisu_tracepoint_probe_register(const char *name, void *probe, void *data);
int tamisu_tracepoint_probe_unregister(const char *name, void *probe,
				       void *data);

struct file *
tamisu_anon_inode_getfile_secure(const char *name,
				 const struct file_operations *fops, void *priv,
				 int flags, const struct inode *context_inode);

void *tamisu_symtab_search(struct symtab *s, const char *name);
int tamisu_symtab_insert(struct symtab *s, char *name, void *datum);
int tamisu_hashtab_insert(struct hashtab *h, void *key, void *datum,
			  struct hashtab_key_params key_params);
struct avtab_node *tamisu_avtab_search_node(struct avtab *h,
					    const struct avtab_key *key);
struct avtab_node *
tamisu_avtab_search_node_next(struct avtab_node *node,
			      tamisu_avtab_specified_t specified);
struct avtab_node *
tamisu_avtab_insert_nonunique(struct avtab *h, const struct avtab_key *key,
			      const struct avtab_datum *datum);
int tamisu_avtab_alloc(struct avtab *h, u32 nrules);
void tamisu_avtab_destroy(struct avtab *h);
int tamisu_ebitmap_get_bit(const struct ebitmap *e, tamisu_ebitmap_bit_t bit);
int tamisu_ebitmap_set_bit(struct ebitmap *e, tamisu_ebitmap_bit_t bit,
			   int value);
struct filename_trans_datum *
tamisu_policydb_filenametr_search(struct policydb *p,
				  struct filename_trans_key *key);
void tamisu_policydb_destroy(struct policydb *p);
int tamisu_policydb_write(struct policydb *p, tamisu_policydb_file_t *fp);
int tamisu_policydb_read(struct policydb *p, tamisu_policydb_file_t *fp);
int tamisu_policydb_load_isids(struct policydb *p, struct sidtab *s);
struct context *tamisu_sidtab_search(struct sidtab *s, u32 sid);

void tamisu_selinux_policyload_notify(u32 seqno);

#endif // #ifndef __TAMISU_KSYMS_H
