/* For the VM: a stand-in real library for R1's "version mismatch" refusal (review B4). Its
 * OrtGetApiBase works and its version string is 1.18.0, but GetApi(18) (any version) returns NULL.
 * A test bridge whose REAL_PATH is this DLL must refuse it in phase 1: stubcall.exe then sees the
 * stub OrtApiBase, the stub OrtApi on every slot, and ordinals 2-4 returning the stub's status.
 * Its own providers must never be called: each ends the process with exit code 99.
 *
 * Built by bridge/win/build.sh --test into OUT/tests/fake-nov18/onnxruntime.dll, with ordinals 1-4
 * from tests/win/probes/ort.def. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "onnxruntime_c_api.h"

static const OrtApi *ORT_API_CALL f_GetApi(uint32_t version) {
    (void)version;
    return NULL;
}
static const char *ORT_API_CALL f_GetVersionString(void) { return "1.18.0"; }
static const OrtApiBase f_base = {f_GetApi, f_GetVersionString};

const OrtApiBase *ORT_API_CALL OrtGetApiBase(void) { return &f_base; }

OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProviderEx_DML(OrtSessionOptions *o, void *d, void *q) {
    (void)o; (void)d; (void)q;
    ExitProcess(99);
}
OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_CPU(OrtSessionOptions *o, int a) {
    (void)o; (void)a;
    ExitProcess(99);
}
OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(OrtSessionOptions *o, int d) {
    (void)o; (void)d;
    ExitProcess(99);
}
