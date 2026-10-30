#ifndef SEVENZIP_H
#define SEVENZIP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Minimal 7z header reader. It only locates a .pkg entry that is stored raw
 * (Copy method, no compression) so it can be streamed straight from the archive. */

/* Reads count bytes at offset into buf; returns bytes read or <0 on error. */
typedef ssize_t (*sevenzip_read_fn)(void *ctx, void *buf, size_t count, uint64_t offset);

/* Finds the pkg_index-th (0-based) file whose name ends with ".pkg", in header order.
 * On success returns 0 and sets the absolute byte range of the file inside the archive.
 * On failure returns -1 and fills err. */
int sevenzip_find_raw_pkg(sevenzip_read_fn rd, void *ctx, uint64_t archive_size, uint32_t pkg_index,
                          uint64_t *out_offset, uint64_t *out_size, char *err, size_t err_sz);

#endif
