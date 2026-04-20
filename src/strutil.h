/*
 * strutil.h — Lightweight string utilities for Varnish.
 */

#ifndef VARNISH_STRUTIL_H
#define VARNISH_STRUTIL_H

#include <stddef.h>
#include <string.h>

static inline void str_copy_trunc(char *dst, size_t dst_size, const char *src) {
    size_t len;

    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strnlen(src, dst_size - 1);
    memmove(dst, src, len);
    dst[len] = '\0';
}

static inline int str_append(char *dst, size_t dst_size, const char *suffix) {
    size_t dst_len;
    size_t suffix_len;

    if (!dst || dst_size == 0 || !suffix) return -1;

    dst_len = strlen(dst);
    suffix_len = strlen(suffix);
    if (dst_len + suffix_len + 1 > dst_size) return -1;

    memmove(dst + dst_len, suffix, suffix_len + 1);
    return 0;
}

static inline int path_join(char *dst, size_t dst_size, const char *lhs, const char *rhs) {
    size_t lhs_len;
    size_t rhs_len;

    if (!dst || dst_size == 0 || !lhs || !rhs) return -1;

    lhs_len = strlen(lhs);
    rhs_len = strlen(rhs);
    if (lhs_len + 1 + rhs_len + 1 > dst_size) {
        dst[0] = '\0';
        return -1;
    }

    memmove(dst, lhs, lhs_len);
    dst[lhs_len] = '/';
    memmove(dst + lhs_len + 1, rhs, rhs_len);
    dst[lhs_len + rhs_len + 1] = '\0';

    return 0;
}

#endif /* VARNISH_STRUTIL_H */
