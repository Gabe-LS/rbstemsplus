/* unzip.c: the app zip's reader for the updater (app.md §5.4, release.md §7: "extracts it into
 * ...\Programs\RB Stems Plus\<new>\"), over common/plat.h's file layer. Windows has no zip API in C,
 * and no third-party library is added: this is our own reader with its own inflate (RFC 1951).
 *
 * The zip it reads has been checked against the verified payload's SHA-256 first, so its bytes are
 * release.sh's; the reader is still strict, and fuzzed (tests/fuzz.c):
 *  - one disk, no ZIP64, no encryption, methods stored (0) and deflate (8) only; the central
 *    directory and each local header agree on the method and the name; no two entries share a
 *    byte (local header to the end of the data); every entry's CRC-32 and size are checked on the
 *    bytes written;
 *  - names: printable ASCII, '/' separated, relative, at most 8 levels and 255 bytes; no empty, "."
 *    or ".." component, no '\', ':' or a character Windows refuses in a name, no component ending
 *    in a space or a dot (Windows would drop them), no device name (CON, NUL, COM1...); no two
 *    entries the same name ignoring ASCII case (the volume ignores it), nor a file that is another
 *    entry's folder ("a" and "A/b"); a Unix symbolic link entry
 *    is refused (it would come out as a file holding the link's target);
 *  - at most UNZIP_ENTRIES_MAX entries, UNZIP_FILE_MAX bytes per file, UNZIP_TOTAL_MAX in all.
 * Folders are made and files created through the folder handles (no link followed); a name that
 * exists already is refused. */
#ifndef RBSP_WIN_UNZIP_H
#define RBSP_WIN_UNZIP_H

#include "plat.h"

#include <stddef.h>
#include <stdint.h>

enum {
    UNZIP_ENTRIES_MAX = 4096,
    UNZIP_FILE_MAX = 128 * 1024 * 1024,
    UNZIP_TOTAL_MAX = 512 * 1024 * 1024,
    UNZIP_NAME_MAX = 255,
    UNZIP_DEPTH_MAX = 8,
};

enum {
    UNZIP_OK = 0,
    UNZIP_NOT_ZIP,     /* not a zip this reads (structure, method, flags, ZIP64, a size cap) */
    UNZIP_BAD_NAME,    /* a name the rules above refuse, or two the same */
    UNZIP_DAMAGED,     /* a stream that doesn't inflate, or a CRC-32 or size that doesn't match */
    UNZIP_WRITE,       /* a file or folder couldn't be made (*err) */
};

/* RFC 1951 raw deflate: in[0..n) into exactly out_n bytes at out. 0, or -1 when the stream isn't
 * valid, needs more input, ends early or would write more than out_n bytes. *used (may be NULL):
 * the input bytes the stream took (its last partial byte counted). */
int unzip_inflate(const unsigned char *in, size_t n, unsigned char *out, size_t out_n, size_t *used);

/* CRC-32 (ISO-HDLC, zip's), continuing from crc (0 to start) */
uint32_t unzip_crc32(uint32_t crc, const void *p, size_t n);

/* whether a zip entry's name is one the rules above allow (dirs end in '/') */
int unzip_name_ok(const char *name, size_t n);

/* the whole zip read and every entry inflated and CRC-checked, nothing written: UNZIP_* (UNZIP_WRITE:
 * out of memory) */
int unzip_check(const void *data, size_t n);

/* Every entry of the zip in data[0..n) (unzip_check first: nothing is written from a zip that
 * fails it) into the folder dest (an absolute path to an existing,
 * empty folder). UNZIP_*; *err the errno of UNZIP_WRITE (may be NULL). On a failure, what was
 * written stays: the caller deletes the folder. */
int unzip_extract(const void *data, size_t n, const char *dest, int *err);
/* the same into the folder the handle root holds (the updater's staging handle: what is checked
 * is what gets renamed, review UH2); root stays open */
int unzip_extract_at(const void *data, size_t n, plat_dir root, int *err);

#endif
