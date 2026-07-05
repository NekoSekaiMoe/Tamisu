#ifndef __TAMISU_H_FEATURE
#define __TAMISU_H_FEATURE

#include <linux/types.h>
#include "uapi/feature.h" // IWYU pragma: keep

typedef int (*tamisu_feature_get_t)(u64 *value);
typedef int (*tamisu_feature_set_t)(u64 value);

struct tamisu_feature_handler {
	u32 feature_id;
	const char *name;
	tamisu_feature_get_t get_handler;
	tamisu_feature_set_t set_handler;
};

int tamisu_register_feature_handler(
    const struct tamisu_feature_handler *handler);

int tamisu_unregister_feature_handler(u32 feature_id);

int tamisu_get_feature(u32 feature_id, u64 *value, bool *supported);

int tamisu_set_feature(u32 feature_id, u64 value);

void tamisu_feature_init(void);

void tamisu_feature_exit(void);

#endif // #ifndef __TAMISU_H_FEATURE
