/* bridge/win/ort_win.h on the Mac: the wide model path, the version check and the CPU-tensor check
 * (bridge.md §1, §3 DirectML 1), the latter against a fake OrtApi that answers what the test
 * sets. Built and run by bridge/win/tests/run.sh. */
#include "../ort_win.h"

#include <stdio.h>

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* the fake library: GetTensorMemoryInfo fails, gives NULL, or an info whose device is `dev` */
static struct OrtStatus { int x; } fail_status;
static int mode, dev, released;
static OrtMemoryInfo *const fake_info = (OrtMemoryInfo *)(uintptr_t)0x1000;
static OrtStatus *ORT_API_CALL f_GetTensorMemoryInfo(const OrtValue *v, const OrtMemoryInfo **mi) {
    (void)v;
    if (mode == 0) return &fail_status;
    *mi = mode == 1 ? NULL : fake_info;
    return NULL;
}
static void ORT_API_CALL f_MemoryInfoGetDeviceType(const OrtMemoryInfo *mi, OrtMemoryInfoDeviceType *out) {
    if (mi == fake_info && dev >= 0) *out = (OrtMemoryInfoDeviceType)dev;
}
static void ORT_API_CALL f_ReleaseStatus(OrtStatus *s) { released += s == &fail_status; }

int main(void) {
    static const uint16_t yes1[] = u"C:/Users/x/AppData/Roaming/Pioneer/rekordbox6/models/hdemucs.onnx",
                          yes2[] = u"C:\\rb\\hdemucs.onnx", no1[] = u"C:\\rb\\xhdemucs.onnx", no2[] = u"hdemucs.onnx",
                          no3[] = u"C:\\rb\\hdemucs.onnx.bak", no4[] = u"C:\\rb\\HDEMUCS.ONNX", no5[] = u"/hdemucs.onn",
                          yes3[] = u"/hdemucs.onnx", no6[] = u"";
    CHECK(ortw_is_demucs16(yes1) && ortw_is_demucs16(yes2) && ortw_is_demucs16(yes3), "Demucs paths");
    CHECK(!ortw_is_demucs16(no1) && !ortw_is_demucs16(no2) && !ortw_is_demucs16(no3) && !ortw_is_demucs16(no4)
              && !ortw_is_demucs16(no5) && !ortw_is_demucs16(no6) && !ortw_is_demucs16(NULL), "not Demucs");
    CHECK(ortw_version_ok("1.18.0") && ortw_version_ok("1.18.1") && !ortw_version_ok("1.17.0") && !ortw_version_ok("1.180")
              && !ortw_version_ok(NULL) && !ortw_version_ok(""), "versions");

    OrtApi R;
    memset(&R, 0, sizeof R);
    R.GetTensorMemoryInfo = f_GetTensorMemoryInfo;
    R.MemoryInfoGetDeviceType = f_MemoryInfoGetDeviceType;
    R.ReleaseStatus = f_ReleaseStatus;
    const OrtValue *v = (const OrtValue *)(uintptr_t)0x2000;
    mode = 0;
    CHECK(!ortw_on_cpu(&R, v) && released == 1, "a failed query: not CPU, its status released");
    mode = 1;
    CHECK(!ortw_on_cpu(&R, v), "no info: not CPU");
    mode = 2;
    dev = OrtMemoryInfoDeviceType_CPU;
    CHECK(ortw_on_cpu(&R, v), "CPU");
    dev = OrtMemoryInfoDeviceType_GPU;
    CHECK(!ortw_on_cpu(&R, v), "GPU (DirectML)");
    dev = -1;
    CHECK(!ortw_on_cpu(&R, v), "an answer that writes nothing: not CPU");
    const OrtValue *vs[3] = {v, NULL, v};
    dev = OrtMemoryInfoDeviceType_CPU;
    CHECK(ortw_all_on_cpu(&R, vs, 3), "all on the CPU, a NULL output skipped");
    dev = OrtMemoryInfoDeviceType_FPGA;
    CHECK(!ortw_all_on_cpu(&R, vs, 3), "one not on the CPU");
    CHECK(ortw_all_on_cpu(&R, NULL, 2), "no outputs given");
    printf("ort_win: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
