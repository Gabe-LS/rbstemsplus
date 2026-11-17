/* The Windows bridge's checks that need no Windows call (bridge.md §1 and §3, task T4): the wide
 * model path, the CPU-tensor check, the version check. entry_win.c uses them, and so will the
 * hooks in rbstems_bridge.c once it builds for Windows (T1b2): there hook_CreateSession takes
 * ortw_is_demucs() in place of its strcmp on "/hdemucs.onnx", and view() refuses a tensor for
 * which ortw_on_cpu() is 0 before GetTensorMutableData, under #ifdef _WIN32 so the Mac's bytes stay
 * (I4). bridge/win/tests/ort.c runs them on the Mac.
 *
 * Memory rule (bridge.md §3 "CRT and heaps"): nothing here frees what ONNX Runtime returned. A
 * status goes back through R->ReleaseStatus; GetTensorMemoryInfo's info belongs to the value. */
#ifndef RBSP_ORT_WIN_H
#define RBSP_ORT_WIN_H

#include "onnxruntime_c_api.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Whether a model path is rekordbox's Demucs model: it ends in "hdemucs.onnx" after a '/' (rekordbox
 * joins the folder and the name with '/') or a '\'. p: the path's UTF-16 units (ORTCHAR_T is
 * wchar_t, 16 bits, on Windows); NULL is no. */
static inline int ortw_is_demucs16(const uint16_t *p) {
    static const char tail[] = "hdemucs.onnx";             /* 12 units */
    size_t n = 0;
    if (!p) return 0;
    while (p[n]) n++;
    if (n < 13 || (p[n - 13] != '/' && p[n - 13] != '\\')) return 0;
    for (size_t i = 0; i < 12; i++)
        if (p[n - 12 + i] != (uint16_t)tail[i]) return 0;
    return 1;
}

/* the real library is the ONNX Runtime 1.18 whose OrtApi layout the hooks patch */
static inline int ortw_version_ok(const char *v) { return v && strncmp(v, "1.18.", 5) == 0; }

/* Whether a tensor's data is in the CPU's memory (bridge.md §3 DirectML 1): only then may the
 * bridge read it through GetTensorMutableData; a DirectML tensor's pointer is GPU memory and
 * reading it would crash rekordbox. Anything the runtime can't answer counts as not on the CPU (the
 * caller passes the run through). R: the real library's OrtApi. */
static inline int ortw_on_cpu(const OrtApi *R, const OrtValue *v) {
    const OrtMemoryInfo *mi = NULL;
    OrtStatus *st = R->GetTensorMemoryInfo(v, &mi);
    if (st) {
        R->ReleaseStatus(st);
        return 0;
    }
    if (!mi) return 0;
    OrtMemoryInfoDeviceType t = OrtMemoryInfoDeviceType_GPU;
    R->MemoryInfoGetDeviceType(mi, &t);
#ifdef RBSTEMS_TEST_REAL_PATH
    /* TEST builds (BW1): the harness's gpu= runs name one value, by its address in hex, whose
     * device the runtime then answers as the GPU (a CPU-only VM has no DirectML tensor to hand) */
    const char *g = getenv("RBSTEMS_TEST_GPU_VALUE");
    if (g && *g && strtoull(g, NULL, 16) == (unsigned long long)(uintptr_t)v) t = OrtMemoryInfoDeviceType_GPU;
#endif
    return t == OrtMemoryInfoDeviceType_CPU;
}

/* every one of n values (NULL ones skipped: an output ORT will allocate) is on the CPU */
static inline int ortw_all_on_cpu(const OrtApi *R, const OrtValue *const *v, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (v && v[i] && !ortw_on_cpu(R, v[i])) return 0;
    return 1;
}

#endif
