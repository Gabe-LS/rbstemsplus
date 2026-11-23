/* The WPF shell's own files, relative to the opened support folder. */
#include "core_win.h"
#include "fs_write.h"

#ifdef _WIN32
#include "plat_win.h"
#else
/* The shell's Mac host tests use the same directory opens and append checks. */
#include <sys/stat.h>
#include <unistd.h>

static int plat_file_append_at(plat_dir d, const char *name, const void *p, size_t n) {
    int fd = openat(d.fd, name, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
    if (fd < 0) return errno;
    struct stat st;
    int e = 0;
    if (fstat(fd, &st)) e = errno;
    else if (!S_ISREG(st.st_mode) || st.st_nlink != 1) e = PLAT_EFTYPE;
    else {
        ssize_t written = write(fd, p, n);
        if (written < 0) e = errno;
        else if ((size_t)written != n) e = EIO;
    }
    close(fd);
    return e;
}
#endif

static int open_folder_at(plat_dir parent, const char *name, plat_dir *out) {
    int e = plat_dir_open_at(parent, name, out);
    if (e != ENOENT) return e;
    e = plat_mkdir_at(parent, name);
    if (e && e != EEXIST) return e;
    return plat_dir_open_at(parent, name, out);
}

static int open_support(const char *parent, const char *support, plat_dir *out) {
    if (!parent || !support) return EINVAL;
    plat_dir d;
    int e = plat_dir_open(parent, &d);
    if (e) return e;
    e = open_folder_at(d, support, out);
    plat_dir_close(d);
    return e;
}

CORE_API int32_t CORE_CALL core_win_window_save(const char *parent, const char *support, const char *name, const void *text, size_t n) {
    plat_dir d;
    int e = open_support(parent, support, &d);
    if (e) return e;
    e = win_write_at(d, name, text, n);
    plat_dir_close(d);
    return e;
}

CORE_API int32_t CORE_CALL core_win_log_append(const char *parent, const char *support, const void *text, size_t n) {
    plat_dir d, logs;
    int e = open_support(parent, support, &d);
    if (e) return e;
    e = open_folder_at(d, "Logs", &logs);
    plat_dir_close(d);
    if (e) return e;
    e = plat_file_append_at(logs, "app.log", text, n);
    plat_dir_close(logs);
    return e;
}
