#ifndef __TAMISU_H_SEPOLICY
#define __TAMISU_H_SEPOLICY

#include <linux/types.h>

#include "ss/policydb.h"

struct selinux_policy *tamisu_dup_sepolicy(struct selinux_policy *old_pol);
void tamisu_destroy_sepolicy(struct selinux_policy *pol);

// Operation on types
bool tamisu_type(struct policydb *db, const char *name, const char *attr);
bool tamisu_attribute(struct policydb *db, const char *name);
bool tamisu_permissive(struct policydb *db, const char *type);
bool tamisu_enforce(struct policydb *db, const char *type);
bool tamisu_typeattribute(struct policydb *db, const char *type,
			  const char *attr);

// Access vector rules
bool tamisu_allow(struct policydb *db, const char *src, const char *tgt,
		  const char *cls, const char *perm);
bool tamisu_deny(struct policydb *db, const char *src, const char *tgt,
		 const char *cls, const char *perm);
bool tamisu_auditallow(struct policydb *db, const char *src, const char *tgt,
		       const char *cls, const char *perm);
bool tamisu_dontaudit(struct policydb *db, const char *src, const char *tgt,
		      const char *cls, const char *perm);

// Extended permissions access vector rules
bool tamisu_allowxperm(struct policydb *db, const char *src, const char *tgt,
		       const char *cls, const char *range);
bool tamisu_auditallowxperm(struct policydb *db, const char *src,
			    const char *tgt, const char *cls,
			    const char *range);
bool tamisu_dontauditxperm(struct policydb *db, const char *src,
			   const char *tgt, const char *cls, const char *range);

// Type rules
bool tamisu_type_transition(struct policydb *db, const char *src,
			    const char *tgt, const char *cls, const char *def,
			    const char *obj);
bool tamisu_type_change(struct policydb *db, const char *src, const char *tgt,
			const char *cls, const char *def);
bool tamisu_type_member(struct policydb *db, const char *src, const char *tgt,
			const char *cls, const char *def);

// File system labeling
bool tamisu_genfscon(struct policydb *db, const char *fs_name, const char *path,
		     const char *ctx);

#endif // #ifndef __TAMISU_H_SEPOLICY
