/* Phase 0 probe: validates the loader-lock check (R3) and the stub (R1) together.
 *   llcheck.exe STUB_DLL CALLER_DLL
 * Loads the stub onnxruntime.dll by full path, then llcaller.dll, whose DllMain calls ordinal 1,
 * GetApi(18), CreateEnv, GetErrorMessage and ReleaseStatus under the loader lock, the way
 * Demucs3.dll's DllMain does. Then main calls ordinal 1 outside the lock. The stub's log
 * (C:\rbtest\stub.log) must show lock=1 from llcaller.dll and lock=0 from llcheck.exe, and
 * nothing may deadlock or crash. With -DCALLER it builds llcaller.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "onnxruntime_c_api.h"

typedef const OrtApiBase *(ORT_API_CALL *getbase_f)(void);

static int exercise(const char *who) {
    HMODULE m = GetModuleHandleW(L"onnxruntime.dll");
    if (!m) return 10;
    getbase_f get = (getbase_f)(void *)GetProcAddress(m, MAKEINTRESOURCEA(1));
    if (!get) return 11;
    const OrtApiBase *base = get();
    if (!base) return 12;
    const OrtApi *api = base->GetApi(ORT_API_VERSION);
    if (!api) return 13;
    OrtEnv *env = NULL;
    OrtStatus *st = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, who, &env);
    if (!st) return 14;
    const char *msg = api->GetErrorMessage(st);
    OrtErrorCode code = api->GetErrorCode(st);
    api->ReleaseStatus(st);
    OrtMemoryInfoDeviceType dt = OrtMemoryInfoDeviceType_GPU;
    api->MemoryInfoGetDeviceType(NULL, &dt);
    return (msg && *msg && code == ORT_FAIL && dt == OrtMemoryInfoDeviceType_CPU && api->GetTrainingApi(ORT_API_VERSION) == NULL) ? 0 : 15;
}

#ifdef CALLER
static volatile int dllmain_result = -1;
__declspec(dllexport) int llcaller_result(void) { return dllmain_result; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)h; (void)r;
    if (why == DLL_PROCESS_ATTACH) dllmain_result = exercise("llcaller DllMain");
    return TRUE;
}
#else
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: llcheck STUB_DLL CALLER_DLL\n"); return 2; }
    if (!LoadLibraryA(argv[1])) { printf("stub load failed %lu\n", GetLastError()); return 3; }
    HMODULE c = LoadLibraryA(argv[2]);
    if (!c) { printf("caller load failed %lu\n", GetLastError()); return 4; }
    int (*res)(void) = (int (*)(void))(void *)GetProcAddress(c, "llcaller_result");
    int in_dllmain = res ? res() : -2;
    int in_main = exercise("llcheck main");
    printf("stub under the loader lock (llcaller DllMain): %s (%d)\n", in_dllmain ? "FAILED" : "ok", in_dllmain);
    printf("stub outside the lock (main): %s (%d)\n", in_main ? "FAILED" : "ok", in_main);
    return in_dllmain || in_main;
}
#endif
