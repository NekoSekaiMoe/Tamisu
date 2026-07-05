#ifndef __TAMISU_UAPI_FEATURE_H
#define __TAMISU_UAPI_FEATURE_H

#ifdef __cplusplus
extern "C" {
#endif // #ifdef __cplusplus

enum tamisu_feature_id {
  // Tamisu zygisk-only build: the sole feature is the Tamisu Zygisk master switch.
  TAMISU_FEATURE_YUKIZYGISK = 103,

  TAMISU_FEATURE_MAX
};

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // #ifndef __TAMISU_UAPI_FEATURE_H
