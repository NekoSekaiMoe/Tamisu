#ifndef __KSU_UAPI_FEATURE_H
#define __KSU_UAPI_FEATURE_H

#ifdef __cplusplus
extern "C" {
#endif // #ifdef __cplusplus

enum ksu_feature_id {
  // YukiSU zygisk-only build: the sole feature is the YukiZygisk master switch.
  KSU_FEATURE_YUKIZYGISK = 103,

  KSU_FEATURE_MAX
};

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // #ifndef __KSU_UAPI_FEATURE_H
