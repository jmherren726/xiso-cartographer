#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* mkdir() under strict C11 */
#endif

#include "host_path.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include <direct.h>
#include <wchar.h>
#include <windows.h>

/*
 * UTF-8 -> absolute UTF-16 path with the "\\?\" prefix (drive paths only;
 * UNC paths are returned without it). Caller frees.
 */
static wchar_t *long_wide_path(const char *path)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w == NULL) {
        return NULL;
    }
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, n);
    for (wchar_t *p = w; *p != L'\0'; p++) {
        if (*p == L'/') {
            *p = L'\\';
        }
    }
    DWORD len = GetFullPathNameW(w, 0, NULL, NULL);
    if (len == 0) {
        free(w);
        return NULL;
    }
    wchar_t *full = malloc(((size_t)len + 4) * sizeof *full);
    if (full == NULL) {
        free(w);
        return NULL;
    }
    DWORD got = GetFullPathNameW(w, len, full + 4, NULL);
    free(w);
    if (got == 0 || got >= len) {
        free(full);
        return NULL;
    }
    if (full[4] == L'\\' && full[5] == L'\\') {
        memmove(full, full + 4, ((size_t)got + 1) * sizeof *full); /* UNC: no prefix */
    } else {
        full[0] = L'\\';
        full[1] = L'\\';
        full[2] = L'?';
        full[3] = L'\\';
    }
    return full;
}

FILE *xc_host_fopen(const char *path, const char *mode)
{
    wchar_t wmode[8];
    size_t i = 0;
    for (; mode[i] != '\0' && i < 7; i++) {
        wmode[i] = (wchar_t)(unsigned char)mode[i];
    }
    wmode[i] = L'\0';
    wchar_t *w = long_wide_path(path);
    if (w == NULL) {
        return NULL;
    }
    FILE *f = _wfopen(w, wmode);
    free(w);
    return f;
}

bool xc_host_mkdir(const char *path)
{
    wchar_t *w = long_wide_path(path);
    if (w == NULL) {
        return false;
    }
    bool ok = _wmkdir(w) == 0 || errno == EEXIST;
    free(w);
    return ok;
}

bool xc_host_remove(const char *path)
{
    wchar_t *w = long_wide_path(path);
    if (w == NULL) {
        return false;
    }
    bool ok = _wremove(w) == 0;
    free(w);
    return ok;
}

#else

#include <sys/stat.h>

FILE *xc_host_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

bool xc_host_mkdir(const char *path)
{
    return mkdir(path, 0777) == 0 || errno == EEXIST;
}

bool xc_host_remove(const char *path)
{
    return remove(path) == 0;
}

#endif
