/* Phase 0 probe (the Windows plan §3 "VM probes"; bridge.md §3 and §6 Q1-Q2): stands in for
 * rekordbox's onnxruntime.dll, passes every call to Pioneer's real library and logs
 * (C:\rbtest\ortprobe.log):
 *  - R3, the loader lock: for each OrtGetApiBase/GetApi call, the calling module and whether this
 *    thread owns the PEB LoaderLock (validated by DllMain, which must read 1);
 *  - the modules loaded by a phase-1-shaped start (the descriptor of the real DLL and its folder
 *    read from handles opened with READ_CONTROL | FILE_READ_ATTRIBUTES and
 *    FILE_FLAG_OPEN_REPARSE_POINT, then LoadLibraryExW), as bridge.md §3 asks (r4:I48);
 *  - CPU tensors: GetTensorMemoryInfo + MemoryInfoGetDeviceType on every Run input and output,
 *    and whether any output was pre-allocated by the caller;
 *  - IoBinding: CreateIoBinding, Bind*, RunWithBinding;
 *  - session options rekordbox sets (threads, execution mode, mem pattern, arena, optimisation,
 *    config entries, clone), env creation, execution providers;
 *  - whether ReleaseSession ever arrives with ntdll's RtlDllShutdownInProgress true (T10, BX1).
 * Changes nothing rekordbox gets back. The real library: C:\Program Files\RB Stems Plus\ort\. */
#include "probe_common.h"
#include "onnxruntime_c_api.h"
#include <aclapi.h>

#define REAL_DIR L"C:\\Program Files\\RB Stems Plus\\ort"
#define REAL REAL_DIR L"\\onnxruntime.dll"

static HMODULE real;
static const OrtApiBase *real_base;
static const OrtApi *R;
static OrtApi api;
static SRWLOCK lk = SRWLOCK_INIT;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER freq;
static int dllmain_lock = -2;
/* fault injection for the stub-crash investigation: C:\rbtest\fail-createenv or
 * C:\rbtest\fail-createsession or C:\rbtest\fail-run makes that call fail with a real ORT status;
 * C:\rbtest\fail-fakerun makes Run "succeed" with zero outputs without running the model */
static int fail_env, fail_session, fail_run, fake_run;

#define IGN(x) do { OrtStatus *s_ = (x); if (s_) R->ReleaseStatus(s_); } while (0)

static void caller(const void *ra, char *m, size_t n) { module_of(ra, m, n); }

/* the descriptor of a path, read from a handle opened as the bridge's phase 1 will */
static void read_sd(const wchar_t *path, int dir) {
    DWORD fl = FILE_FLAG_OPEN_REPARSE_POINT | (dir ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    HANDLE h = CreateFileW(path, READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, fl, NULL);
    if (h == INVALID_HANDLE_VALUE) { plog("phase1: open %ls failed %lu", path, GetLastError()); return; }
    char sd[4096];
    DWORD need = 0;
    BOOL ok = GetKernelObjectSecurity(h, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, sd, sizeof sd, &need);
    FILE_ATTRIBUTE_TAG_INFO tag = {0};
    GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag);
    plog("phase1: %ls descriptor %s (%lu bytes), attributes 0x%lx, reparse tag 0x%lx", path, ok ? "read" : "FAILED",
         ok ? (DWORD)GetSecurityDescriptorLength(sd) : GetLastError(), tag.FileAttributes, tag.ReparseTag);
    CloseHandle(h);
}

static BOOL CALLBACK init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    QueryPerformanceFrequency(&freq);
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    static modset before, after;
    modules_now(&before);
    plog("phase1 start in %ls, lock=%d, %lu modules loaded", exe, owns_loader_lock(), before.n);
    modules_log("modules before phase1", NULL, &before);
    fail_env = GetFileAttributesW(L"C:\\rbtest\\fail-createenv") != INVALID_FILE_ATTRIBUTES;
    fail_session = GetFileAttributesW(L"C:\\rbtest\\fail-createsession") != INVALID_FILE_ATTRIBUTES;
    fail_run = GetFileAttributesW(L"C:\\rbtest\\fail-run") != INVALID_FILE_ATTRIBUTES;
    fake_run = GetFileAttributesW(L"C:\\rbtest\\fail-fakerun") != INVALID_FILE_ATTRIBUTES;
    plog("fault injection: CreateEnv %s, CreateSession %s, Run %s", fail_env ? "FAILS" : "real", fail_session ? "FAILS" : "real",
         fail_run ? "FAILS" : fake_run ? "FAKED (zero outputs, model not run)" : "real");
    read_sd(REAL_DIR, 1);
    read_sd(REAL, 0);
    real = LoadLibraryExW(REAL, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!real) { plog("ERROR loading the real library: error %lu", GetLastError()); return TRUE; }
    const OrtApiBase *(ORT_API_CALL *get)(void) = (const OrtApiBase *(ORT_API_CALL *)(void))GetProcAddress(real, "OrtGetApiBase");
    real_base = get ? get() : NULL;
    modules_now(&after);
    modules_log("modules loaded by phase1", &before, &after);
    plog("real ONNX Runtime %s", real_base ? real_base->GetVersionString() : "MISSING");
    return TRUE;
}
static void start(void) { InitOnceExecuteOnce(&once, init, NULL, NULL); }

static void status_text(OrtStatus *st, char *out, size_t n) {
    if (!st) { snprintf(out, n, "ok"); return; }
    snprintf(out, n, "FAILED: %s", R ? R->GetErrorMessage(st) : "?");
}

/* ---- sessions */
#define MAXS 32
static struct { OrtSession *s; char model[160]; long runs; double ms; } ss[MAXS];
static int find(OrtSession *s) {
    for (int i = 0; i < MAXS; i++) if (ss[i].s == s) return i;
    return -1;
}

static OrtStatus *ORT_API_CALL h_CreateSession(const OrtEnv *env, const ORTCHAR_T *path, const OrtSessionOptions *o, OrtSession **out) {
    char p[400] = "";
    WideCharToMultiByte(CP_UTF8, 0, path, -1, p, sizeof p, NULL, NULL);
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    OrtStatus *st = fail_session ? R->CreateStatus(ORT_FAIL, "probe: CreateSession refused") : R->CreateSession(env, path, o, out);
    QueryPerformanceCounter(&b);
    char r[300]; status_text(st, r, sizeof r);
    const char *base = strrchr(p, '\\'); base = base ? base + 1 : p;
    const char *b2 = strrchr(base, '/'); base = b2 ? b2 + 1 : base;
    plog("CreateSession %s options %p -> %s (%.0f ms)", base, (void *)o, r, (b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart);
    if (!st && *out) {
        AcquireSRWLockExclusive(&lk);
        int i = find(NULL);
        if (i >= 0) { ss[i].s = *out; snprintf(ss[i].model, sizeof ss[i].model, "%s", base); ss[i].runs = 0; ss[i].ms = 0; }
        ReleaseSRWLockExclusive(&lk);
    }
    return st;
}
static OrtStatus *ORT_API_CALL h_CreateSessionFromArray(const OrtEnv *env, const void *d, size_t n, const OrtSessionOptions *o, OrtSession **out) {
    OrtStatus *st = R->CreateSessionFromArray(env, d, n, o, out);
    char r[300]; status_text(st, r, sizeof r);
    plog("CreateSessionFromArray (%zu bytes) -> %s", n, r);
    return st;
}
/* T10, review BX1: does ReleaseSession ever arrive once ExitProcess has begun (ntdll's
 * RtlDllShutdownInProgress true, the other threads killed)? Logged without lk, which a killed
 * thread may hold. plog's lock is held only around one append; a hang at exit here would be an
 * answer too */
static void ORT_API_CALL h_ReleaseSession(OrtSession *s) {
    static BOOLEAN(NTAPI *down)(void);
    static volatile LONG resolved;
    if (!resolved) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        FARPROC f = nt ? GetProcAddress(nt, "RtlDllShutdownInProgress") : NULL;
        memcpy((void *)&down, &f, sizeof f);
        InterlockedExchange(&resolved, 1);
    }
    if (down && down()) {
        char m[300] = "";
        caller(__builtin_return_address(0), m, sizeof m);
        plog("ReleaseSession(%p) with shutdown in progress (BX1), from %s", (void *)s, m);
        R->ReleaseSession(s);
        return;
    }
    AcquireSRWLockExclusive(&lk);
    int i = find(s);
    if (i >= 0) {
        if (ss[i].runs) plog("ReleaseSession %s: %ld runs, %.0f ms each on average", ss[i].model, ss[i].runs, ss[i].ms / ss[i].runs);
        ss[i].s = NULL;
    }
    ReleaseSRWLockExclusive(&lk);
    R->ReleaseSession(s);
}

/* "type T [dims] dev=cpu|gpu|? mem=NAME" for a tensor */
static void describe(const OrtValue *v, char *out, size_t n) {
    if (!v) { snprintf(out, n, "NULL"); return; }
    OrtTensorTypeAndShapeInfo *info = NULL;
    int k;
    if (R->GetTensorTypeAndShape(v, &info)) { k = snprintf(out, n, "(not a tensor)"); }
    else {
        ONNXTensorElementDataType t; size_t nd = 0; int64_t dims[8] = {0};
        IGN(R->GetTensorElementType(info, &t));
        IGN(R->GetDimensionsCount(info, &nd));
        if (nd > 8) nd = 8;
        IGN(R->GetDimensions(info, dims, nd));
        R->ReleaseTensorTypeAndShapeInfo(info);
        k = snprintf(out, n, "type %d [", (int)t);
        for (size_t i = 0; i < nd && k < (int)n; i++) k += snprintf(out + k, n - k, i ? ",%lld" : "%lld", (long long)dims[i]);
        if (k < (int)n) k += snprintf(out + k, n - k, "]");
    }
    const OrtMemoryInfo *mi = NULL;
    if (k < (int)n && !R->GetTensorMemoryInfo(v, &mi) && mi) {
        OrtMemoryInfoDeviceType dt = (OrtMemoryInfoDeviceType)-1;
        R->MemoryInfoGetDeviceType(mi, &dt);
        const char *name = "?";
        IGN(R->MemoryInfoGetName(mi, &name));
        OrtMemType mt = OrtMemTypeDefault;
        IGN(R->MemoryInfoGetMemType(mi, &mt));
        snprintf(out + k, n - k, " dev=%s mem=%s memtype=%d", dt == OrtMemoryInfoDeviceType_CPU ? "CPU" : dt == OrtMemoryInfoDeviceType_GPU ? "GPU" : "OTHER",
                 name ? name : "?", (int)mt);
    }
}

/* fail-fakerun: Run "succeeds" with zero-filled outputs of hdemucs.onnx's shapes, the model unrun */
static OrtStatus *fake(const char *const *names, size_t n, OrtValue **out) {
    OrtAllocator *a = NULL;
    OrtStatus *st = R->GetAllocatorWithDefaultOptions(&a);
    if (st) return st;
    static const int64_t x[] = {1, 4, 4, 2048, 474}, xt[] = {1, 8, 485100};
    for (size_t j = 0; j < n; j++) {
        int isx = strcmp(names[j], "x") == 0;
        const int64_t *shape = isx ? x : xt;
        size_t nd = isx ? 5 : 3, count = 1;
        for (size_t k = 0; k < nd; k++) count *= (size_t)shape[k];
        st = R->CreateTensorAsOrtValue(a, shape, nd, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &out[j]);
        if (st) return st;
        void *p = NULL;
        st = R->GetTensorMutableData(out[j], &p);
        if (st) return st;
        memset(p, 0, count * sizeof(float));
    }
    return NULL;
}

static OrtStatus *ORT_API_CALL h_Run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names, const OrtValue *const *in,
                                     size_t nin, const char *const *out_names, size_t nout, OrtValue **out) {
    int prealloc = 0;
    char pre[600] = "";
    int pk = 0;
    for (size_t j = 0; j < nout; j++)
        if (out[j]) { prealloc++; char d[160]; describe(out[j], d, sizeof d); pk += snprintf(pre + pk, sizeof pre - pk, " | pre %s %s", out_names[j], d); if (pk >= (int)sizeof pre) pk = sizeof pre - 1; }
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    OrtStatus *st = fail_run ? R->CreateStatus(ORT_FAIL, "probe: Run refused")
                    : fake_run ? fake(out_names, nout, out) : R->Run(s, ro, in_names, in, nin, out_names, nout, out);
    QueryPerformanceCounter(&b);
    double ms = (b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart;
    AcquireSRWLockExclusive(&lk);
    int i = find(s);
    long run = 0; char model[160] = "?";
    if (i >= 0) { run = ++ss[i].runs; ss[i].ms += ms; memcpy(model, ss[i].model, sizeof model); }
    ReleaseSRWLockExclusive(&lk);
    if (run <= 3 || run % 20 == 0) {
        char line[1400]; int k = snprintf(line, sizeof line, "Run %s #%ld %.0f ms, %d of %zu outputs pre-allocated%s", model, run, ms, prealloc, nout, pre);
        for (size_t j = 0; j < nin && k < (int)sizeof line; j++) {
            char d[160]; describe(in[j], d, sizeof d);
            k += snprintf(line + k, sizeof line - k, " | in %s %s", in_names[j], d);
        }
        for (size_t j = 0; j < nout && k < (int)sizeof line; j++) {
            char d[160]; describe(st ? NULL : out[j], d, sizeof d);
            k += snprintf(line + k, sizeof line - k, " | out %s %s", out_names[j], d);
        }
        if (st && k < (int)sizeof line) { char r[300]; status_text(st, r, sizeof r); snprintf(line + k, sizeof line - k, " -> %s", r); }
        plog("%s", line);
    }
    return st;
}

/* ---- IoBinding */
static OrtStatus *ORT_API_CALL h_RunWithBinding(OrtSession *s, const OrtRunOptions *ro, const OrtIoBinding *b) {
    static volatile LONG n;
    LONG c = InterlockedIncrement(&n);
    OrtStatus *st = R->RunWithBinding(s, ro, b);
    if (c <= 5 || c % 20 == 0) { char r[300]; status_text(st, r, sizeof r); plog("RunWithBinding #%ld -> %s", c, r); }
    return st;
}
static OrtStatus *ORT_API_CALL h_CreateIoBinding(OrtSession *s, OrtIoBinding **out) {
    OrtStatus *st = R->CreateIoBinding(s, out);
    plog("CreateIoBinding -> %s", st ? "FAILED" : "ok");
    return st;
}
static OrtStatus *ORT_API_CALL h_BindInput(OrtIoBinding *b, const char *name, const OrtValue *v) {
    char d[160]; describe(v, d, sizeof d);
    plog("BindInput %s %s", name, d);
    return R->BindInput(b, name, v);
}
static OrtStatus *ORT_API_CALL h_BindOutput(OrtIoBinding *b, const char *name, const OrtValue *v) {
    char d[160]; describe(v, d, sizeof d);
    plog("BindOutput %s %s", name, d);
    return R->BindOutput(b, name, v);
}
static OrtStatus *ORT_API_CALL h_BindOutputToDevice(OrtIoBinding *b, const char *name, const OrtMemoryInfo *mi) {
    const char *nm = "?"; if (mi) IGN(R->MemoryInfoGetName(mi, &nm));
    plog("BindOutputToDevice %s mem=%s", name, nm ? nm : "?");
    return R->BindOutputToDevice(b, name, mi);
}

/* ---- options and env */
static OrtStatus *ORT_API_CALL h_CreateEnv(OrtLoggingLevel l, const char *id, OrtEnv **out) {
    char m[64]; caller(__builtin_return_address(0), m, sizeof m);
    OrtStatus *st = fail_env ? R->CreateStatus(ORT_FAIL, "probe: CreateEnv refused") : R->CreateEnv(l, id, out);
    plog("CreateEnv(level %d, '%s') from %s lock=%d -> %s", (int)l, id ? id : "", m, owns_loader_lock(), st ? "FAILED" : "ok");
    return st;
}
static OrtStatus *ORT_API_CALL h_CreateEnvWithGlobalThreadPools(OrtLoggingLevel l, const char *id, const OrtThreadingOptions *t, OrtEnv **out) {
    OrtStatus *st = R->CreateEnvWithGlobalThreadPools(l, id, t, out);
    plog("CreateEnvWithGlobalThreadPools(level %d, '%s') -> %s", (int)l, id ? id : "", st ? "FAILED" : "ok");
    return st;
}
static OrtStatus *ORT_API_CALL h_CreateSessionOptions(OrtSessionOptions **out) {
    OrtStatus *st = R->CreateSessionOptions(out);
    plog("CreateSessionOptions -> %p", st ? NULL : (void *)*out);
    return st;
}
static OrtStatus *ORT_API_CALL h_CloneSessionOptions(const OrtSessionOptions *in, OrtSessionOptions **out) {
    OrtStatus *st = R->CloneSessionOptions(in, out);
    plog("CloneSessionOptions %p -> %p", (void *)in, st ? NULL : (void *)*out);
    return st;
}
static OrtStatus *ORT_API_CALL h_SetIntraOpNumThreads(OrtSessionOptions *o, int n) { plog("SetIntraOpNumThreads(%p, %d)", (void *)o, n); return R->SetIntraOpNumThreads(o, n); }
static OrtStatus *ORT_API_CALL h_SetInterOpNumThreads(OrtSessionOptions *o, int n) { plog("SetInterOpNumThreads(%p, %d)", (void *)o, n); return R->SetInterOpNumThreads(o, n); }
static OrtStatus *ORT_API_CALL h_SetSessionExecutionMode(OrtSessionOptions *o, ExecutionMode m) { plog("SetSessionExecutionMode(%p, %d)", (void *)o, (int)m); return R->SetSessionExecutionMode(o, m); }
static OrtStatus *ORT_API_CALL h_EnableMemPattern(OrtSessionOptions *o) { plog("EnableMemPattern(%p)", (void *)o); return R->EnableMemPattern(o); }
static OrtStatus *ORT_API_CALL h_DisableMemPattern(OrtSessionOptions *o) { plog("DisableMemPattern(%p)", (void *)o); return R->DisableMemPattern(o); }
static OrtStatus *ORT_API_CALL h_EnableCpuMemArena(OrtSessionOptions *o) { plog("EnableCpuMemArena(%p)", (void *)o); return R->EnableCpuMemArena(o); }
static OrtStatus *ORT_API_CALL h_DisableCpuMemArena(OrtSessionOptions *o) { plog("DisableCpuMemArena(%p)", (void *)o); return R->DisableCpuMemArena(o); }
static OrtStatus *ORT_API_CALL h_SetGOL(OrtSessionOptions *o, GraphOptimizationLevel g) { plog("SetSessionGraphOptimizationLevel(%p, %d)", (void *)o, (int)g); return R->SetSessionGraphOptimizationLevel(o, g); }
static OrtStatus *ORT_API_CALL h_AddSessionConfigEntry(OrtSessionOptions *o, const char *k, const char *v) { plog("AddSessionConfigEntry(%p, %s=%s)", (void *)o, k, v); return R->AddSessionConfigEntry(o, k, v); }
static OrtStatus *ORT_API_CALL h_AppendEP(OrtSessionOptions *o, const char *name, const char *const *keys, const char *const *vals, size_t n) {
    OrtStatus *st = R->SessionOptionsAppendExecutionProvider(o, name, keys, vals, n);
    plog("SessionOptionsAppendExecutionProvider %s (%zu options) -> %s", name, n, st ? "FAILED" : "ok");
    return st;
}
static OrtStatus *ORT_API_CALL h_GetEPApi(const char *name, uint32_t v, const void **out) {
    OrtStatus *st = R->GetExecutionProviderApi(name, v, out);
    plog("GetExecutionProviderApi %s v%u -> %s", name, v, st ? "FAILED" : "ok");
    return st;
}

static const OrtApi *ORT_API_CALL p_GetApi(uint32_t version) {
    char m[64]; caller(__builtin_return_address(0), m, sizeof m);
    static volatile LONG n;
    LONG c = InterlockedIncrement(&n);
    if (c <= 10) plog("GetApi(%u) call %ld from %s lock=%d", version, c, m, owns_loader_lock());
    const OrtApi *r = real_base ? real_base->GetApi(version) : NULL;
    if (!r) { plog("GetApi(%u) -> NULL", version); return NULL; }
    AcquireSRWLockExclusive(&lk);
    if (!R) {
        R = r;
        memcpy(&api, r, sizeof api);
        api.CreateSession = h_CreateSession;
        api.CreateSessionFromArray = h_CreateSessionFromArray;
        api.ReleaseSession = h_ReleaseSession;
        api.Run = h_Run;
        api.RunWithBinding = h_RunWithBinding;
        api.CreateIoBinding = h_CreateIoBinding;
        api.BindInput = h_BindInput;
        api.BindOutput = h_BindOutput;
        api.BindOutputToDevice = h_BindOutputToDevice;
        api.CreateEnv = h_CreateEnv;
        api.CreateEnvWithGlobalThreadPools = h_CreateEnvWithGlobalThreadPools;
        api.CreateSessionOptions = h_CreateSessionOptions;
        api.CloneSessionOptions = h_CloneSessionOptions;
        api.SetIntraOpNumThreads = h_SetIntraOpNumThreads;
        api.SetInterOpNumThreads = h_SetInterOpNumThreads;
        api.SetSessionExecutionMode = h_SetSessionExecutionMode;
        api.EnableMemPattern = h_EnableMemPattern;
        api.DisableMemPattern = h_DisableMemPattern;
        api.EnableCpuMemArena = h_EnableCpuMemArena;
        api.DisableCpuMemArena = h_DisableCpuMemArena;
        api.SetSessionGraphOptimizationLevel = h_SetGOL;
        api.AddSessionConfigEntry = h_AddSessionConfigEntry;
        api.SessionOptionsAppendExecutionProvider = h_AppendEP;
        api.GetExecutionProviderApi = h_GetEPApi;
    }
    ReleaseSRWLockExclusive(&lk);
    return r == R ? &api : r;
}
static const char *ORT_API_CALL p_Version(void) { return real_base ? real_base->GetVersionString() : ""; }
static OrtApiBase p_base = {p_GetApi, p_Version};

/* ordinal 1 */
const OrtApiBase *ORT_API_CALL OrtGetApiBase(void) {
    static volatile LONG n;
    LONG c = InterlockedIncrement(&n);
    char m[64]; caller(__builtin_return_address(0), m, sizeof m);
    if (c <= 10) plog("OrtGetApiBase call %ld from %s lock=%d (DllMain read lock=%d)", c, m, owns_loader_lock(), dllmain_lock);
    start();
    if (!real_base) return NULL;
    return strncmp(real_base->GetVersionString(), "1.18.", 5) == 0 ? &p_base : real_base;
}

typedef OrtStatus *(ORT_API_CALL *ep_int)(OrtSessionOptions *, int);
typedef OrtStatus *(ORT_API_CALL *ep_ex)(OrtSessionOptions *, void *, void *);

OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProviderEx_DML(OrtSessionOptions *o, void *dml, void *queue) {
    start();
    ep_ex f = (ep_ex)GetProcAddress(real, "OrtSessionOptionsAppendExecutionProviderEx_DML");
    OrtStatus *st = f ? f(o, dml, queue) : NULL;
    plog("ordinal 2 AppendExecutionProviderEx_DML(%p) -> %s", (void *)o, f ? (st ? "FAILED" : "ok") : "MISSING");
    return st;
}
OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_CPU(OrtSessionOptions *o, int arena) {
    start();
    ep_int f = (ep_int)GetProcAddress(real, "OrtSessionOptionsAppendExecutionProvider_CPU");
    OrtStatus *st = f ? f(o, arena) : NULL;
    plog("ordinal 3 AppendExecutionProvider_CPU(%p, arena %d) -> %s", (void *)o, arena, f ? (st ? "FAILED" : "ok") : "MISSING");
    return st;
}
OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(OrtSessionOptions *o, int device) {
    start();
    ep_int f = (ep_int)GetProcAddress(real, "OrtSessionOptionsAppendExecutionProvider_DML");
    OrtStatus *st = f ? f(o, device) : NULL;
    plog("ordinal 4 AppendExecutionProvider_DML(%p, device %d) -> %s", (void *)o, device, f ? (st ? "FAILED" : "ok") : "MISSING");
    return st;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)r;
    if (why == DLL_PROCESS_ATTACH) {
        plog_path = L"C:\\rbtest\\ortprobe.log";
        dllmain_lock = owns_loader_lock();
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
