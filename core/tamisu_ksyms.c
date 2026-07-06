#include "tamisu_ksyms.h"

#include <linux/anon_inodes.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kallsyms.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/version.h>

#include "klog.h" // IWYU pragma: keep
#include "tamisu_symbol_resolver.h"

#define TAMISU_RESOLVE_FN(name)                                                \
	({                                                                     \
		static void *__addr;                                           \
		if (!__addr)                                                   \
			__addr = tamisu_lookup_symbol(#name);                  \
		__addr;                                                        \
	})

#define TAMISU_RESOLVE_DATA(name)                                              \
	({                                                                     \
		static void *__addr;                                           \
		if (!__addr)                                                   \
			__addr = tamisu_lookup_symbol(#name);                  \
		__addr;                                                        \
	})

struct mm_struct *tamisu_get_init_mm(void)
{
	return (struct mm_struct *)TAMISU_RESOLVE_DATA(init_mm);
}

struct selinux_state *tamisu_get_selinux_state(void)
{
	return (struct selinux_state *)TAMISU_RESOLVE_DATA(selinux_state);
}

struct lsm_blob_sizes *tamisu_get_selinux_blob_sizes(void)
{
	return (struct lsm_blob_sizes *)TAMISU_RESOLVE_DATA(selinux_blob_sizes);
}

tamisu_selinux_cred_security_t *tamisu_selinux_cred(const struct cred *cred)
{
	struct lsm_blob_sizes *blob = tamisu_get_selinux_blob_sizes();

	if (!cred || !cred->security || !blob)
		return NULL;
	return (tamisu_selinux_cred_security_t *)((char *)cred->security +
						  blob->lbs_cred);
}

struct inode_security_struct *tamisu_selinux_inode(const struct inode *inode)
{
	struct lsm_blob_sizes *blob = tamisu_get_selinux_blob_sizes();

	if (!inode || !inode->i_security || !blob)
		return NULL;
	return inode->i_security + blob->lbs_inode;
}

u32 tamisu_current_sid(void)
{
	const tamisu_selinux_cred_security_t *tsec =
	    tamisu_selinux_cred(current_cred());

	return tsec ? tsec->sid : 0;
}

int tamisu_kallsyms_lookup_size_offset(unsigned long addr,
				       unsigned long *symbolsize,
				       unsigned long *offset)
{
	typedef int (*fn_t)(unsigned long, unsigned long *, unsigned long *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(kallsyms_lookup_size_offset);

	if (!fn)
		return 0;
	return fn(addr, symbolsize, offset);
}

int tamisu_task_work_add(struct task_struct *task, struct callback_head *work,
			 enum task_work_notify_mode mode)
{
	typedef int (*fn_t)(struct task_struct *, struct callback_head *,
			    enum task_work_notify_mode);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(task_work_add);

	if (!fn)
		return -ENOENT;
	return fn(task, work, mode);
}

void tamisu_change_pid(struct task_struct *task, enum pid_type type,
		       struct pid *pid)
{
	typedef void (*fn_t)(struct task_struct *, enum pid_type, struct pid *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(change_pid);

	if (!fn) {
		pr_err("change_pid not found\n");
		return;
	}
	fn(task, type, pid);
}

#ifdef TAMISU_CHANGE_PID_HAS_PID_LINKS
void tamisu_change_pid_new(struct pid **pids, struct task_struct *task,
			   enum pid_type type, struct pid *pid)
{
	typedef void (*fn_t)(struct pid **, struct task_struct *, enum pid_type,
			     struct pid *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(change_pid);

	if (!fn) {
		pr_err("change_pid not found\n");
		return;
	}
	fn(pids, task, type, pid);
}
#endif // #ifdef TAMISU_CHANGE_PID_HAS_PID_LINKS

struct tamisu_tracepoint_find_ctx {
	const char *name;
	struct tracepoint *tp;
};

static void tamisu_tracepoint_find_cb(struct tracepoint *tp, void *priv)
{
	struct tamisu_tracepoint_find_ctx *ctx = priv;

	if (!tp || !tp->name || ctx->tp)
		return;
	if (!strcmp(tp->name, ctx->name))
		ctx->tp = tp;
}

static struct tracepoint *tamisu_find_tracepoint(const char *name)
{
	char sym[KSYM_NAME_LEN];
	struct tracepoint *tp;
	struct tamisu_tracepoint_find_ctx ctx = {.name = name};

	if (!name || !name[0])
		return NULL;

	{
		int len = snprintf(sym, sizeof(sym), "__tracepoint_%s", name);

		if (len <= 0 || len >= (int)sizeof(sym))
			return NULL;
		tp = tamisu_lookup_symbol(sym);
		if (tp)
			return tp;
	}

	for_each_kernel_tracepoint(tamisu_tracepoint_find_cb, &ctx);
	return ctx.tp;
}

int tamisu_tracepoint_probe_register(const char *name, void *probe, void *data)
{
	struct tracepoint *tp = tamisu_find_tracepoint(name);

	if (!tp) {
		pr_err("tracepoint %s not found\n", name ?: "(null)");
		return -ENOENT;
	}
	return tracepoint_probe_register(tp, probe, data);
}

int tamisu_tracepoint_probe_unregister(const char *name, void *probe,
				       void *data)
{
	struct tracepoint *tp = tamisu_find_tracepoint(name);

	if (!tp)
		return -ENOENT;
	return tracepoint_probe_unregister(tp, probe, data);
}

struct file *
tamisu_anon_inode_getfile_secure(const char *name,
				 const struct file_operations *fops, void *priv,
				 int flags, const struct inode *context_inode)
{
	typedef struct file *(*secure_fn_t)(const char *,
					    const struct file_operations *,
					    void *, int, const struct inode *);
	secure_fn_t secure_fn =
	    (secure_fn_t)TAMISU_RESOLVE_FN(anon_inode_getfile_secure);

	if (secure_fn)
		return secure_fn(name, fops, priv, flags, context_inode);

	return anon_inode_getfile(name, fops, priv, flags);
}

void *tamisu_symtab_search(struct symtab *s, const char *name)
{
	typedef void *(*fn_t)(struct symtab *, const char *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(symtab_search);

	if (!fn)
		return NULL;
	return fn(s, name);
}

int tamisu_symtab_insert(struct symtab *s, char *name, void *datum)
{
	typedef int (*fn_t)(struct symtab *, char *, void *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(symtab_insert);

	if (!fn)
		return -ENOENT;
	return fn(s, name, datum);
}

int tamisu_hashtab_insert(struct hashtab *h, void *key, void *datum,
			  struct hashtab_key_params key_params)
{
	typedef int (*insert_fn_t)(struct hashtab *, struct hashtab_node **,
				   void *, void *);
	insert_fn_t insert_fn =
	    (insert_fn_t)TAMISU_RESOLVE_FN(__hashtab_insert);
	u32 hvalue;
	struct hashtab_node *prev, *cur;

	if (!insert_fn)
		return -ENOENT;
	if (!h->size || h->nel == HASHTAB_MAX_NODES)
		return -EINVAL;

	hvalue = key_params.hash(key) & (h->size - 1);
	prev = NULL;
	cur = h->htable[hvalue];
	while (cur) {
		int cmp = key_params.cmp(key, cur->key);

		if (cmp == 0)
			return -EEXIST;
		if (cmp < 0)
			break;
		prev = cur;
		cur = cur->next;
	}

	return insert_fn(h, prev ? &prev->next : &h->htable[hvalue], key,
			 datum);
}

struct avtab_node *tamisu_avtab_search_node(struct avtab *h,
					    const struct avtab_key *key)
{
	typedef struct avtab_node *(*fn_t)(struct avtab *,
					   const struct avtab_key *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(avtab_search_node);

	if (!fn)
		return NULL;
	return fn(h, key);
}

struct avtab_node *
tamisu_avtab_search_node_next(struct avtab_node *node,
			      tamisu_avtab_specified_t specified)
{
	typedef struct avtab_node *(*fn_t)(struct avtab_node *,
					   tamisu_avtab_specified_t);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(avtab_search_node_next);

	if (!fn)
		return NULL;
	return fn(node, specified);
}

struct avtab_node *
tamisu_avtab_insert_nonunique(struct avtab *h, const struct avtab_key *key,
			      const struct avtab_datum *datum)
{
	typedef struct avtab_node *(*fn_t)(struct avtab *,
					   const struct avtab_key *,
					   const struct avtab_datum *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(avtab_insert_nonunique);

	if (!fn)
		return NULL;
	return fn(h, key, datum);
}

int tamisu_avtab_alloc(struct avtab *h, u32 nrules)
{
	typedef int (*fn_t)(struct avtab *, u32);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(avtab_alloc);

	if (!fn)
		return -ENOENT;
	return fn(h, nrules);
}

void tamisu_avtab_destroy(struct avtab *h)
{
	typedef void (*fn_t)(struct avtab *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(avtab_destroy);

	if (fn)
		fn(h);
}

int tamisu_ebitmap_get_bit(const struct ebitmap *e, tamisu_ebitmap_bit_t bit)
{
	typedef int (*fn_t)(const struct ebitmap *, tamisu_ebitmap_bit_t);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(ebitmap_get_bit);

	if (!fn)
		return 0;
	return fn(e, bit);
}

int tamisu_ebitmap_set_bit(struct ebitmap *e, tamisu_ebitmap_bit_t bit,
			   int value)
{
	typedef int (*fn_t)(struct ebitmap *, tamisu_ebitmap_bit_t, int);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(ebitmap_set_bit);

	if (!fn)
		return -ENOENT;
	return fn(e, bit, value);
}

struct filename_trans_datum *
tamisu_policydb_filenametr_search(struct policydb *p,
				  struct filename_trans_key *key)
{
	typedef struct filename_trans_datum *(*fn_t)(
	    struct policydb *, struct filename_trans_key *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(policydb_filenametr_search);

	if (!fn)
		return NULL;
	return fn(p, key);
}

void tamisu_policydb_destroy(struct policydb *p)
{
	typedef void (*fn_t)(struct policydb *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(policydb_destroy);

	if (fn)
		fn(p);
}

int tamisu_policydb_write(struct policydb *p, tamisu_policydb_file_t *fp)
{
	typedef int (*fn_t)(struct policydb *, tamisu_policydb_file_t *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(policydb_write);

	if (!fn)
		return -ENOENT;
	return fn(p, fp);
}

int tamisu_policydb_read(struct policydb *p, tamisu_policydb_file_t *fp)
{
	typedef int (*fn_t)(struct policydb *, tamisu_policydb_file_t *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(policydb_read);

	if (!fn)
		return -ENOENT;
	return fn(p, fp);
}

int tamisu_policydb_load_isids(struct policydb *p, struct sidtab *s)
{
	typedef int (*fn_t)(struct policydb *, struct sidtab *);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(policydb_load_isids);

	if (!fn)
		return -ENOENT;
	return fn(p, s);
}

struct context *tamisu_sidtab_search(struct sidtab *s, u32 sid)
{
	typedef struct sidtab_entry *(*fn_t)(struct sidtab *, u32);
	fn_t fn = (fn_t)TAMISU_RESOLVE_FN(sidtab_search_entry);
	struct sidtab_entry *entry;

	if (!fn)
		return NULL;
	entry = fn(s, sid);
	return entry ? &entry->context : NULL;
}

void tamisu_selinux_policyload_notify(u32 seqno)
{
#ifdef TAMISU_SELINUX_POLICYLOAD_NO_STATE
	typedef int (*avc_ss_reset_fn_t)(u32);
	typedef void (*status_update_fn_t)(u32);

	avc_ss_reset_fn_t avc_ss_reset_fn =
	    (avc_ss_reset_fn_t)TAMISU_RESOLVE_FN(avc_ss_reset);
	status_update_fn_t status_update_fn =
	    (status_update_fn_t)TAMISU_RESOLVE_FN(
		selinux_status_update_policyload);
#else
	typedef int (*avc_ss_reset_fn_t)(struct selinux_avc *, u32);
	typedef void (*status_update_fn_t)(struct selinux_state *, int);

	struct selinux_state *state = tamisu_get_selinux_state();
	avc_ss_reset_fn_t avc_ss_reset_fn =
	    (avc_ss_reset_fn_t)TAMISU_RESOLVE_FN(avc_ss_reset);
	status_update_fn_t status_update_fn =
	    (status_update_fn_t)TAMISU_RESOLVE_FN(
		selinux_status_update_policyload);
#endif // #ifdef TAMISU_SELINUX_POLICYLOAD_NO_STATE
	typedef void (*selnl_notify_fn_t)(u32);
	selnl_notify_fn_t selnl_notify_fn =
	    (selnl_notify_fn_t)TAMISU_RESOLVE_FN(selnl_notify_policyload);

#ifdef TAMISU_SELINUX_POLICYLOAD_NO_STATE
	if (avc_ss_reset_fn)
		avc_ss_reset_fn(seqno);
	if (status_update_fn)
		status_update_fn(seqno);
#else
	if (state && avc_ss_reset_fn)
		avc_ss_reset_fn(state->avc, seqno);
	if (state && status_update_fn)
		status_update_fn(state, seqno);
#endif // #ifdef TAMISU_SELINUX_POLICYLOAD_NO_STATE
	if (selnl_notify_fn)
		selnl_notify_fn(seqno);
}
