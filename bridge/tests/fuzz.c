/* Fuzz targets for what the bridge reads from files anyone who can write a drive may control:
 * config.ini (cfg_parse), a chosen location's marker (marker_ok), a kept set (parse_kept), an
 * entry's .desc (parse_desc, with and without a chosen location's caps) and an entry's FLAC
 * (decode_flac, from memory and through plat_file_open_at from a file).
 *
 * Built by bridge/build.sh --fuzz with AddressSanitizer and UndefinedBehaviorSanitizer, with the
 * bridge's own source included (its static functions are the targets). Apple's clang has no
 * libFuzzer, so main() below is a small mutation fuzzer: it starts from built-in seeds (a real
 * FLAC is encoded with the bridge's own write_flac) and runs N deterministic mutations per target.
 * With a clang that has libFuzzer (-DUSE_LIBFUZZER -fsanitize=fuzzer), LLVMFuzzerTestOneInput
 * takes over; the first byte picks the target.
 *
 *   fuzz [ITERATIONS]        default 20000 per target; exits 1 on an invariant failure
 */
#include "../rbstems_bridge.c"

static const char *FUZZ_ID = "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F";
static const char *FUZZ_KEY = "ab11111111111111111111111111111111111111111111111111111111111111";
#define FUZZ_FRAMES 4096
static char fuzz_dir[PATH_MAX];

static void die(const char *what) {
    fprintf(stderr, "FAIL %s\n", what);
    abort();
}

static void t_config(const uint8_t *d, size_t n) {
    config c = {.enabled = 1, .max_bytes = 20LL << 30, .max_days = 60};
    if (cfg_parse((const char *)d, n, &c, logf_) == 0) {
        if (c.location == 1 && (!paths_location_ok(c.dir) || !cfg_uuid_ok(c.id))) die("config: a chosen location that breaks the path rules");
        if (c.location < -1 || c.location > 1) die("config: location out of range");
        if (!(c.max_days >= 1 && c.max_days <= 36500) || c.max_bytes < (long long)(0.1 * (1LL << 30)) - 1) die("config: a limit out of range");
    } else if (!memchr(d, 0, n) && cfg_utf8_ok(d, n)) die("config: valid text refused");
    /* an over-long location line at the top (our scope) always makes the location malformed */
    if (n > CFG_LINE_MAX + 11 && !memcmp(d, "cache_dir=", 10) && !memchr(d, '\n', CFG_LINE_MAX + 1)
        && !memchr(d, 0, n) && cfg_utf8_ok(d, n) && c.location != -1) die("config: an over-long cache_dir line didn't make the location malformed");
}

static void t_marker(const uint8_t *d, size_t n) {
    if (marker_ok((const char *)d, n, FUZZ_ID) && !memmem(d, n, "format=" FORMAT, strlen("format=" FORMAT))) die("marker: accepted without its format");
}

/* a kept set's file: what counts and what fills agree, and an accepted file is exactly its lines */
static void t_kept(const uint8_t *d, size_t n) {
    long long until = -1, until2 = -1;
    size_t c = 0, c2 = 0;
    if (parse_kept((const char *)d, n, &until, NULL, &c)) return;
    kept_entry *v = malloc((c ? c : 1) * sizeof *v);
    if (parse_kept((const char *)d, n, &until2, v, &c2) || c2 != c || until2 != until) die("kept: counting and filling differ");
    if (until < 0 || until >= 1000000000000LL) die("kept: a date out of range");
    char line[KEPT_LINE + 1];
    size_t head = n - c * KEPT_LINE;
    for (size_t i = 0; i < c; i++) {
        char m[65], k[65];
        hex(v[i].model, 32, m);
        hex(v[i].key, 32, k);
        snprintf(line, sizeof line, "%s %s\n", m, k);
        if (memcmp(d + head + i * KEPT_LINE, line, KEPT_LINE)) die("kept: an entry that isn't its line");
    }
    free(v);
}

static void t_desc(const uint8_t *d, size_t n, int as_chosen) {
    summary s;
    chosen = as_chosen;
    if (parse_desc(d, n, &s) == 0) {
        size_t n2 = 2 * (size_t)s.frames;
        if (s.frames <= 0 || s.frames > DESC_MAX_FRAMES || !s.d) die("desc: bad frames");
        if (s.version == 2 && as_chosen) {
            for (int c = 0; c < 2; c++) if (!(s.rt[c] <= CHOSEN_RT_MAX)) die("desc: rt over its cap");
            for (size_t i = 0; i < n2; i++) if (!(s.rf[i] <= CHOSEN_RF_MAX)) die("desc: rf over its cap");
            if (near_usable(&s) && !(fabs(s.g0 - 1.0) <= CHOSEN_G0_SLACK)) die("desc: a near candidate with g0 out of range");
        }
        free_summary(&s);
    }
    chosen = 0;
}

static int16_t fuzz_pcm[FUZZ_FRAMES * 8];

static void t_flac(const uint8_t *d, size_t n, int from_file) {
    if (!from_file) {
        decode_flac(d, n, FUZZ_KEY, fuzz_pcm, FUZZ_FRAMES);
        return;
    }
    char path[PATH_MAX + 100];
    snprintf(path, sizeof path, "%s/%s.flac", fuzz_dir, FUZZ_KEY);
    FILE *f = fopen(path, "wb");
    if (!f) die("flac: can't write the scratch file");
    fwrite(d, 1, n, f);
    fclose(f);
    plat_dir dir;
    plat_file fd;
    int64_t size;
    char name[80];
    if (plat_dir_open(fuzz_dir, &dir)) die("flac: can't open the scratch folder");
    snprintf(name, sizeof name, "%s.flac", FUZZ_KEY);
    if (!plat_file_open_at(dir, name, flac_max(FUZZ_FRAMES), &size, &fd)) {
        unsigned char *b = malloc((size_t)size + 1);
        if (!plat_read_all(fd, b, (size_t)size)) decode_flac(b, (size_t)size, FUZZ_KEY, fuzz_pcm, FUZZ_FRAMES);
        free(b);
        plat_file_close(fd);
    }
    plat_dir_close(dir);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (!n) return 0;
    switch (d[0] % 7) {
    case 0: t_config(d + 1, n - 1); break;
    case 1: t_marker(d + 1, n - 1); break;
    case 2: t_desc(d + 1, n - 1, 0); break;
    case 3: t_desc(d + 1, n - 1, 1); break;
    case 4: t_flac(d + 1, n - 1, 0); break;
    case 5: t_kept(d + 1, n - 1); break;
    default: t_flac(d + 1, n - 1, 1); break;
    }
    return 0;
}

#ifndef USE_LIBFUZZER
static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

/* one mutation of seed into out (at most cap bytes): flips, interesting bytes, inserts, deletes,
 * truncation, duplication of a slice */
static size_t mutate(const uint8_t *seed, size_t n, uint8_t *out, size_t cap) {
    if (n > cap) n = cap;
    memcpy(out, seed, n);
    static const uint8_t special[] = {0, 0xFF, 0x7F, 0x80, '\n', '\r', '=', '"', '[', ']', '#', '/', '.', 0xEF, 0xC0, 0xED};
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

static void run(const char *name, void (*f)(const uint8_t *, size_t, int), int arg, const uint8_t *seed, size_t n, long iters) {
    size_t cap = n * 2 + 4096;
    uint8_t *buf = malloc(cap);
    f(seed, n, arg);
    for (long i = 0; i < iters; i++) f(buf, mutate(seed, n, buf, cap), arg);
    free(buf);
    printf("ok   %s: %ld mutations of a %zu-byte seed\n", name, iters, n);
}

static void w_config(const uint8_t *d, size_t n, int a) { (void)a; t_config(d, n); }
static void w_marker(const uint8_t *d, size_t n, int a) { (void)a; t_marker(d, n); }
static void w_kept(const uint8_t *d, size_t n, int a) { (void)a; t_kept(d, n); }

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    rng = argc > 2 ? strtoull(argv[2], NULL, 0) : (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL;
    if (!rng) rng = 1;
    printf("fuzz: seed %llu (fuzz %ld %llu repeats this run)\n", (unsigned long long)rng, iters, (unsigned long long)rng);
    snprintf(fuzz_dir, sizeof fuzz_dir, "%s/rbstems-fuzz.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
    mkdir(fuzz_dir, 0700);

    const char *configs[] = {
        "enabled=1\nmax_gb=20\nmax_days=60\nrekordbox_model=1\n",
        "# settings\n[cache]\ncache_dir=\"/Volumes/GLS/RB Stems Plus Cache.noindex\"\ncache_id=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F\nmax_gb=3\n[watcher]\nenabled=0\n",
        "\xEF\xBB\xBF  Cache_Dir = '/Volumes/DJ''s SSD/Sets #1/RB Stems Plus Cache.noindex' # x\r\ncache_id=0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f\r\n",
    };
    for (size_t i = 0; i < sizeof configs / sizeof *configs; i++)
        run("config", w_config, 0, (const uint8_t *)configs[i], strlen(configs[i]), iters);
    /* lines at, under and over CFG_LINE_MAX, and an over-long section line */
    static char longs[4][4096];
    const int widths[] = {1023, 1024, 1025};
    for (int i = 0; i < 3; i++) {
        int k = snprintf(longs[i], sizeof longs[i], "cache_dir=\"/Volumes/GLS/RB Stems Plus Cache.noindex\"");
        while (k < widths[i]) longs[i][k++] = ' ';
        snprintf(longs[i] + k, sizeof longs[i] - k, "\ncache_id=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F\n");
        run("config (long line)", w_config, 0, (const uint8_t *)longs[i], strlen(longs[i]), iters / 4);
    }
    int k = snprintf(longs[3], sizeof longs[3], "cache_dir=\"/Volumes/GLS/RB Stems Plus Cache.noindex\"\ncache_id=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F\n[");
    while (k < 1200) longs[3][k++] = 'a';
    snprintf(longs[3] + k, sizeof longs[3] - k, "]\nmax_gb=3\n");
    run("config (long section)", w_config, 0, (const uint8_t *)longs[3], strlen(longs[3]), iters / 4);
    const char *mk = "id=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F\nformat=rbstems-v1\n";
    run("marker", w_marker, 0, (const uint8_t *)mk, strlen(mk), iters);
    const char *ks = "rbstemsplus-kept 1\nuntil 1763164800\n"
                     "435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a ab11111111111111111111111111111111111111111111111111111111111111\n"
                     "435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a cd22222222222222222222222222222222222222222222222222222222222222\n";
    long long ku;
    size_t kc;
    if (parse_kept(ks, strlen(ks), &ku, NULL, &kc) || ku != 1763164800 || kc != 2) die("kept: the seed isn't read");
    run("kept", w_kept, 0, (const uint8_t *)ks, strlen(ks), iters);

    /* a valid RBD2 .desc of 20 frames */
    int frames = 20;
    size_t dn = 16 + 4 * 2 * (size_t)frames + 8 + 4 * 2 * (size_t)frames;
    uint8_t *desc = calloc(1, dn);
    float r0 = 0.99f, g0 = 1.01f, v;
    memcpy(desc, "RBD2", 4);
    memcpy(desc + 4, &frames, 4);
    memcpy(desc + 8, &r0, 4);
    memcpy(desc + 12, &g0, 4);
    for (size_t i = 16; i < dn; i += 4) { v = -(float)(i % 40); memcpy(desc + i, &v, 4); }
    run("desc", t_desc, 0, desc, dn, iters);
    run("desc (chosen location)", t_desc, 1, desc, dn, iters);

    /* a real entry, encoded by write_flac */
    static int16_t pcm[FUZZ_FRAMES * 8];
    for (size_t i = 0; i < FUZZ_FRAMES * 8; i++) pcm[i] = (int16_t)(sin(i * 0.01) * 8000 + (i % 7) * 30);
    write_job job = {pcm, FUZZ_FRAMES};
    plat_dir dir;
    int err;
    if (plat_dir_open(fuzz_dir, &dir) || write_flac(dir, FUZZ_KEY, &job, &err)) die("can't encode the seed FLAC");
    plat_dir_close(dir);
    char path[PATH_MAX + 100];
    snprintf(path, sizeof path, "%s/%s.flac", fuzz_dir, FUZZ_KEY);
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    size_t fn = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *flac = malloc(fn);
    if (fread(flac, 1, fn, f) != fn) die("can't read the seed FLAC");
    fclose(f);
    if (decode_flac(flac, fn, FUZZ_KEY, fuzz_pcm, FUZZ_FRAMES) || memcmp(fuzz_pcm, pcm, sizeof pcm)) die("the seed FLAC doesn't decode to its audio");
    run("flac", t_flac, 0, flac, fn, iters / 4);
    run("flac (plat_file_open_at from a file)", t_flac, 1, flac, fn, iters / 20);
    unlink(path);
    rmdir(fuzz_dir);
    printf("fuzz: no failures\n");
    return 0;
}
#endif
