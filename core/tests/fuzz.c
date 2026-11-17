/* Fuzz targets for what the core reads from outside (mac-core §7): event JSON, the start config,
 * helper lines (the helper runs Pioneer's code: its output is untrusted; the pure parsers and the
 * core's path), text slots with any bytes, report masking, and the state files (the busy marker,
 * the bridge's state, the dismissed keys, launchctl's output). Config and kept sets come with
 * common/ (bridge T1a).
 *
 * Built by core/build.sh fuzz with ASan and UBSan and CORE_WIN_POLICY (both text columns), in the
 * bridge's style: Apple's clang has no libFuzzer, so main() is a small mutation fuzzer from built-in
 * seeds; with a clang that has libFuzzer (-DUSE_LIBFUZZER -fsanitize=fuzzer),
 * LLVMFuzzerTestOneInput takes over and the first byte picks the target.
 *
 *   core-fuzz [ITERATIONS [SEED]]   default 20000 per seed; exits nonzero on an invariant failure
 */
#include "../internal.h"
#include "dfuzz.h"
#include <stdio.h>
#include <time.h>

static void die(const char *what) {
    fputs("FAIL ", stderr);
    fputs(what, stderr);
    fputs("\n", stderr);
    abort();
}

/* ---------------------------------------------------------------- a minimal host: main jobs queued, run after each input */

typedef struct { core_job *jobs[4096]; int n; } fhost;

static void fh_post_main(void *ctx, core_job *job) {
    fhost *h = ctx;
    if (h->n < 4096) h->jobs[h->n++] = job;
    else core_job_drop(job);
}
static void fh_post_work(void *ctx, core_work kind, core_job *job) { (void)ctx; (void)kind; core_job_drop(job); }
static void fh_render(void *ctx, core_snap *s, const core_delta *d) { (void)ctx; (void)d; core_snap_release(s); }
static uint32_t fh_front(void *ctx) { (void)ctx; return 0; }
static int32_t fh_spawn(void *ctx, uint64_t run, const char *const *argv, int32_t argc) { (void)ctx; (void)run; (void)argv; (void)argc; return 0; }
static int32_t fh_modal(void *ctx) { (void)ctx; return 0; }
static void fh_facts(void *ctx, core_facts *out) { (void)ctx; core_fact_flag(out, FACT_STEMS_PLUS_PRESENT, 1); }
static int32_t fh_dialog(void *ctx, const core_dialog *d, core_reply *r) { (void)ctx; (void)d; (void)r; return 1; }
static void fh_close(void *ctx, core_close w, int32_t p) { (void)ctx; (void)w; (void)p; }
static void fh_log(void *ctx, const char *l, size_t n) { (void)ctx; (void)l; (void)n; }

static void fh_pump(fhost *h) {
    while (h->n) {
        int n = h->n;
        core_job *batch[4096];
        memcpy(batch, h->jobs, (size_t)n * sizeof *batch);
        h->n = 0;
        for (int i = 0; i < n; i++) core_run(batch[i]);
    }
}

static core_host fh_table(fhost *h) {
    core_host t;
    memset(&t, 0, sizeof t);
    t.size = sizeof t;
    t.ctx = h;
    t.render = fh_render;
    t.front_facts = fh_front;
    t.helper_spawn = fh_spawn;
    t.modal_open = fh_modal;
    t.facts = fh_facts;
    t.post_main = fh_post_main;
    t.post_work = fh_post_work;
    t.dialog = fh_dialog;
    t.close_reply = fh_close;
    t.log = fh_log;
    return t;
}

static fhost the_host;   /* the fuzz driver's own state (a test program, not the core) */
static core *the_core;
static uint64_t the_run;

/* ---------------------------------------------------------------- targets */

static size_t check_arr_max = 256;   /* the array limit the parse ran with */
static void check_strings(const jval *v) {
    if (v->type == J_STR && (strlen(v->s) != v->len || !utf8_valid(v->s, v->len))) die("json: a string with a NUL or not UTF-8");
    if (v->type == J_OBJ || v->type == J_ARR) {
        if (v->n > (v->type == J_OBJ ? 256 : check_arr_max)) die("json: too many members");
        for (size_t i = 0; i < v->n; i++) {
            if (v->type == J_OBJ && (strlen(v->keys[i]) != v->key_lens[i] || !utf8_valid(v->keys[i], v->key_lens[i]))) die("json: a bad key");
            check_strings(&v->items[i]);
        }
    }
}

static void t_json(const uint8_t *d, size_t n) {
    arena a = {0};
    jval v;
    check_arr_max = 256;
    if (json_parse(&a, (const char *)d, n, &v) == CORE_OK) check_strings(&v);
    arena_free(&a);
    /* json_parse_lim: the event limits, and kept.noticed's (32 MiB, 100000 array members) */
    a = (arena){0};
    if (json_parse_lim(&a, (const char *)d, n, &(json_lim){64 * 1024, 256, SIZE_MAX, 16}, &v) == CORE_OK) check_strings(&v);
    arena_free(&a);
    a = (arena){0};
    check_arr_max = 100000;
    if (json_parse_lim(&a, (const char *)d, n, &(json_lim){SIZE_MAX, 100000, 500257, 2}, &v) == CORE_OK) check_strings(&v);
    check_arr_max = 256;
    arena_free(&a);
}

static void t_event(const uint8_t *d, size_t n) {
    int r = core_event(the_core, (const char *)d, n);
    if (r != CORE_OK && r != CORE_E_PARSE && r != CORE_E_UNKNOWN && r != CORE_E_ARG) die("event: an unexpected result");
    if (the_core->scratch.used_total) die("event: the scratch arena isn't reset");
    fh_pump(&the_host);
}

static void t_config(const uint8_t *d, size_t n) {
    core_host t = fh_table(&the_host);
    core *c = core_new(&t, CORE_ROLE_APP, (const char *)d, n);
    if (c && c->version && !utf8_valid(c->version, strlen(c->version))) die("config: a version that isn't UTF-8");
    core_free(c);
    fh_pump(&the_host);
}

static void t_helper(const uint8_t *d, size_t n) {
    core_helper_line(the_core, the_run, (const char *)d, n);
    fh_pump(&the_host);
}

static void t_dfuzz(const uint8_t *d, size_t n) {
    if (dfuzz_run(d, n)) die("dfuzz: the model, its deltas or a held snapshot disagree with the mirror");
}

/* text slots with any bytes: the first two bytes pick the text and the OS, the rest are the values
 * of the slots it uses, split at 0xFF */
static void t_text(const uint8_t *d, size_t n) {
    if (n < 2) return;
    core_text_id id = (core_text_id)(d[0] % CORE_TEXT_COUNT);
    core_os os = (d[1] & 1) ? CORE_OS_WIN : CORE_OS_MAC;
    core_slot s[CORE_SLOT_COUNT];
    int ns = 0;
    const uint8_t *p = d + 2, *e = d + n;
    for (int k = 0; k < CORE_SLOT_COUNT && p <= e; k++) {
        if (!(d[1] >> (k + 1) & 1)) continue;
        const uint8_t *q = memchr(p, 0xFF, (size_t)(e - p));
        if (!q) q = e;
        s[ns++] = (core_slot){k, 0, (const char *)p, (size_t)(q - p)};
        p = q + 1;
    }
    size_t full = 0, again = 0;
    int r = core_text(os, id, s, ns, NULL, 0, &full);
    if (r != CORE_E_SPACE && r != CORE_E_ARG && r != CORE_E_NOTYET) die("text: an unexpected result");
    if (r != CORE_E_SPACE) return;
    char *b = malloc(full + 1), small[9];
    if (core_text(os, id, s, ns, b, full + 1, &again) != CORE_OK || again != full) die("text: two lengths for one text");
    for (int i = 0; i < ns; i++)   /* every value is there as it was */
        if (s[i].len && !memmem(b, full, s[i].value, s[i].len)) die("text: a slot value is missing");
    if (core_text(os, id, s, ns, small, sizeof small, &again) != CORE_E_SPACE && full >= sizeof small) die("text: no CORE_E_SPACE");
    if (memcmp(small, b, strlen(small))) die("text: a cut result isn't a prefix");
    free(b);
}

static void check_pstr(pstr s, const char *what) {
    if (!s.s || s.s[s.n] != 0 || !utf8_valid(s.s, s.n)) die(what);
}

/* the helper's lines, through every pure parser (helper_lines.c): strings come out valid UTF-8 */
static prep_estimate fuzz_est;
static arena fuzz_est_arena;
static void t_lines(const uint8_t *d, size_t n) {
    arena a = {0};
    prep_run_line l;
    int r = prep_run_line_parse(&a, (const char *)d, n, &l);
    if (r < -1 || r > 1) die("lines: an unexpected result");
    if (r == 1 && l.kind == RUN_TRACK) {
        check_pstr(l.track.file, "lines: a track's file");
        check_pstr(l.track.why, "lines: a track's why");
        pstr name = prep_last_path_component(l.track.file);
        if (name.s < l.track.file.s || name.s + name.n > l.track.file.s + l.track.file.n) die("lines: a name outside its path");
    }
    if (r == 1 && l.kind == RUN_DONE && ((l.has_stopped && !utf8_valid(l.stopped.s, l.stopped.n)) || (l.has_error && !utf8_valid(l.error.s, l.error.n))))
        die("lines: done's strings");
    prep_playlist p;
    if (prep_playlist_parse(&a, (const char *)d, n, &p) == 1) { check_pstr(p.id, "lines: an id"); check_pstr(p.name, "lines: a name"); }
    pstr f;
    if (prep_track_list_parse(&a, (const char *)d, n, &f) == 1) check_pstr(f, "lines: a listed file");
    /* one estimate over the run's inputs: its tracks and folders stay sound */
    if (fuzz_est.ntracks > 4096 || fuzz_est.done) { arena_reset(&fuzz_est_arena); prep_estimate_init(&fuzz_est); }
    if (prep_estimate_line(&fuzz_est_arena, &fuzz_est, (const char *)d, n) < 0) die("lines: out of memory");
    for (int32_t i = 0; i < fuzz_est.nfolders; i++) check_pstr(fuzz_est.folders[i], "lines: a folder");
    char b[512];
    tbuf t;
    tb_init(&t, CORE_OS_MAC, b, sizeof b);
    w_estimate_summary(&t, &fuzz_est);
    w_set_state(&t, &fuzz_est);
    if (t.err && t.err != CORE_E_SPACE) die("lines: the estimate's words");
    arena_free(&a);
}

/* report masking (mask.c): OK, the same text with a buffer of any size, a cut one a prefix */
static void t_mask(const uint8_t *d, size_t n) {
    static const char *const names[] = {"DJ Smith", "dj", "Jos\xc3\xa9", "Anna Stra\xc3\x9f\x65", "\xc4\xb0x"}, *const keep[] = {"deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"};
    core_mask_ctx mc = {sizeof mc, 0, "/Users/dj", names, 5, 1, keep};
    size_t full = 0, again = 0;
    int32_t r = core_mask(&mc, (const char *)d, n, NULL, 0, &full);
    if (r != CORE_E_SPACE) die("mask: no CORE_E_SPACE");
    char *b = malloc(full + 1), small[17];
    if (core_mask(&mc, (const char *)d, n, b, full + 1, &again) != CORE_OK || again != full) die("mask: two lengths");
    if (core_mask(&mc, (const char *)d, n, small, sizeof small, &again) != (full < sizeof small ? CORE_OK : CORE_E_SPACE)) die("mask: a cut result");
    if (memcmp(small, b, strlen(small))) die("mask: a cut result isn't a prefix");
    if (utf8_valid((const char *)d, n) && !utf8_valid(b, full)) die("mask: UTF-8 in, not UTF-8 out");
    free(b);
}

/* the state files (state.c, watcher.c) */
static void t_state(const uint8_t *d, size_t n) {
    arena a = {0};
    busy_marker m;
    if (busy_marker_parse(&a, (const char *)d, n, &m) == 1) {
        if (m.pid <= 0) die("state: a marker's pid");
        if (m.has_action) { check_pstr(m.id, "state: a marker's id"); check_pstr(m.label, "state: a marker's label"); }
    }
    bridge_state b;
    if (bridge_state_parse(&a, (const char *)d, n, &b) == 1) {
        if (b.pid <= 0) die("state: the bridge's pid");
        check_pstr(b.location, "state: the bridge's location");
        check_pstr(b.reason, "state: the bridge's reason");
    }
    keyset k;
    if (keyset_parse(&a, (const char *)d, n, &k)) die("state: out of memory");
    for (int32_t i = 1; i < k.n; i++) {
        size_t m2 = k.keys[i - 1].n < k.keys[i].n ? k.keys[i - 1].n : k.keys[i].n;
        int c = memcmp(k.keys[i - 1].s, k.keys[i].s, m2);
        if (c > 0 || (!c && k.keys[i - 1].n >= k.keys[i].n)) die("state: the keys aren't sorted and unique");
    }
    int32_t model, cache;
    state_chosen((const char *)d, n, &model, &cache);
    (void)watcher_launchd_disabled((const char *)d, n, "com.gabels.rbstemsplus.watcher");
    arena_free(&a);
}

#ifdef USE_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (!the_core) {
        core_host t = fh_table(&the_host);
        the_core = core_new(&t, CORE_ROLE_APP, "{\"os\":\"mac\"}", CORE_NTS);
        the_run = core_test_helper_start(the_core, 0);
    }
    if (!n) return 0;
    switch (d[0] % 9) {
    case 8: t_state(d + 1, n - 1); break;
    case 7: t_mask(d + 1, n - 1); break;
    case 6: t_lines(d + 1, n - 1); break;
    case 5: t_dfuzz(d + 1, n - 1); break;
    case 0: t_json(d + 1, n - 1); break;
    case 1: t_event(d + 1, n - 1); break;
    case 2: t_config(d + 1, n - 1); break;
    case 3: t_helper(d + 1, n - 1); break;
    default: t_text(d + 1, n - 1); break;
    }
    return 0;
}
#else
static uint64_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

/* one mutation of seed into out (at most cap bytes): flips, interesting bytes, inserts, deletes,
 * truncation, duplication of a slice (the bridge's fuzz.c) */
static size_t mutate(const uint8_t *seed, size_t n, uint8_t *out, size_t cap) {
    if (n > cap) n = cap;
    memcpy(out, seed, n);
    static const uint8_t special[] = {0, 0xFF, 0x7F, 0x80, '"', '\\', '{', '}', '[', ']', ':', ',', 'u', '0', '-', 'e', '.', 0xEF, 0xC0, 0xED, 0xF4};
    for (int k = 1 + (int)(rnd() % 8); k > 0; k--) {
        size_t at = n ? rnd() % n : 0;
        switch (rnd() % 7) {
        case 0: if (n) out[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: if (n) out[at] = special[rnd() % sizeof special]; break;
        case 2: if (n) out[at] = (uint8_t)rnd(); break;
        case 3: if (n < cap) { memmove(out + at + 1, out + at, n - at); out[at] = (uint8_t)rnd(); n++; } break;
        case 4: if (n) { memmove(out + at, out + at + 1, n - at - 1); n--; } break;
        case 5: n = at; break;
        default: {
            size_t len = n ? 1 + rnd() % (n < 64 ? n : 64) : 0, from = n > len ? rnd() % (n - len) : 0;
            if (len && n + len <= cap) { memmove(out + at + len, out + at, n - at); memmove(out + at, out + from + (from >= at ? len : 0), len); n += len; }
        }
        }
    }
    return n;
}

/* every input within a time budget (C29): the C14 kind of bug (seconds on main) fails here. The
 * budget is the input's own CPU time (this thread's: the targets and the main jobs after them run
 * here), not the wall clock, so a loaded machine (other lanes' builds) doesn't fail an input that a
 * rerun passes, while a quadratic blow-up still does. Both fuzzers (mac and win) run on the Mac. */
enum { BUDGET_MS = 250 };   /* generous: the sanitizer builds are several times slower */

static double now_ms(void) { return (double)plat_now_mono_ns() / 1e6; }
static double cpu_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) die("no thread CPU clock");
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

typedef struct { double cpu, wall; } took;
typedef void (*target)(const uint8_t *, size_t);

static took timed(target f, const uint8_t *in, size_t n) {
    double w0 = now_ms(), c0 = cpu_ms();
    f(in, n);
    took t = {cpu_ms() - c0, now_ms() - w0};
    return t;
}
/* over the budget: both times printed (the wall time alone says nothing on a loaded machine) */
static int over_budget(const char *name, took t) {
    if (t.cpu <= BUDGET_MS) return 0;
    fprintf(stderr, "over budget: %s: %.2f ms of CPU (%.2f ms wall), the budget is %d ms of CPU\n", name, t.cpu, t.wall, (int)BUDGET_MS);
    return 1;
}

static void run(const char *name, target f, const char *seed, size_t n, long iters) {
    size_t cap = n * 2 + 1024;
    uint8_t *buf = malloc(cap);
    took worst = {0, 0};
    f((const uint8_t *)seed, n);
    for (long i = 0; i < iters; i++) {
        size_t m = mutate((const uint8_t *)seed, n, buf, cap);
        took t = timed(f, buf, m);
        if (t.cpu > worst.cpu) worst.cpu = t.cpu;
        if (t.wall > worst.wall) worst.wall = t.wall;
        if (over_budget(name, t)) die("an input took longer than its time budget");
    }
    free(buf);
    printf("ok   %s: %ld mutations of a %zu-byte seed (slowest %.2f ms of CPU, %.2f ms wall)\n", name, iters, n, worst.cpu, worst.wall);
}

/* the budget's self-test: an input that keeps the CPU busy past the budget fails, one that sleeps
 * as long (the wall clock past the budget, no CPU: a loaded machine's case) doesn't */
static void busy_input(const uint8_t *in, size_t n) {
    (void)in, (void)n;
    double c0 = cpu_ms();
    volatile uint64_t spin = 0;
    while (cpu_ms() - c0 < BUDGET_MS * 1.5) spin++;
}
static void sleeping_input(const uint8_t *in, size_t n) {
    (void)in, (void)n;
    struct timespec ts = {0, (long)BUDGET_MS * 1500000L}, left;
    while (nanosleep(&ts, &left) != 0) ts = left;
}
static void budget_selftest(void) {
    took busy = timed(busy_input, NULL, 0);
    if (!over_budget("self-test: a busy loop (expected)", busy)) die("budget self-test: a busy input wasn't over the budget");
    took sleeping = timed(sleeping_input, NULL, 0);
    if (sleeping.wall <= BUDGET_MS) die("budget self-test: the sleeping input didn't sleep past the budget");
    if (over_budget("self-test: a sleep", sleeping)) die("budget self-test: a sleeping input was over the budget");
    printf("ok   budget self-test: a busy loop fails (%.0f ms of CPU), a sleep doesn't (%.0f ms wall, %.2f ms of CPU)\n", busy.cpu, sleeping.wall, sleeping.cpu);
}

#define SEED(f, name, s) run(name, f, s, sizeof s - 1, iters)

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    rng = argc > 2 ? strtoull(argv[2], NULL, 0) : (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL;
    if (!rng) rng = 1;
    printf("fuzz: seed %llu (core-fuzz %ld %llu repeats this run)\n", (unsigned long long)rng, iters, (unsigned long long)rng);
    budget_selftest();
    core_host t = fh_table(&the_host);
    the_core = core_new(&t, CORE_ROLE_APP, "{\"os\":\"mac\"}", CORE_NTS);
    if (!the_core) die("can't make a core");
    the_run = core_test_helper_start(the_core, 0);
    if (!the_run) die("can't start a helper run");

    SEED(t_json, "json", "{\"a\":[1,-2.5e3,{\"b\":null,\"c\":true}],\"d\":\"\\u00e9\\ud83d\\ude00\\n\\\"\",\"e\":false}");
    SEED(t_json, "json", "[\"x\",[],{},0,-0,1E+2,\"\\/\\b\\f\\r\\t\"]");
    SEED(t_event, "event", "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    SEED(t_event, "event", "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":12,\"gen\":3}");
    SEED(t_event, "event", "{\"ev\":\"activated\"}");
    SEED(t_event, "event", "{\"ev\":\"settings\",\"key\":\"max_gb\",\"value\":\"3\"}");
    SEED(t_config, "config", "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"/Users/x\",\"support\":\"C:\\\\Users\\\\x\"}}");
    SEED(t_helper, "helper", "{\"kind\":\"track\",\"id\":\"123\",\"name\":\"Track \\u00e9\",\"result\":\"prepare\",\"seconds\":312}");
    SEED(t_text, "text", "\x0b\x0c" "GLS\xff" "20");
    SEED(t_text, "text", "\x0a\x08" "{version}\xc3\xa9%s%n");
    SEED(t_text, "text", "\x00\x01" "x");
    SEED(t_config, "config", "{\"os\":\"mac\",\"version\":\"1.4.0\",\"paths\":{\"home\":\"/Users/x\"}}");
    prep_estimate_init(&fuzz_est);
    SEED(t_lines, "lines", "{\"file\":\"/Users/dj/Music/a.mp3\",\"result\":\"prepared\",\"written\":12,\"already\":3,\"silent\":0,\"refused\":1,\"how\":\"x\"}");
    SEED(t_lines, "lines", "{\"estimate\":2,\"ready\":1,\"prepare\":1,\"time_s\":55,\"need_gb\":0.061,\"fits\":true,\"folders\":[\"Documents\",\"Desktop\"],}");
    SEED(t_lines, "lines", "{\"done\":3,\"stopped\":\"stopped\",\"kept_gb\":2.125,\"manifest_error\":\"\\u00e9\\ud83d\\ude00\"}");
    SEED(t_lines, "lines", "{\"at\":0,\"files\":3,\"chunk\":1,\"chunks\":2e3,\"x\":[[[{\"a\":[1,{}]}]]],\"y\":-0.0e-5}");
    SEED(t_lines, "lines", "{\"playlist\":\"12\",\"parent\":\"0\",\"name\":\"Fri\\\"day\",\"kind\":\"playlist\",\"tracks\":18446744073709551616}");
    SEED(t_mask, "mask", "password=hunter2 Authorization: Bearer abcdefghijkl me@example.com ?token=x&b=2 /Users/dj DJ Smith");
    SEED(t_mask, "mask", "-----BEGIN RSA PRIVATE KEY-----\nMIIE\n-----END RSA PRIVATE KEY----- eyJabcd.efgh.ij AKIAIOSFODNN7EXAMPLE");
    SEED(t_mask, "mask", "ghp_aB3dE5gH7jK9mN1pQ3sT5vX7zB9dF1hJ xoxb-1234567890 sk-abcdefghijklmnopqrstu caf\xc3\xa9 \xff\xfe pa\xc5\xbfsword=1");
    /* outside ASCII: ß and ẞ in keywords and names, a combining mark, ZWSP, Ethiopic, ﬆ, İ, Kelvin */
    SEED(t_mask, "mask", "pa\xc3\x9fwd=1 ACCE\xe1\xba\x9e_KEY: v ?se\xc3\x9fion=2 \xcc\x81password=x x\xe2\x80\x8btoken=y \xe1\x8d\xa2pwd=z "
                         "ANNA STRASSE \xef\xac\x86 i\xcc\x87x /Users/\xe2\x84\xaaim AKIAIOSFODNN7EXAMPLE\xcc\x81");
    SEED(t_state, "state", "42\n1700000000\ninstall\nInstall Stems Cache\n");
    SEED(t_state, "state", "pid=1\nstart=2\nstate=pending\nlocation=/Volumes/GLS\nreason=not-connected\n");
    SEED(t_state, "state", "cache:7.2.19\nmodel:0002\n\"com.gabels.rbstemsplus.watcher\" => disabled\nmodel=1\r\n");
    SEED(t_dfuzz, "dfuzz", "\x03\x10\x00\x81\x05\x02\x08\x00\x06\x01\x01\x00\x04\x83\x07\x05\x09\x01\x0b\x00\x03\x05\x05\x03\x0a\x00\x08\x02");
    {   /* long inputs near the 64 KiB limit: numbers, mutated (the short seeds stay short) */
        static char big[60000];
        size_t k = 0;
        big[k++] = '[';
        while (k + 40 < sizeof big) {
            memcpy(big + k, "12345678901234567890123.5e-321,0e99999,", 39);
            k += 39;
        }
        big[k++] = '0';
        big[k++] = ']';
        run("json (60 KB)", t_json, big, k, iters / 20 + 1);
        /* masking a long log: every pattern's long runs at once, within the budget (linear) */
        static const char piece[] = "password-password-pwd ?a;b;c;d eyJaaaa.bbbb AKIA a@b@c-d.e ---- ";
        for (k = 0; k + sizeof piece < sizeof big; k += sizeof piece - 1) memcpy(big + k, piece, sizeof piece - 1);
        run("mask (60 KB)", t_mask, big, k, iters / 200 + 1);
        memset(big, 'a', sizeof big);
        run("mask (60 KB, one run)", t_mask, big, sizeof big, iters / 200 + 1);
        for (size_t i = 0; i < sizeof big; i++) big[i] = i & 1 ? 'a' : '-';
        run("mask (60 KB of -a)", t_mask, big, sizeof big, iters / 200 + 1);
        /* \b looking back through 60 KB of combining marks; keywords with ß ending past a run */
        for (k = 0; k + 2 <= sizeof big; k += 2) memcpy(big + k, "\xcc\x81", 2);
        run("mask (60 KB of U+0301)", t_mask, big, k, iters / 200 + 1);
        static const char piece2[] = "xpa\xc3\x9fwd_aaaaaaaaaaaaaaaa \xcc\x81\xcc\x81se\xc3\x9fion ";
        for (k = 0; k + sizeof piece2 < sizeof big; k += sizeof piece2 - 1) memcpy(big + k, piece2, sizeof piece2 - 1);
        run("mask (60 KB of pa\xc3\x9fwd)", t_mask, big, k, iters / 200 + 1);
        /* keywords packed in one run, no space: every hit's second run ends at the first's end */
        static const char *const dense[] = {"pwd", "_token", "acces\xc5\xbfkey"};
        for (size_t d = 0; d < sizeof dense / sizeof *dense; d++) {
            size_t dl = strlen(dense[d]);
            for (k = 0; k + dl <= sizeof big; k += dl) memcpy(big + k, dense[d], dl);
            run("mask (60 KB, keywords in one run)", t_mask, big, k, iters / 200 + 1);
        }
        /* a helper line near the limit */
        memcpy(big, "{\"file\":\"", 9);
        for (k = 9; k + 12 < sizeof big; k += 9) memcpy(big + k, "\\u00e9a/b", 9);
        memcpy(big + k, "\"}", 2);
        run("lines (60 KB)", t_lines, big, k + 2, iters / 200 + 1);
    }
    arena_free(&fuzz_est_arena);
    core_helper_ended(the_core, the_run, 0, "", 0);
    core_free(the_core);
    fh_pump(&the_host);
    printf("fuzz: no failures\n");
    return 0;
}
#endif
