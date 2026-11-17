#ifndef RBSP_CORE_DFUZZ_H
#define RBSP_CORE_DFUZZ_H
#include <stddef.h>
#include <stdint.h>
/* the stateful model and delta fuzz (dfuzz.c): 0, or the source line of the first mismatch */
int dfuzz_run(const uint8_t *d, size_t n);
#endif
