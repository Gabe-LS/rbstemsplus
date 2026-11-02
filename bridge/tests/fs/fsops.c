/* What exFAT's and FAT32's "._" companions do under the calls the bridge and the
 * app use. Run in a folder on the volume; prints the "._" names left after each step.
 *
 *   fsops DIR        (DIR is created; everything happens inside it)
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int dir;

static void list(const char *after) {
    int d = openat(dir, ".", O_RDONLY | O_DIRECTORY);       /* its own offset: a dup() shares it, and reads EOF */
    DIR *dp = fdopendir(d);
    struct dirent *e;
    char names[4096] = "";
    while ((e = readdir(dp)))
        if (!strncmp(e->d_name, "._", 2)) { strcat(names, " "); strcat(names, e->d_name); }
    closedir(dp);
    printf("  %-58s ._ files:%s\n", after, names[0] ? names : " none");
}

static void put(const char *name, const char *text) {
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) { perror(name); exit(1); }
    write(fd, text, strlen(text));
    close(fd);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    mkdir(argv[1], 0755);
    dir = open(argv[1], O_RDONLY | O_DIRECTORY);
    put("k.flac.1.tmp", "new");
    list("openat(O_CREAT) k.flac.1.tmp");
    renameat(dir, "k.flac.1.tmp", dir, "k.flac");
    list("renameat k.flac.1.tmp -> k.flac (no target)");
    put("k.flac.2.tmp", "newer");
    list("openat(O_CREAT) k.flac.2.tmp");
    renameat(dir, "k.flac.2.tmp", dir, "k.flac");
    list("renameat k.flac.2.tmp -> k.flac (over the target)");
    unlinkat(dir, "k.flac", 0);
    list("unlinkat k.flac");
    mkdirat(dir, "ab", 0755);
    list("mkdirat ab");
    int ab = openat(dir, "ab", O_RDONLY | O_DIRECTORY);
    int fd = openat(ab, "x.desc", O_WRONLY | O_CREAT | O_EXCL, 0644);
    close(fd);
    unlinkat(ab, "x.desc", 0);
    close(ab);
    unlinkat(dir, "ab", AT_REMOVEDIR);
    list("unlinkat(AT_REMOVEDIR) ab (emptied first)");
    mkdirat(dir, "cd", 0755);
    char p[1024];
    snprintf(p, sizeof p, "%s/cd", argv[1]);
    rmdir(p);
    list("mkdirat cd, rmdir(path) cd");
    return 0;
}
