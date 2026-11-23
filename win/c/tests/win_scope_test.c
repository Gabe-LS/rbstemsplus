/* CORE_STEP_WIN_SCOPE (core_native_steps.c's win_scope, extracted by prepare-args-test.sh) over a
 * fake inventory: every verb goes through inventory.c's inv_targets; Uninstall with an incomplete
 * inventory is allowed only for our bridges with their receipts (D3), the root kept by the worker. */
#include <strings.h>

#include "check.h"
#include "plat.h"
#define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h supplies the platform types */
#include "internal.h"
#include "core_native_steps.h"
#include "dismissed.h"
#include "host_facts.h"
#include "native_facts.h"
#include "native_steps.h"
#include "update.h"

int checks, failures;

typedef int SRWLOCK;
typedef int64_t LONG64;
#define NS_PATH_MAX 1024
#define NS_KEYS_MAX 4
#define SRWLOCK_INIT 0

static int complete = 1, cache_supported;
static inv_assess_row rows[3];
static int reads;

/* the fake fresh read: 7.2.19 holds our bridge (its receipt per rows[]), 7.2.20 Pioneer's, 7.2.18 on disk only */
int host_facts_read_scope(struct core *c, const payload_manifest *manifest, host_facts *out) {
    (void)c;
    reads++;
    memset(out, 0, sizeof *out);
    out->inventory = calloc(1, sizeof *out->inventory);
    out->assessment = calloc(INV_MAX, sizeof *out->assessment);
    out->cache_manifest = calloc(1, sizeof *out->cache_manifest);
    if (!out->inventory || !out->assessment || !out->cache_manifest) abort();
    if (manifest) *out->cache_manifest = *manifest;   /* the scope's payload: the flow's, when given */
    inv_list *l = out->inventory;
    static const char *const versions[] = {"7.2.20", "7.2.19", "7.2.18"};
    l->n = 3;
    l->complete = complete;
    for (int i = 0; i < 3; i++) {
        strcpy(l->it[i].version, versions[i]);
        snprintf(l->it[i].name, sizeof l->it[i].name, "rekordbox %s", versions[i]);
        l->it[i].trust = i == 2 ? INV_DISK_ONLY : INV_VERIFIED;
        if (i < 2) memset(l->it[i].exe.sha256, i ? '1' : '2', 64);
        out->assessment[i] = rows[i];
    }
    return 0;
}

int host_facts_read_core(core *c, host_facts *out) {
    int before = reads;
    int rc = host_facts_read_scope(c, NULL, out);
    reads = before;
    out->cache_answered = complete;
    out->blocked.rekordbox_supported = cache_supported;
    return rc;
}

void host_facts_free(host_facts *f) {
    free(f->inventory);
    free(f->assessment);
    free(f->cache_manifest);
    memset(f, 0, sizeof *f);
}

/* Older R1 sampling saw the live bridge and shared runtime and reported nothing wrong. */
int nw_admin_r1(nw_admin *admin, const char *folder) {
    (void)admin;
    (void)folder;
    return NF_R1_NOTHING;
}

static char dismissal[64];

static int dismissed_add_locked(const char *support, const char *lines, size_t n) {
    if (strcmp(support, "support") || n >= sizeof dismissal) abort();
    snprintf(dismissal, sizeof dismissal, "%.*s", (int)n, lines);
    return 0;
}

static void read_lists(nw_steps *s, payload_manifest *saved, int *hs, payload_manifest *installed, int *hi) {
    (void)s; (void)saved; (void)installed;
    *hs = *hi = 0;
}

void native_blocked_plus(const char *model, const payload_manifest *m, char *engine, size_t n, int *supported) {
    (void)model; (void)m; (void)engine; (void)n; (void)supported;
    abort();  /* this test asks for Cache only */
}

#include "win_scope.inc"

static nw_steps steps = {.support = "support"};

typedef struct {
    int rc;
    int64_t code;
    const char *token, *versions, *affected;
} scope_out;

static arena a;

static void test_r1_dismiss_assessment(void) {
    core_facts out;
    rows[0].r1 = NF_R1_FOREIGN;  /* a live bridge without a matching receipt */
    rows[1].r1 = NF_R1_NOTHING;
    rows[2].r1 = NF_R1_FOREIGN;  /* a repair row that wasn't shown on the button */
    facts_init(&out, &a);
    CHECK(r1_dismiss(&steps, "7.2.20,7.2.19", &out) == 0 &&
          out.s[FACT_STEP_TEXT] && !strcmp(out.s[FACT_STEP_TEXT], "7.2.20") &&
          !strcmp(dismissal, "r1:7.2.20\n"), "Don't Ask Again writes only the shown row that still needs repair");
}

static void test_blocked_cache_answer(void) {
    blocked_read r;
    complete = 0;
    blocked_facts(&steps, 0, 1, &r);
    CHECK(r.cache_r == -1, "BLOCKED has no answer when the inventory is incomplete and support is unknown");
    cache_supported = 1;
    blocked_facts(&steps, 0, 1, &r);
    CHECK(r.cache_r == 0 && r.b.rekordbox_supported, "BLOCKED can report a known supported row in an incomplete inventory");
    complete = 1;
    cache_supported = 0;
    blocked_facts(&steps, 0, 1, &r);
    CHECK(r.cache_r == 0 && !r.b.rekordbox_supported, "BLOCKED can report no support in a complete inventory");
}

static scope_out scope(const char *verb) {
    core_facts out;
    arena_free(&a);
    memset(&a, 0, sizeof a);
    facts_init(&out, &a);
    scope_out r = {win_scope(&steps, verb, &out), out.i[FACT_STEP_CODE], out.s[FACT_WIN_SCOPE], out.s[FACT_WIN_SCOPE_VERSIONS],
                   out.s[FACT_WIN_SCOPE_AFFECTED]};
    if (!r.token) r.token = "(unset)";
    if (!r.versions) r.versions = "(unset)";
    if (!r.affected) r.affected = "(unset)";
    return r;
}

int main(void) {
    rows[0] = (inv_assess_row){.live = COMPAT_LIVE_PIONEER, .listed = 1, .original_bound = 1};
    rows[1] = (inv_assess_row){.live = COMPAT_LIVE_OURS, .listed = 1, .original_bound = 1, .receipt_ok = 1};
    rows[2] = (inv_assess_row){.live = COMPAT_LIVE_OURS_UNLISTED};

    scope_out s = scope("uninstall");
    CHECK(!s.rc && s.code == 0 && !strcmp(s.token, "*") && !strcmp(s.versions, "") && !strcmp(s.affected, "7.2.18\n7.2.19\n"),
          "uninstall, complete: the sweep, every bridge affected (%d %lld %s / %s)", s.rc, (long long)s.code, s.token, s.affected);
    rows[0].r1 = NF_R1_LIVE_MISSING;
    s = scope("uninstall");
    CHECK(!s.rc && s.code == 0 && !strcmp(s.affected, "7.2.18\n7.2.19\n7.2.20\n"), "uninstall, complete: a repair row is affected too (%s)", s.affected);

    complete = 0;
    s = scope("uninstall");
    CHECK(!s.rc && s.code == 0 && !strcmp(s.token, "*") && !strcmp(s.affected, "7.2.19\n"),
          "uninstall, incomplete (D3): allowed, only the bridge with its receipt (%lld %s / %s)", (long long)s.code, s.token, s.affected);
    rows[1].receipt_ok = 0;
    s = scope("uninstall");
    CHECK(!s.rc && s.code == 1 && !strcmp(s.token, "") && !strcmp(s.affected, ""),
          "uninstall, incomplete, no bridge with its receipt: nothing may be changed now (%lld %s)", (long long)s.code, s.affected);
    rows[1].receipt_ok = 1;

    complete = 1;
    s = scope("restore");
    CHECK(!s.rc && s.code == 0 && !strcmp(s.token, "7.2.20:2222222222222222") && !strcmp(s.versions, "7.2.20\n"),
          "restore, complete: the inv_targets token (%s)", s.token);

    CHECK(scope("install").rc == 1, "install without a flow manifest: couldn't be checked");
    CHECK(scope("anything").rc == 1 && reads == 5, "an unknown verb: no read");
    /* re-review finding 6: install's token carries the digest of the flow's payload.json, the one
     * its scope came from ("<targets>@<sha256>"); a flow manifest without one isn't a scope */
    steps.flow.has = 1;
    steps.flow.m.has_windows = 1;
    memset(steps.flow.m.win_bridge.sha256, 'b', 64);
    CHECK(scope("install").rc == 1, "install, the flow payload's bytes unknown: couldn't be checked");
    memset(steps.flow.m.json_sha256, 'a', 64);
    s = scope("install");
    {
        const char *at = strchr(s.token, '@');
        CHECK(!s.rc && s.code == 0 && !strncmp(s.token, "7.2.20:2222222222222222", 23) && at && strlen(at + 1) == 64 && strspn(at + 1, "a") == 64,
              "install: the targets and the flow payload's digest (%s)", s.token);
    }
    test_r1_dismiss_assessment();
    test_blocked_cache_answer();
    arena_free(&a);
    printf("win scope: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
