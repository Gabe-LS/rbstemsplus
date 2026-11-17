/* shell.h's CriticalWork. */
#include "shell.h"

void critical_init(critical_work *w) {
    plat_mutex_init(&w->lock);
    plat_cond_init(&w->idle);
    w->count = 0;
    w->closed = 0;
}

void critical_close(critical_work *w) {
    plat_mutex_lock(&w->lock);
    w->closed = 1;
    plat_mutex_unlock(&w->lock);
}

int critical_admit(critical_work *w) {
    plat_mutex_lock(&w->lock);
    int open = !w->closed;
    plat_mutex_unlock(&w->lock);
    return open;
}

int critical_try_begin(critical_work *w) {
    plat_mutex_lock(&w->lock);
    int ok = !w->closed;
    if (ok) w->count++;
    plat_mutex_unlock(&w->lock);
    return ok;
}

void critical_begin(critical_work *w) {
    plat_mutex_lock(&w->lock);
    w->count++;
    plat_mutex_unlock(&w->lock);
}

void critical_end(critical_work *w) {
    plat_mutex_lock(&w->lock);
    if (w->count > 0 && --w->count == 0) plat_cond_broadcast(&w->idle);
    plat_mutex_unlock(&w->lock);
}

int critical_running(critical_work *w) {
    plat_mutex_lock(&w->lock);
    int r = w->count > 0;
    plat_mutex_unlock(&w->lock);
    return r;
}

int critical_wait_idle(critical_work *w, uint32_t timeout_ms) {
    uint64_t end = plat_now_mono_ns() + (uint64_t)timeout_ms * 1000000u;
    plat_mutex_lock(&w->lock);
    while (w->count > 0) {
        uint64_t now = plat_now_mono_ns();
        if (now >= end) break;
        plat_cond_wait_ns(&w->idle, &w->lock, end - now);
    }
    int idle = w->count == 0;
    plat_mutex_unlock(&w->lock);
    return idle;
}

static critical_work process_work;
static plat_once_t process_once = PLAT_ONCE_INIT;

static void process_init(void) { critical_init(&process_work); }

critical_work *critical_process(void) {
    plat_once(&process_once, process_init);
    return &process_work;
}
