#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "klog.h" // IWYU pragma: keep
#include "linux/lsm_audit.h" // IWYU pragma: keep
#include "objsec.h"
#include "tamisu_selinux.h"
#include "tamisu_sepolicy.h"
#include "ss/context.h"
#include "ss/services.h"
#include "security.h"
#include "uapi/selinux.h"
#include "xfrm.h"

#define SELINUX_POLICY_INSTEAD_SELINUX_SS

#define ALL NULL

struct selinux_policy *backup_sepolicy;

static struct policydb *get_policydb(void)
{
	struct policydb *db;
	struct selinux_policy *policy = selinux_state.policy;
	db = &policy->policydb;
	return db;
}

static DEFINE_MUTEX(tamisu_rules);

static void reset_avc_cache(void);

static void backup_original_sepolicy_once(void)
{
	int ret;

	if (backup_sepolicy)
		return;

	backup_sepolicy = tamisu_dup_sepolicy(selinux_state.policy);
	if (IS_ERR(backup_sepolicy)) {
		pr_err("failed to create backup sepolicy: %ld\n",
		       PTR_ERR(backup_sepolicy));
		backup_sepolicy = NULL;
		return;
	}

	backup_sepolicy->sidtab =
	    kzalloc(sizeof(*backup_sepolicy->sidtab), GFP_KERNEL);
	if (!backup_sepolicy->sidtab) {
		pr_err("failed to alloc backup sidtab\n");
		tamisu_destroy_sepolicy(backup_sepolicy);
		backup_sepolicy = NULL;
		return;
	}

	ret = policydb_load_isids(&backup_sepolicy->policydb,
				  backup_sepolicy->sidtab);
	if (ret) {
		pr_err("failed to load isids for backup sepolicy: %d\n", ret);
		kfree(backup_sepolicy->sidtab);
		tamisu_destroy_sepolicy(backup_sepolicy);
		backup_sepolicy = NULL;
		return;
	}

	pr_info("backup sepolicy success\n");
}

static const char *tamisu_file_load_perms[] = {
    "read", "open", "getattr", "map", "execute",
};

static const char *tamisu_tmpfs_hook_perms[] = {
    "read", "write", "open", "getattr", "map", "execute",
};

static u32 tamisu_file_perm_mask(struct class_datum *cls, const char *perm_name)
{
	struct perm_datum *perm;

	if (!cls || !perm_name)
		return 0;

	perm = symtab_search(&cls->permissions, perm_name);
	if (!perm && cls->comdatum)
		perm = symtab_search(&cls->comdatum->permissions, perm_name);
	if (!perm || perm->value == 0 || perm->value > 32)
		return 0;
	return 1U << (perm->value - 1);
}

static u32 tamisu_required_av(struct class_datum *cls, const char *const *perms,
			      int count)
{
	u32 av = 0;
	int i;

	for (i = 0; i < count; i++)
		av |= tamisu_file_perm_mask(cls, perms[i]);
	return av;
}

static u32 tamisu_direct_allowed_av(struct policydb *db, u32 src_type,
				    u32 tgt_type, u16 target_class)
{
	struct avtab_key key = {};
	struct avtab_node *node;

	key.source_type = src_type;
	key.target_type = tgt_type;
	key.target_class = target_class;
	key.specified = AVTAB_ALLOWED;
	node = avtab_search_node(&db->te_avtab, &key);
	return node ? node->datum.u.data : 0;
}

static const char *tamisu_type_name_by_value(struct policydb *db, u32 type)
{
	if (!db || type == 0 || type > db->p_types.nprim)
		return NULL;
	if (!db->sym_val_to_name[SYM_TYPES])
		return NULL;
	return db->sym_val_to_name[SYM_TYPES][type - 1];
}

static u32 tamisu_type_value_by_name(struct policydb *db, const char *name)
{
	struct type_datum *type;

	if (!db || !name)
		return 0;
	type = symtab_search(&db->p_types, name);
	return type ? type->value : 0;
}

static bool tamisu_apply_file_av(struct policydb *db, const char *src,
				 const char *tgt, u32 av, bool allow,
				 const char *const *perms, int count)
{
	struct class_datum *cls;
	bool ok = true;
	int i;

	cls = symtab_search(&db->p_classes, "file");
	if (!cls)
		return false;

	for (i = 0; i < count; i++) {
		u32 mask = tamisu_file_perm_mask(cls, perms[i]);

		if (!(av & mask))
			continue;
		if (allow)
			ok = tamisu_allow(db, src, tgt, "file", perms[i]) && ok;
		else
			ok = tamisu_deny(db, src, tgt, "file", perms[i]) && ok;
	}
	return ok;
}

int tamisu_file_load_policy_allow_current(struct file *file,
					  struct tamisu_file_load_policy *state)
{
	struct selinux_policy *pol, *old_pol;
	struct policydb *db;
	struct inode_security_struct *isec;
	struct context *scontext;
	struct context *tcontext;
	struct class_datum *cls;
	const char *src_name;
	const char *tgt_name;
	u32 ssid;
	u32 tsid;
	u32 required_av;
	u32 direct_av;
	u32 add_av;
	u32 tmpfs_type;
	u32 tmpfs_required_av;
	u32 tmpfs_direct_av;
	u32 tmpfs_add_av = 0;
	int ret = 0;

	if (!file || !state)
		return -EINVAL;
	memset(state, 0, sizeof(*state));

	isec = selinux_inode(file_inode(file));
	if (!isec)
		return -EINVAL;
	ssid = current_sid();
	tsid = isec->sid;
	if (!ssid || !tsid)
		return -EINVAL;

	mutex_lock(&selinux_state.policy_mutex);

	old_pol = selinux_state.policy;
	db = &old_pol->policydb;
	scontext = sidtab_search(old_pol->sidtab, ssid);
	tcontext = sidtab_search(old_pol->sidtab, tsid);
	if (!scontext || !tcontext) {
		ret = -ENOENT;
		goto out_unlock;
	}
	cls = symtab_search(&db->p_classes, "file");
	if (!cls) {
		ret = -ENOENT;
		goto out_unlock;
	}
	src_name = tamisu_type_name_by_value(db, scontext->type);
	tgt_name = tamisu_type_name_by_value(db, tcontext->type);
	if (!src_name || !tgt_name) {
		ret = -ENOENT;
		goto out_unlock;
	}

	required_av = tamisu_required_av(cls, tamisu_file_load_perms,
					 ARRAY_SIZE(tamisu_file_load_perms));
	direct_av = tamisu_direct_allowed_av(db, scontext->type, tcontext->type,
					     cls->value);
	add_av = required_av & ~direct_av;

	tmpfs_type = tamisu_type_value_by_name(db, "tmpfs");
	if (tmpfs_type) {
		tmpfs_required_av =
		    tamisu_required_av(cls, tamisu_tmpfs_hook_perms,
				       ARRAY_SIZE(tamisu_tmpfs_hook_perms));
		tmpfs_direct_av = tamisu_direct_allowed_av(
		    db, scontext->type, tmpfs_type, cls->value);
		tmpfs_add_av = tmpfs_required_av & ~tmpfs_direct_av;
	}

	if (!add_av && !tmpfs_add_av)
		goto out_unlock;

	pol = tamisu_dup_sepolicy(rcu_dereference_protected(
	    old_pol, lockdep_is_held(&selinux_state.policy_mutex)));
	if (IS_ERR(pol)) {
		ret = PTR_ERR(pol);
		pr_err("file_load_policy: dup failed: %d\n", ret);
		goto out_unlock;
	}
	db = &pol->policydb;
	if (add_av &&
	    !tamisu_apply_file_av(db, src_name, tgt_name, add_av, true,
				  tamisu_file_load_perms,
				  ARRAY_SIZE(tamisu_file_load_perms))) {
		tamisu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}
	if (tmpfs_add_av &&
	    !tamisu_apply_file_av(db, src_name, "tmpfs", tmpfs_add_av, true,
				  tamisu_tmpfs_hook_perms,
				  ARRAY_SIZE(tamisu_tmpfs_hook_perms))) {
		tamisu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}

	state->src_type = scontext->type;
	state->tgt_type = tcontext->type;
	state->tmpfs_type = tmpfs_type;
	state->target_class = cls->value;
	state->added_av = add_av;
	state->tmpfs_added_av = tmpfs_add_av;

	pr_info("file_load_policy: allow src=%s tgt=%s file added=0x%x "
		"tmpfs=0x%x\n",
		src_name, tgt_name, add_av, tmpfs_add_av);
	rcu_assign_pointer(selinux_state.policy, pol);
	synchronize_rcu();
	tamisu_destroy_sepolicy(old_pol);
	reset_avc_cache();

out_unlock:
	mutex_unlock(&selinux_state.policy_mutex);
	return ret;
}

int tamisu_file_load_policy_restore(const struct tamisu_file_load_policy *state)
{
	struct selinux_policy *pol, *old_pol;
	struct policydb *db;
	const char *src_name;
	const char *tgt_name = NULL;
	const char *tmpfs_name = NULL;
	int ret = 0;

	if (!state || (!state->added_av && !state->tmpfs_added_av))
		return 0;

	mutex_lock(&selinux_state.policy_mutex);

	old_pol = selinux_state.policy;
	db = &old_pol->policydb;
	src_name = tamisu_type_name_by_value(db, state->src_type);
	if (state->added_av)
		tgt_name = tamisu_type_name_by_value(db, state->tgt_type);
	if (state->tmpfs_added_av)
		tmpfs_name = tamisu_type_name_by_value(db, state->tmpfs_type);
	if (!src_name || (state->added_av && !tgt_name) ||
	    (state->tmpfs_added_av && !tmpfs_name)) {
		ret = -ENOENT;
		goto out_unlock;
	}

	pol = tamisu_dup_sepolicy(rcu_dereference_protected(
	    old_pol, lockdep_is_held(&selinux_state.policy_mutex)));
	if (IS_ERR(pol)) {
		ret = PTR_ERR(pol);
		pr_err("file_load_policy: restore dup failed: %d\n", ret);
		goto out_unlock;
	}
	db = &pol->policydb;
	if (state->added_av &&
	    !tamisu_apply_file_av(db, src_name, tgt_name, state->added_av,
				  false, tamisu_file_load_perms,
				  ARRAY_SIZE(tamisu_file_load_perms))) {
		tamisu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}
	if (state->tmpfs_added_av &&
	    !tamisu_apply_file_av(
		db, src_name, tmpfs_name, state->tmpfs_added_av, false,
		tamisu_tmpfs_hook_perms, ARRAY_SIZE(tamisu_tmpfs_hook_perms))) {
		tamisu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}

	pr_info("file_load_policy: restore src=%s tgt=%s file cleared=0x%x "
		"tmpfs=0x%x\n",
		src_name, tgt_name ? tgt_name : "-", state->added_av,
		state->tmpfs_added_av);
	rcu_assign_pointer(selinux_state.policy, pol);
	synchronize_rcu();
	tamisu_destroy_sepolicy(old_pol);
	reset_avc_cache();

out_unlock:
	mutex_unlock(&selinux_state.policy_mutex);
	return ret;
}

void apply_tamisu_rules(void)
{
	struct policydb *db;

	if (!getenforce()) {
		pr_info("SELinux permissive or disabled, apply rules!\n");
	}

	mutex_lock(&tamisu_rules);

	backup_original_sepolicy_once();

	db = get_policydb();

	tamisu_permissive(db, TAMISU_DOMAIN);
	tamisu_typeattribute(db, TAMISU_DOMAIN, "mlstrustedsubject");
	tamisu_typeattribute(db, TAMISU_DOMAIN, "netdomain");
	tamisu_typeattribute(db, TAMISU_DOMAIN, "bluetoothdomain");

	// Create unconstrained file type
	tamisu_type(db, KERNEL_SU_FILE, "file_type");
	tamisu_typeattribute(db, KERNEL_SU_FILE, "mlstrustedobject");
	tamisu_allow(db, ALL, KERNEL_SU_FILE, ALL, ALL);

	// allow all!
	tamisu_allow(db, TAMISU_DOMAIN, ALL, ALL, ALL);

	// allow us do any ioctl
	if (db->policyvers >= POLICYDB_VERSION_XPERMS_IOCTL) {
		tamisu_allowxperm(db, TAMISU_DOMAIN, ALL, "blk_file", ALL);
		tamisu_allowxperm(db, TAMISU_DOMAIN, ALL, "fifo_file", ALL);
		tamisu_allowxperm(db, TAMISU_DOMAIN, ALL, "chr_file", ALL);
		tamisu_allowxperm(db, TAMISU_DOMAIN, ALL, "file", ALL);
	}

	// our tamisu_daemon triggered by init
	tamisu_allow(db, "init", TAMISU_DOMAIN, ALL, ALL);

	// copied from Magisk rules
	// suRights
	tamisu_allow(db, "servicemanager", TAMISU_DOMAIN, "dir", "search");
	tamisu_allow(db, "servicemanager", TAMISU_DOMAIN, "dir", "read");
	tamisu_allow(db, "servicemanager", TAMISU_DOMAIN, "file", "open");
	tamisu_allow(db, "servicemanager", TAMISU_DOMAIN, "file", "read");
	tamisu_allow(db, "servicemanager", TAMISU_DOMAIN, "process", "getattr");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "process", "sigchld");

	// allowLog
	tamisu_allow(db, "logd", TAMISU_DOMAIN, "dir", "search");
	tamisu_allow(db, "logd", TAMISU_DOMAIN, "file", "read");
	tamisu_allow(db, "logd", TAMISU_DOMAIN, "file", "open");
	tamisu_allow(db, "logd", TAMISU_DOMAIN, "file", "getattr");

	// dumpsys
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "fd", "use");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "fifo_file", "write");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "fifo_file", "read");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "fifo_file", "open");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "fifo_file", "getattr");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "unix_stream_socket", "read");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "unix_stream_socket", "write");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "unix_stream_socket", "connectto");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "unix_stream_socket", "getopt");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "unix_stream_socket", "getattr");

	// use memfd created by su domain
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "memfd_file", "execute");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "memfd_file", "getattr");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "memfd_file", "map");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "memfd_file", "read");
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "memfd_file", "write");

	// bootctl
	tamisu_allow(db, "hwservicemanager", TAMISU_DOMAIN, "dir", "search");
	tamisu_allow(db, "hwservicemanager", TAMISU_DOMAIN, "file", "read");
	tamisu_allow(db, "hwservicemanager", TAMISU_DOMAIN, "file", "open");
	tamisu_allow(db, "hwservicemanager", TAMISU_DOMAIN, "process",
		     "getattr");

	// Allow all binder transactions
	tamisu_allow(db, ALL, TAMISU_DOMAIN, "binder", ALL);

	// Allow system server kill su process
	tamisu_allow(db, "system_server", TAMISU_DOMAIN, "process", "getpgid");
	tamisu_allow(db, "system_server", TAMISU_DOMAIN, "process", "sigkill");

	// Tamisu Zygisk system_server trampolines.
	tamisu_allow(db, "system_server", "system_server", "process",
		     "execmem");
	// https://android-review.googlesource.com/c/platform/system/logging/+/3725346
	tamisu_dontaudit(db, "untrusted_app", TAMISU_DOMAIN, "dir", "getattr");
	mutex_unlock(&tamisu_rules);
}

#define TAMISU_SEPOLICY_MAX_BATCH_SIZE (8U * 1024U * 1024U)
#define TAMISU_SEPOLICY_MAX_ARGS 5

struct sepol_data {
	u32 cmd;
	u32 subcmd;
};

struct sepol_batch_cursor {
	const u8 *cur;
	const u8 *end;
};

static size_t sepol_remaining(const struct sepol_batch_cursor *cursor)
{
	return (size_t)(cursor->end - cursor->cur);
}

static int sepol_read_cmd_header(struct sepol_batch_cursor *cursor,
				 struct sepol_data *header)
{
	if (sepol_remaining(cursor) < sizeof(*header))
		return -EINVAL;

	memcpy(header, cursor->cur, sizeof(*header));
	cursor->cur += sizeof(*header);

	return 0;
}

static int sepol_read_string(struct sepol_batch_cursor *cursor,
			     const char **out)
{
	u32 len;
	const char *str;

	if (sepol_remaining(cursor) < sizeof(len))
		return -EINVAL;

	memcpy(&len, cursor->cur, sizeof(len));
	cursor->cur += sizeof(len);

	if (len >= sepol_remaining(cursor))
		return -EINVAL;

	str = (const char *)cursor->cur;
	if (memchr(str, '\0', len) != NULL || str[len] != '\0')
		return -EINVAL;

	cursor->cur += len + 1;
	if (len == 0) {
		*out = ALL;
		return 0;
	}

	*out = str;
	return 0;
}

static int sepol_require_not_all(const char *value, const char *name)
{
	if (value != ALL)
		return 0;

	pr_err("sepol: %s cannot be ALL.\n", name);
	return -EINVAL;
}

static int sepol_expected_argc(u32 cmd)
{
	switch (cmd) {
	case TAMISU_SEPOLICY_CMD_NORMAL_PERM:
		return 4;
	case TAMISU_SEPOLICY_CMD_XPERM:
		return 5;
	case TAMISU_SEPOLICY_CMD_TYPE_STATE:
		return 1;
	case TAMISU_SEPOLICY_CMD_TYPE:
	case TAMISU_SEPOLICY_CMD_TYPE_ATTR:
		return 2;
	case TAMISU_SEPOLICY_CMD_ATTR:
		return 1;
	case TAMISU_SEPOLICY_CMD_TYPE_TRANSITION:
		return 5;
	case TAMISU_SEPOLICY_CMD_TYPE_CHANGE:
		return 4;
	case TAMISU_SEPOLICY_CMD_GENFSCON:
		return 3;
	default:
		return -EINVAL;
	}
}

static int apply_one_sepolicy_cmd(struct policydb *db,
				  const struct sepol_data *header,
				  const char **args)
{
	bool success = false;
	int ret;

	switch (header->cmd) {
	case TAMISU_SEPOLICY_CMD_NORMAL_PERM:
		if (header->subcmd == TAMISU_SEPOLICY_SUBCMD_NORMAL_PERM_ALLOW)
			success = tamisu_allow(db, args[0], args[1], args[2],
					       args[3]);
		else if (header->subcmd ==
			 TAMISU_SEPOLICY_SUBCMD_NORMAL_PERM_DENY)
			success =
			    tamisu_deny(db, args[0], args[1], args[2], args[3]);
		else if (header->subcmd ==
			 TAMISU_SEPOLICY_SUBCMD_NORMAL_PERM_AUDITALLOW)
			success = tamisu_auditallow(db, args[0], args[1],
						    args[2], args[3]);
		else if (header->subcmd ==
			 TAMISU_SEPOLICY_SUBCMD_NORMAL_PERM_DONTAUDIT)
			success = tamisu_dontaudit(db, args[0], args[1],
						   args[2], args[3]);
		else
			pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
		return success ? 0 : -EINVAL;

	case TAMISU_SEPOLICY_CMD_XPERM:
		ret = sepol_require_not_all(args[3], "operation");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[4], "perm_set");
		if (ret < 0)
			return ret;

		if (header->subcmd == TAMISU_SEPOLICY_SUBCMD_XPERM_ALLOW)
			success = tamisu_allowxperm(db, args[0], args[1],
						    args[2], args[4]);
		else if (header->subcmd ==
			 TAMISU_SEPOLICY_SUBCMD_XPERM_AUDITALLOW)
			success = tamisu_auditallowxperm(db, args[0], args[1],
							 args[2], args[4]);
		else if (header->subcmd ==
			 TAMISU_SEPOLICY_SUBCMD_XPERM_DONTAUDIT)
			success = tamisu_dontauditxperm(db, args[0], args[1],
							args[2], args[4]);
		else
			pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
		return success ? 0 : -EINVAL;

	case TAMISU_SEPOLICY_CMD_TYPE_STATE:
		ret = sepol_require_not_all(args[0], "type");
		if (ret < 0)
			return ret;

		if (header->subcmd ==
		    TAMISU_SEPOLICY_SUBCMD_TYPE_STATE_PERMISSIVE)
			success = tamisu_permissive(db, args[0]);
		else if (header->subcmd ==
			 TAMISU_SEPOLICY_SUBCMD_TYPE_STATE_ENFORCE)
			success = tamisu_enforce(db, args[0]);
		else
			pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
		return success ? 0 : -EINVAL;

	case TAMISU_SEPOLICY_CMD_TYPE:
	case TAMISU_SEPOLICY_CMD_TYPE_ATTR:
		ret = sepol_require_not_all(args[0], "type");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[1], "attribute");
		if (ret < 0)
			return ret;

		if (header->cmd == TAMISU_SEPOLICY_CMD_TYPE)
			success = tamisu_type(db, args[0], args[1]);
		else
			success = tamisu_typeattribute(db, args[0], args[1]);
		if (!success) {
			pr_err("sepol: %d failed.\n", header->cmd);
			return -EINVAL;
		}
		return 0;

	case TAMISU_SEPOLICY_CMD_ATTR:
		ret = sepol_require_not_all(args[0], "attribute");
		if (ret < 0)
			return ret;

		if (!tamisu_attribute(db, args[0])) {
			pr_err("sepol: %d failed.\n", header->cmd);
			return -EINVAL;
		}
		return 0;

	case TAMISU_SEPOLICY_CMD_TYPE_TRANSITION:
		ret = sepol_require_not_all(args[0], "src");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[1], "tgt");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[2], "cls");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[3], "default_type");
		if (ret < 0)
			return ret;

		success = tamisu_type_transition(db, args[0], args[1], args[2],
						 args[3], args[4]);
		return success ? 0 : -EINVAL;

	case TAMISU_SEPOLICY_CMD_TYPE_CHANGE:
		ret = sepol_require_not_all(args[0], "src");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[1], "tgt");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[2], "cls");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[3], "default_type");
		if (ret < 0)
			return ret;

		if (header->subcmd == TAMISU_SEPOLICY_SUBCMD_TYPE_CHANGE_CHANGE)
			success = tamisu_type_change(db, args[0], args[1],
						     args[2], args[3]);
		else if (header->subcmd ==
			 TAMISU_SEPOLICY_SUBCMD_TYPE_CHANGE_MEMBER)
			success = tamisu_type_member(db, args[0], args[1],
						     args[2], args[3]);
		else
			pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
		return success ? 0 : -EINVAL;

	case TAMISU_SEPOLICY_CMD_GENFSCON:
		ret = sepol_require_not_all(args[0], "name");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[1], "path");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[2], "context");
		if (ret < 0)
			return ret;

		if (!tamisu_genfscon(db, args[0], args[1], args[2])) {
			pr_err("sepol: %d failed.\n", header->cmd);
			return -EINVAL;
		}
		return 0;

	default:
		pr_err("sepol: unknown cmd: %d\n", header->cmd);
		return -EINVAL;
	}
}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))
extern int avc_ss_reset(u32 seqno);
#else
extern int avc_ss_reset(struct selinux_avc *avc, u32 seqno);
#endif // #if (LINUX_VERSION_CODE >= KERNEL_VERSI...
// reset avc cache table, otherwise the new rules will not take effect if
// already denied
static void reset_avc_cache(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))
	avc_ss_reset(0);
	selnl_notify_policyload(0);
	selinux_status_update_policyload(0);
#else
	struct selinux_avc *avc = selinux_state.avc;
	avc_ss_reset(avc, 0);
	selnl_notify_policyload(0);
	selinux_status_update_policyload(&selinux_state, 0);
#endif // #if (LINUX_VERSION_CODE >= KERNEL_VERSI...
	selinux_xfrm_notify_policyload();
}

int handle_sepolicy(void __user *user_data, u64 data_len)
{
	struct selinux_policy *pol, *old_pol;
	struct policydb *db;
	struct sepol_batch_cursor cursor;
	u8 *payload;
	int ret;
	int success_cmd_count;
	u32 cmd_index;

	if (!user_data || !data_len)
		return -EINVAL;

	if (data_len > TAMISU_SEPOLICY_MAX_BATCH_SIZE)
		return -E2BIG;

	payload = kvmalloc((size_t)data_len, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	if (copy_from_user(payload, user_data, (size_t)data_len)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (!getenforce()) {
		pr_info("SELinux permissive or disabled when handle policy!\n");
	}

	mutex_lock(&selinux_state.policy_mutex);

	old_pol = selinux_state.policy;
	pol = tamisu_dup_sepolicy(rcu_dereference_protected(
	    old_pol, lockdep_is_held(&selinux_state.policy_mutex)));
	if (IS_ERR(pol)) {
		ret = PTR_ERR(pol);
		pr_err("tamisu_dup_sepolicy err: %d\n", ret);
		goto out_unlock;
	}
	db = &pol->policydb;

	cursor.cur = payload;
	cursor.end = payload + (size_t)data_len;

	ret = 0;
	success_cmd_count = 0;
	cmd_index = 0;
	while (cursor.cur < cursor.end) {
		struct sepol_data header;
		const char *args[TAMISU_SEPOLICY_MAX_ARGS] = {0};
		int expected_argc;
		u32 arg_index;

		ret = sepol_read_cmd_header(&cursor, &header);
		if (ret < 0) {
			pr_err("sepol: failed to read cmd header #%u.\n",
			       cmd_index);
			goto out_drop_new_policy;
		}

		expected_argc = sepol_expected_argc(header.cmd);
		if (expected_argc < 0 ||
		    expected_argc > TAMISU_SEPOLICY_MAX_ARGS) {
			ret = -EINVAL;
			pr_err("sepol: invalid cmd header #%u.\n", cmd_index);
			goto out_drop_new_policy;
		}

		for (arg_index = 0; arg_index < (u32)expected_argc;
		     arg_index++) {
			ret = sepol_read_string(&cursor, &args[arg_index]);
			if (ret < 0) {
				pr_err("sepol: failed to read cmd #%u arg "
				       "#%u.\n",
				       cmd_index, arg_index);
				goto out_drop_new_policy;
			}
		}

		ret = apply_one_sepolicy_cmd(db, &header, args);
		if (ret < 0) {
			pr_err("sepol: cmd #%u failed, cmd=%u subcmd=%u.\n",
			       cmd_index, header.cmd, header.subcmd);
		} else {
			success_cmd_count++;
		}
		cmd_index++;
	}

	rcu_assign_pointer(selinux_state.policy, pol);
	synchronize_rcu();
	tamisu_destroy_sepolicy(old_pol);

	reset_avc_cache();
	ret = success_cmd_count;
	goto out_unlock;

out_drop_new_policy:
	tamisu_destroy_sepolicy(pol);
out_unlock:
	mutex_unlock(&selinux_state.policy_mutex);
out_free:
	kvfree(payload);
	return ret;
}
