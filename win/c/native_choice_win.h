/* The Windows location choice's ops and the measure's rekordbox ops (native_location_ops'
 * rekordbox_running and process_start), shared by the DLL and winc-location-wintest. */
#ifndef RBSP_WIN_NATIVE_CHOICE_WIN_H
#define RBSP_WIN_NATIVE_CHOICE_WIN_H

#include <stddef.h>
#include <stdint.h>

int nc_volume(void *ctx, const char *folder, int *refusal, char *root, size_t rn, char *real, size_t n,
              void (*log)(void *, const char *));
int nc_new_id(void *ctx, char out[40]);
void nc_not_indexed(void *ctx, const char *dir);
int nl_rekordbox_running(void *ctx);
int64_t nl_process_start(void *ctx, int32_t pid);

#endif
