/* pe_code.c: a PE file's code digest (multi-version contract, .orchestration/mv-spec.md §2.3;
 * decision D2a). Pioneer re-signs onnxruntime.dll per rekordbox release with the code unchanged
 * (7.2.19's and 7.2.20's differ only in the certificate table and the PE CheckSum), so the
 * signed list binds each version's file sha256 and one shared code digest, and the one runtime
 * copy in ort\ serves every bound version. Pure C11 over compat.h's compat_read_fn, no Windows
 * call: the Mac tests, the worker and scripts/win-ort-code.py (the same definition, for
 * build.sh's recorded check) agree on it.
 *
 * L0 contract: the body is a stub that answers "not implemented" (-1); lane B0 writes it. */
#ifndef RBSP_WIN_PE_CODE_H
#define RBSP_WIN_PE_CODE_H

#include "compat.h"
#include "payload.h"

#include <stdint.h>

enum { PE_CODE_CERT_MAX = 64 * 1024 };   /* the certificate table's largest size taken */

/* The sha256 of the file's size bytes with three ranges skipped: the optional header's CheckSum
 * (4 bytes), the Certificate Table data-directory entry (8 bytes) and the certificate table
 * itself. Needs a PE32 or PE32+ whose certificate table starts after every section's raw data,
 * ends exactly at EOF and is at most PE_CODE_CERT_MAX bytes (a file with no certificate table:
 * the two header ranges only). 0 with out filled (64 lowercase hex digits), or -1: no digest,
 * never bound. */
int pe_code_sha256(compat_read_fn read, void *ctx, uint64_t size, payload_sha out);

#endif
