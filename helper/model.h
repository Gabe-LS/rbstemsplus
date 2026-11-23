/* model.c: the model through Pioneer's ONNX Runtime, one chunk at a time */
#ifndef RBSP_MODEL_H
#define RBSP_MODEL_H
#include <stddef.h>
#include "plat_helper.h"

typedef struct model model;
int model_load_ort(const char *lib, char *why, size_t whyn);
/* Prepare keeps path valid for the lifetime of the session, including any CPU fallback. */
model *model_open(const char *path, const plat_model_info *info, int threads, int ours, char *why, size_t whyn);
void model_close(model *m);
enum { MODEL_RUN_BATCH_ERROR = 2 };
/* x [4][4][BINS][F] and xt [8][L], malloc'd on success. Returns 0 on success,
 * 1 for a chunk failure, or MODEL_RUN_BATCH_ERROR when the session is unavailable. */
int model_run(model *m, const float *mix, size_t L, int ours, float **x, float **xt, size_t *F, char *why, size_t whyn);

#if defined(_WIN32) || defined(RBSP_TEST_WIN_MODEL)
/* Windows (helper.md §3, T12) */
typedef enum { MODEL_EP_CPU, MODEL_EP_DML } model_ep;
/* the automatic --threads: NCPU / 2 (GetActiveProcessorCount's) clamped to 1-64; 0 or less gives 1 */
int model_default_threads(long ncpu);
/* DirectML when ASKED, DIRECTML_LOADED and APPEND_FOUND (ORT's ordinal 4), else the CPU */
model_ep model_ep_choice(int asked, int directml_loaded, int append_found);
/* after a failed run on EP: 1 when the chunk runs again on a new CPU session (DirectML's), 0 when
 * the failure stands */
int model_retry_on_cpu(model_ep ep);
/* the timing lines' " power=ac|battery|unknown ep=cpu|dml" from ACLineStatus; its length, 0 if it
 * doesn't fit (BUF "") */
int model_timing_fields(char *buf, size_t n, int ac_line_status, model_ep ep);
#endif
#if defined(_WIN32)
/* --dml, before model_load_ort: DirectML.dll loaded by its full path from rekordbox's versioned
 * folder, its path and version on stderr; 1 (said on stderr) when it can't be: the CPU then */
int model_use_dml(const char *rekordbox_dir);
/* the session's execution provider now (model_open's; the CPU after a DirectML run failure) */
model_ep model_ep_used(void);
/* GetActiveProcessorCount(ALL_PROCESSOR_GROUPS): 0 when it fails */
long model_active_processors(void);
/* GetSystemPowerStatus's ACLineStatus (1 AC, 0 battery, 255 unknown or failed) */
int model_ac_line_status(void);
/* the run's process: below-normal priority and the power-throttling opt-out (execution speed
 * controlled, not throttled); 0, or bit 0 (priority) / bit 1 (throttling) for what failed */
int model_background(void);
#endif
#endif
