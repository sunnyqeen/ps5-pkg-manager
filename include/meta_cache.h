#ifndef META_CACHE_H
#define META_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Process-wide cache of large blocks used to serve small metadata reads.
 * Blocks are shared between sessions and reused only when the key (url),
 * file size and validator (timestamp string) all match. */

#define META_CACHE_SLOTS 16
#define META_CACHE_BLOCK (512U * 1024U)
#define META_CACHE_MAX_READ (64U * 1024U)

/* Reads len bytes at off into buf; returns bytes read or <0 on error. */
typedef ssize_t (*meta_cache_fetch_fn)(void *ctx, void *buf, size_t len, uint64_t off);

/* Returns count on success, -1 on fetch error, -2 when the cache cannot be used
 * (caller should do a direct read). */
ssize_t meta_cache_read(const char *key, uint64_t size, const char *validator,
                        void *buf, size_t count, uint64_t offset,
                        meta_cache_fetch_fn fetch, void *ctx);

#endif
