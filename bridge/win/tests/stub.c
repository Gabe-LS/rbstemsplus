/* The refusal stub's completeness check on the Mac (the private Windows plan's R1, bridge.md §3, J3):
 * bridge/win/stub_api.inc compiled against the Mac's view of the vendored 1.18 header.
 *  - every OrtApi slot is filled, with a function of exactly that slot's type (the designated
 *    initializers would not compile otherwise; here: the struct holds STUB_SLOTS pointers, none
 *    NULL, and every name is listed once);
 *  - every slot called through the typed struct does what R1 says: status slots return the one
 *    static ORT_FAIL status, GetErrorCode/GetErrorMessage read it, CreateStatus copies nothing,
 *    MemoryInfoGetDeviceType writes CPU, GetBuildInfoString a constant, GetTrainingApi NULL,
 *    Release* and ClearBound* nothing;
 *  - none of the calls allocates (the malloc zone's count of blocks in use is the same after).
 * Built and run by bridge/win/tests/run.sh. The same calls run in the VM on the built DLL
 * (stubcall.exe, bridge/win/build.sh --test). */
#include "onnxruntime_c_api.h"

#include <malloc/malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct OrtStatus { OrtErrorCode code; const char *msg; };
static OrtStatus stub_status = {ORT_FAIL, "rbstems bridge: ONNX Runtime unavailable (test)"};
#define STUB_SELFTEST 1
#include "../stub_api.inc"

_Static_assert(sizeof(OrtApi) == STUB_SLOTS * sizeof(void (*)(void)), "a slot of OrtApi isn't in the stub");

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

int main(void) {
    void (*slot[STUB_SLOTS])(void);
    memcpy(slot, &stub_api, sizeof slot);
    int null = 0;
    for (int i = 0; i < STUB_SLOTS; i++) null += slot[i] == NULL;
    CHECK(null == 0, "%d slots are NULL", null);
    for (int i = 0; i < STUB_SLOTS; i++)
        for (int j = i + 1; j < STUB_SLOTS; j++) CHECK(strcmp(stub_names[i], stub_names[j]), "%s listed twice", stub_names[i]);
    CHECK(STUB_SLOTS == 280, "ORT 1.18's OrtApi has 280 slots, the stub %d", STUB_SLOTS);

    int kinds[6] = {0};
    uintptr_t rets[STUB_SLOTS];
    int kind[STUB_SLOTS];
    stub_device = OrtMemoryInfoDeviceType_GPU;
    malloc_statistics_t before, after;
    malloc_zone_statistics(NULL, &before);
    for (int i = 0; i < STUB_SLOTS; i++) kind[i] = stub_call(&stub_api, i, &rets[i]);
    malloc_zone_statistics(NULL, &after);
    CHECK(before.blocks_in_use == after.blocks_in_use, "the stub allocated: %u blocks before, %u after", before.blocks_in_use,
          after.blocks_in_use);
    uintptr_t past;
    CHECK(stub_call(&stub_api, STUB_SLOTS, &past) == -1, "past the last slot");
    for (int i = 0; i < STUB_SLOTS; i++) {
        const char *n = stub_names[i];
        CHECK(kind[i] >= 0 && kind[i] <= STUB_VOID, "%s: kind %d", n, kind[i]);
        if (kind[i] < 0 || kind[i] > STUB_VOID) continue;
        kinds[kind[i]]++;
        switch (kind[i]) {
        case STUB_STATUS: CHECK(rets[i] == (uintptr_t)&stub_status, "%s: not the static status", n); break;
        case STUB_CODE: CHECK(rets[i] == (uintptr_t)ORT_FAIL, "%s: %lu", n, (unsigned long)rets[i]); break;
        case STUB_TEXT: CHECK(rets[i] && *(const char *)rets[i], "%s: empty", n); break;
        case STUB_DEVICE: CHECK(stub_device == OrtMemoryInfoDeviceType_CPU, "%s: device %d", n, (int)stub_device); break;
        case STUB_NULL: CHECK(rets[i] == 0 && !strcmp(n, "GetTrainingApi"), "%s: NULL expected", n); break;
        case STUB_VOID: CHECK(!strncmp(n, "Release", 7) || !strncmp(n, "ClearBound", 10), "%s: void", n); break;
        }
    }
    CHECK(stub_api.CreateStatus(ORT_INVALID_ARGUMENT, "x") == &stub_status && stub_status.code == ORT_FAIL, "CreateStatus copies nothing");
    CHECK(stub_api.GetErrorMessage(&stub_status) == stub_status.msg, "GetErrorMessage reads it");
    stub_api.ReleaseStatus(&stub_status);
    CHECK(stub_status.msg != NULL, "ReleaseStatus is a no-op on it");
    CHECK(kinds[STUB_CODE] == 1 && kinds[STUB_DEVICE] == 1 && kinds[STUB_NULL] == 1 && kinds[STUB_TEXT] == 2, "special slots");
    printf("stub: %d slots (%d status, %d void), %d checks, %d failed\n", STUB_SLOTS, kinds[STUB_STATUS], kinds[STUB_VOID], checks,
           failures);
    return failures ? 1 : 0;
}
