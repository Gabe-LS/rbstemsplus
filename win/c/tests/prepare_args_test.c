#include "check.h"
#include "compat.h"
#include "helper_proc.h"

int checks, failures;

/* Simulate the worker snapshot and detect any payload read during argument preparation. */
typedef struct {
    int proved;
    int payload_reads;
} prepare_fixture;

typedef struct {
    prepare_fixture *steps;
    const char *ort;
    void *c;
} nw;

static const char folder[] = "C:\\Program Files\\rekordbox\\rekordbox 7.2.19";
static int nw_steps_rekordbox_known(prepare_fixture *f, char *out, size_t n, int *proved) {
    snprintf(out, n, "%s", folder);
    *proved = f->proved;
    return 1;
}

static int nw_steps_prepare_payload(prepare_fixture *f, payload_manifest *out) {
    (void)out;
    f->payload_reads++;
    return 0;
}

static void native_core_log(void *ctx, const char *line) {
    (void)ctx;
    (void)line;
}

#include "prepare_args.inc"

static void test_args(void) {
    (void)nw_steps_prepare_payload;   /* used if the old payload read is put back */
    prepare_fixture f = {.proved = 1};
    nw n = {&f, "C:\\Program Files\\RB Stems Plus\\ort\\onnxruntime.dll", NULL};
    const char *const argv[] = {"--ort", "ignored", "--rekordbox", "ignored", "--keep", "Mix", "track.wav"};
    enum { ARGC = sizeof argv / sizeof *argv };
    const char *out[ARGC + 1];
    char rb[COMPAT_PATH_MAX] = "";
    int count = nw_prepare_args(&n, argv, ARGC, 1, rb, out);
    CHECK(count == ARGC + 1 && !strcmp(out[0], "--resample-proved") && out[2] == n.ort &&
          !strcmp(out[4], folder) && out[7] == argv[6], "proved bit puts the flag first and passes the verified folder");
    f.proved = 0;
    count = nw_prepare_args(&n, argv, ARGC, 1, rb, out);
    CHECK(count == ARGC && !strcmp(out[0], "--ort") && !strcmp(out[3], folder), "clear bit leaves the flag out");
    CHECK(f.payload_reads == 0, "spawn does no payload reads");
}

static void test_proved_pair(void) {
    static payload_manifest m;
    const char *version = "7.2.19.0342";
    char sha[65] = {0};
    memset(sha, '7', 64);
    m.has_windows = 1;
    m.n_resampler_proved = 1;
    strcpy(m.resampler_proved[0].product_version, version);
    strcpy(m.resampler_proved[0].rekordbox_sha256, sha);
    CHECK(compat_resample_proved(&m, version, sha), "listed ProductVersion and hash");
    CHECK(!compat_resample_proved(&m, "7.2.19.0343", sha), "unlisted ProductVersion with the same hash");
    memset(sha, '8', 64);
    CHECK(!compat_resample_proved(&m, version, sha), "listed ProductVersion with a mismatched hash");
    m.n_resampler_proved = 0;
    CHECK(!compat_resample_proved(&m, version, m.resampler_proved[0].rekordbox_sha256), "empty resampler_proved list");
}

int main(void) {
    test_args();
    test_proved_pair();
    printf("Prepare args: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
