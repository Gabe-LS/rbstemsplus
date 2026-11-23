/* entry.c: a cache entry as the bridge writes one */
#ifndef RBSP_ENTRY_H
#define RBSP_ENTRY_H
#include <stddef.h>
#include <stdint.h>
#include "plat_helper.h"

#define REBUILD_VERSION "1"             /* the bridge's: the folder of rekordbox's model's rebuilt stems */
#define PIONEER_0002 "435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a"
#define OURS_V4 "0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772"
#define FRAME_FLOOR 1e-6                /* chunks quieter than -60 dBFS (mean square) aren't judged */
#define REBUILT_RESIDUAL_MAX (-10.0)    /* dB: rebuilt stems must explain the chunk at least this well */
#define EXACT_RESIDUAL_MAX (-10.0)      /* dB: any entry on another drive (the bridge refuses others there) */
#define BINS_ 2048

/* the sha256s go through common/plat.h's interface (CommonCrypto on the Mac): 0, or nonzero when
 * the OS's hash can't be had (Windows only) and nothing was written to out */
int rebuilt_id(const char *fp, char out[65]);
/* If info is supplied, capture the model's identity from the handle before reading it. */
int file_sha256(const char *path, char out[65], plat_model_info *info);
int chunk_key(const char *model, const float *mix, size_t L, size_t F, char key[65]);
void mean_std(const float *mix, size_t count, double *mean, double *std);
int ours_pcm(const float *xt, const float *mix, size_t L, int16_t *pcm, const char **why);
int rebuilt_pcm(const float *x, size_t F, const float *xt, const float *mix, size_t L, int16_t *pcm, const char **why);
int store(plat_dir x, const char *key, const int16_t *pcm, const float *mix, size_t L, int rebuilt, int chosen,
          const char *helper, const char **why, int *err);
#endif
