/* For the VM (bridge.md §4 "the stub OrtApiBase", r4:I46, J3): loads a bridge DLL whose real
 * library is refused (a test build pointed at a REAL_PATH that is missing, user-owned, a reparse
 * point, not a DLL...), calls ordinal 1 as rekordbox.exe does, then GetApi(18), and every slot of
 * the table it got through the typed OrtApi (stub_call, generated with the stub), and ordinals 2-4.
 * Every status must be a failed one the same table reads and releases; nothing may crash.
 *
 *   stubcall.exe BRIDGE_DLL        exits 0 when every check passed
 *
 * Built by bridge/win/build.sh --test (x64). Run it unelevated, once per refusal path. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "onnxruntime_c_api.h"

struct OrtStatus { OrtErrorCode code; const char *msg; };   /* the generated file's own (unused here) */
static OrtStatus stub_status = {ORT_FAIL, "stubcall"};
#define STUB_SELFTEST 1
#include "../stub_api.inc"

typedef const OrtApiBase *(ORT_API_CALL *get_base)(void);
typedef OrtStatus *(ORT_API_CALL *ep_ex)(OrtSessionOptions *, void *, void *);
typedef OrtStatus *(ORT_API_CALL *ep_int)(OrtSessionOptions *, int);

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

#define PROC(dst, mod, ord)                                       \
    do {                                                          \
        FARPROC f_ = GetProcAddress((mod), MAKEINTRESOURCEA(ord)); \
        memcpy(&(dst), &f_, sizeof f_);                           \
    } while (0)

static void failed_status(const OrtApi *api, OrtStatus *st, const char *what) {
    CHECK(st != NULL, "%s: no status", what);
    if (!st) return;
    CHECK(api->GetErrorCode(st) == ORT_FAIL, "%s: code %d", what, (int)api->GetErrorCode(st));
    const char *m = api->GetErrorMessage(st);
    CHECK(m && *m, "%s: no message", what);
    api->ReleaseStatus(st);
}

int main(int argc, char **argv) {
    (void)stub_api;
    if (argc != 2) { fprintf(stderr, "usage: stubcall BRIDGE_DLL\n"); return 2; }
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("FAIL: load %s: error %lu\n", argv[1], GetLastError()); return 1; }
    get_base get;
    PROC(get, m, 1);
    CHECK(get != NULL, "no ordinal 1");
    if (!get) return 1;
    const OrtApiBase *base = get();
    CHECK(base != NULL, "OrtGetApiBase returned NULL");
    if (!base) return 1;
    const char *v = base->GetVersionString();
    CHECK(v && *v, "an empty version");
    const OrtApi *api = base->GetApi(ORT_API_VERSION);
    CHECK(api != NULL && base->GetApi(99) != NULL, "GetApi returned NULL");
    if (!api) return 1;
    int kinds[6] = {0};
    for (int i = 0; i < STUB_SLOTS; i++) {
        uintptr_t r = 0;
        stub_device = OrtMemoryInfoDeviceType_GPU;
        int k = stub_call(api, i, &r);
        CHECK(k >= 0 && k <= STUB_VOID, "%s", stub_names[i]);
        if (k < 0 || k > STUB_VOID) continue;
        kinds[k]++;
        if (k == STUB_STATUS) failed_status(api, (OrtStatus *)r, stub_names[i]);
        else if (k == STUB_CODE) CHECK(r == (uintptr_t)ORT_FAIL, "%s", stub_names[i]);
        else if (k == STUB_TEXT) CHECK(r && *(const char *)r, "%s", stub_names[i]);
        else if (k == STUB_DEVICE) CHECK(stub_device == OrtMemoryInfoDeviceType_CPU, "%s", stub_names[i]);
        else if (k == STUB_NULL) CHECK(r == 0, "%s", stub_names[i]);
    }
    ep_ex dml_ex;
    ep_int cpu, dml;
    PROC(dml_ex, m, 2);
    PROC(cpu, m, 3);
    PROC(dml, m, 4);
    CHECK(dml_ex && cpu && dml, "ordinals 2-4");
    long long dummy[8] = {0};
    if (dml_ex) failed_status(api, dml_ex((OrtSessionOptions *)(void *)dummy, NULL, NULL), "ordinal 2");
    if (cpu) failed_status(api, cpu((OrtSessionOptions *)(void *)dummy, 1), "ordinal 3");
    if (dml) failed_status(api, dml((OrtSessionOptions *)(void *)dummy, 0), "ordinal 4");
    CHECK(GetProcAddress(m, MAKEINTRESOURCEA(5)) && !strcmp((const char *)(void *)GetProcAddress(m, "rbstems_marker"), "rbstems-bridge"),
          "ordinal 5, rbstems_marker");
    printf("stubcall: version %s; %d slots called (%d status); %d checks, %d failed\n", v, STUB_SLOTS, kinds[STUB_STATUS], checks, failures);
    return failures ? 1 : 0;
}
