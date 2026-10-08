/* A stand-in "real" library that is not ONNX Runtime 1.18 (it reports 1.17.0), for bridge/tests:
 * the bridge must hand it through untouched. Same install name as Pioneer's library. */
#include <stddef.h>
#include <stdint.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT const char fake_ort_marker[] = "fake-ort";
static const int fake_api;                        /* GetApi's answer: only its address matters */

static const void *get_api(uint32_t version) { (void)version; return &fake_api; }
static const char *get_version(void) { return "1.17.0"; }

static struct { const void *(*GetApi)(uint32_t); const char *(*GetVersionString)(void); } base = {get_api, get_version};

EXPORT const void *OrtGetApiBase(void) { return &base; }

/* the providers return recognisable non-NULL "statuses" */
EXPORT const void *OrtSessionOptionsAppendExecutionProvider_CPU(void *o, int arena) { (void)o; (void)arena; return &fake_ort_marker[1]; }
EXPORT const void *OrtSessionOptionsAppendExecutionProvider_CoreML(void *o, uint32_t f) { (void)o; (void)f; return &fake_ort_marker[2]; }
