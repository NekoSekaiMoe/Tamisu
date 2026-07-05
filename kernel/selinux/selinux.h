#ifndef __TAMISU_H_SELINUX
#define __TAMISU_H_SELINUX

#include "linux/cred.h"
#include "linux/types.h"
#include "linux/version.h"

struct file;

/* YukiSU defaults to the su domain; keep tamisu as a future feature-controlled
 * option. */
#define TAMISU_DOMAIN "su"
#define KERNEL_SU_FILE "tamisu_file"

#define TAMISU_CONTEXT "u:r:" TAMISU_DOMAIN ":s0"
#define TAMISU_FILE_CONTEXT "u:object_r:" KERNEL_SU_FILE ":s0"
#define ZYGOTE_CONTEXT "u:r:zygote:s0"
#define INIT_CONTEXT "u:r:init:s0"

void setenforce(bool);

bool getenforce(void);

bool is_task_tamisu_domain(const struct cred *cred);

bool is_zygote(const struct cred *cred);

bool is_init(const struct cred *cred);

void apply_tamisu_rules(void);
void cache_sid(void);

u32 tamisu_get_tamisu_file_sid(void);

struct tamisu_file_load_policy {
	u32 src_type;
	u32 tgt_type;
	u32 tmpfs_type;
	u16 target_class;
	u16 reserved;
	u32 added_av;
	u32 tmpfs_added_av;
};

int tamisu_file_load_policy_allow_current(struct file *file,
				       struct tamisu_file_load_policy *state);
int tamisu_file_load_policy_restore(const struct tamisu_file_load_policy *state);

int handle_sepolicy(void __user *user_data, u64 data_len);

void setup_tamisu_cred(void);

#endif // #ifndef __TAMISU_H_SELINUX
