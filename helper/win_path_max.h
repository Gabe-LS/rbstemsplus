#ifndef RBSP_WIN_PATH_MAX_H
#define RBSP_WIN_PATH_MAX_H

#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
/* Include after <limits.h>: mingw-w64's PATH_MAX is only MAX_PATH (260). */
#undef PATH_MAX
#define PATH_MAX 4096
#endif

#endif
