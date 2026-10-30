#include "sevenzip.h"
#include "lzma_dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SZ_START_HDR_SIZE 32
#define SZ_HEADER_MAX (8U * 1024U * 1024U)
#define SZ_MAX_ITEMS (1U << 20)
#define SZ_MAX_VALUE (1ULL << 62)

enum {
    K_END = 0x00, K_HEADER = 0x01, K_ARCHIVE_PROPS = 0x02, K_ADDITIONAL = 0x03, K_MAIN = 0x04,
    K_FILES = 0x05, K_PACK = 0x06, K_UNPACK = 0x07, K_SUBSTREAMS = 0x08, K_SIZE = 0x09,
    K_CRC = 0x0A, K_FOLDER = 0x0B, K_CODERS_UNPACK = 0x0C, K_NUM_UNPACK = 0x0D,
    K_EMPTY_STREAM = 0x0E, K_NAMES = 0x11, K_ENCODED = 0x17
};

typedef struct {
    const uint8_t *p, *end;
} cur_t;

typedef struct {
    uint64_t unpack, nsub, nout, npacked;
    int raw, crc_def, lzma;
    uint8_t props[5];
} folder_t;

typedef struct {
    uint64_t pack_pos, npack, nfolders, nsub_total;
    uint64_t *pack_sizes;
    folder_t *folders;
    uint64_t *sub_sizes;
} streams_t;

static void set_err(char *err, size_t err_sz, const char *msg) {
    if (err && err_sz) snprintf(err, err_sz, "%s", msg);
}

static int rd_u8(cur_t *c, uint8_t *v) {
    if (c->p >= c->end) return -1;
    *v = *c->p++;
    return 0;
}

static int skip(cur_t *c, uint64_t n) {
    if (n > (uint64_t)(c->end - c->p)) return -1;
    c->p += n;
    return 0;
}

/* 7z variable length number: leading 1 bits of the first byte give the extra byte count. */
static int rd_num(cur_t *c, uint64_t *v) {
    uint8_t first, mask = 0x80;
    uint64_t val = 0;
    if (rd_u8(c, &first)) return -1;
    for (int i = 0; i < 8; i++) {
        uint8_t b;
        if (!(first & mask)) {
            val |= (uint64_t)(first & (mask - 1)) << (8 * i);
            break;
        }
        if (rd_u8(c, &b)) return -1;
        val |= (uint64_t)b << (8 * i);
        mask >>= 1;
    }
    *v = val;
    return 0;
}

static int rd_bits(cur_t *c, uint64_t n, uint8_t *out) {
    uint8_t b = 0, mask = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (mask == 0) {
            if (rd_u8(c, &b)) return -1;
            mask = 0x80;
        }
        out[i] = (b & mask) ? 1 : 0;
        mask >>= 1;
    }
    return 0;
}

static int parse_digests(cur_t *c, uint64_t n, uint8_t *def) {
    uint8_t all;
    uint64_t defined = 0;
    if (n > SZ_MAX_ITEMS || rd_u8(c, &all)) return -1;
    if (all) {
        defined = n;
        if (def) memset(def, 1, (size_t)n);
    } else {
        uint8_t *bits = (uint8_t *)calloc((size_t)n + 1, 1);
        if (!bits) return -1;
        if (rd_bits(c, n, bits)) {
            free(bits);
            return -1;
        }
        for (uint64_t i = 0; i < n; i++) {
            defined += bits[i];
            if (def) def[i] = bits[i];
        }
        free(bits);
    }
    return skip(c, defined * 4);
}

static void free_streams(streams_t *st) {
    free(st->pack_sizes);
    free(st->folders);
    free(st->sub_sizes);
    memset(st, 0, sizeof(*st));
}

static int parse_folder(cur_t *c, folder_t *f) {
    uint64_t ncoders, total_in = 0, total_out = 0, bind_pairs, packed, v;
    if (rd_num(c, &ncoders) || ncoders == 0 || ncoders > 32) return -1;
    f->raw = ncoders == 1;
    f->lzma = 0;
    for (uint64_t i = 0; i < ncoders; i++) {
        uint8_t flags;
        uint64_t id = 0, nin = 1, nout = 1, psz;
        if (rd_u8(c, &flags)) return -1;
        if ((flags & 0x0F) > 8 || (flags & 0x80)) return -1;
        for (int k = 0; k < (flags & 0x0F); k++) {
            uint8_t b;
            if (rd_u8(c, &b)) return -1;
            id = (id << 8) | b;
        }
        if (flags & 0x10) {
            if (rd_num(c, &nin) || rd_num(c, &nout) || nin > 32 || nout > 32) return -1;
        }
        if (flags & 0x20) {
            if (rd_num(c, &psz)) return -1;
            if (ncoders == 1 && id == 0x030101 && psz == 5 && (size_t)(c->end - c->p) >= 5) {
                memcpy(f->props, c->p, 5);
                f->lzma = 1;
            }
            if (skip(c, psz)) return -1;
        }
        if (ncoders == 1 && (id != 0 || nin != 1 || nout != 1)) f->raw = 0;
        total_in += nin;
        total_out += nout;
    }
    if (total_out == 0 || total_in < total_out - 1) return -1;
    bind_pairs = total_out - 1;
    for (uint64_t i = 0; i < bind_pairs; i++) {
        if (rd_num(c, &v) || rd_num(c, &v)) return -1;
    }
    packed = total_in - bind_pairs;
    if (packed > 1) {
        for (uint64_t i = 0; i < packed; i++) {
            if (rd_num(c, &v)) return -1;
        }
    }
    f->nout = total_out;
    f->npacked = packed;
    return 0;
}

static int parse_pack_info(cur_t *c, streams_t *st) {
    uint64_t id;
    if (rd_num(c, &st->pack_pos) || rd_num(c, &st->npack)) return -1;
    if (st->pack_pos > SZ_MAX_VALUE || st->npack > SZ_MAX_ITEMS) return -1;
    st->pack_sizes = (uint64_t *)calloc((size_t)st->npack + 1, sizeof(uint64_t));
    if (!st->pack_sizes) return -1;
    for (;;) {
        if (rd_num(c, &id)) return -1;
        if (id == K_END) return 0;
        if (id == K_SIZE) {
            for (uint64_t i = 0; i < st->npack; i++) {
                if (rd_num(c, &st->pack_sizes[i]) || st->pack_sizes[i] > SZ_MAX_VALUE) return -1;
            }
        } else if (id == K_CRC) {
            if (parse_digests(c, st->npack, NULL)) return -1;
        } else {
            return -1;
        }
    }
}

static int parse_unpack_info(cur_t *c, streams_t *st) {
    uint64_t id;
    uint8_t external;
    if (rd_num(c, &id) || id != K_FOLDER) return -1;
    if (rd_num(c, &st->nfolders) || st->nfolders > SZ_MAX_ITEMS || rd_u8(c, &external) || external) return -1;
    st->folders = (folder_t *)calloc((size_t)st->nfolders + 1, sizeof(folder_t));
    if (!st->folders) return -1;
    for (uint64_t i = 0; i < st->nfolders; i++) {
        if (parse_folder(c, &st->folders[i])) return -1;
        st->folders[i].nsub = 1;
    }
    if (rd_num(c, &id) || id != K_CODERS_UNPACK) return -1;
    for (uint64_t i = 0; i < st->nfolders; i++) {
        folder_t *f = &st->folders[i];
        for (uint64_t j = 0; j < f->nout; j++) {
            uint64_t v;
            if (rd_num(c, &v) || v > SZ_MAX_VALUE) return -1;
            if (j == f->nout - 1) f->unpack = v;
        }
    }
    for (;;) {
        if (rd_num(c, &id)) return -1;
        if (id == K_END) return 0;
        if (id == K_CRC) {
            uint8_t *def = (uint8_t *)calloc((size_t)st->nfolders + 1, 1);
            if (!def) return -1;
            if (parse_digests(c, st->nfolders, def)) {
                free(def);
                return -1;
            }
            for (uint64_t i = 0; i < st->nfolders; i++) st->folders[i].crc_def = def[i];
            free(def);
        } else {
            return -1;
        }
    }
}

static int parse_substreams(cur_t *c, streams_t *st) {
    uint64_t id, total = 0, k = 0;
    if (!st->folders || rd_num(c, &id)) return -1;
    if (id == K_NUM_UNPACK) {
        for (uint64_t i = 0; i < st->nfolders; i++) {
            if (rd_num(c, &st->folders[i].nsub) || st->folders[i].nsub > SZ_MAX_ITEMS) return -1;
        }
        if (rd_num(c, &id)) return -1;
    }
    for (uint64_t i = 0; i < st->nfolders; i++) total += st->folders[i].nsub;
    if (total > SZ_MAX_ITEMS) return -1;
    free(st->sub_sizes);
    st->sub_sizes = (uint64_t *)calloc((size_t)total + 1, sizeof(uint64_t));
    if (!st->sub_sizes) return -1;
    st->nsub_total = total;
    if (id == K_SIZE) {
        for (uint64_t i = 0; i < st->nfolders; i++) {
            const folder_t *f = &st->folders[i];
            uint64_t sum = 0;
            if (f->nsub == 0) continue;
            for (uint64_t j = 0; j + 1 < f->nsub; j++) {
                uint64_t v;
                if (rd_num(c, &v) || v > SZ_MAX_VALUE) return -1;
                sum += v;
                if (sum > f->unpack) return -1;
                st->sub_sizes[k++] = v;
            }
            st->sub_sizes[k++] = f->unpack - sum;
        }
        if (rd_num(c, &id)) return -1;
    } else {
        for (uint64_t i = 0; i < st->nfolders; i++) {
            if (st->folders[i].nsub == 1) st->sub_sizes[k++] = st->folders[i].unpack;
            else if (st->folders[i].nsub > 1) return -1;
        }
    }
    while (id != K_END) {
        uint64_t cnt = 0;
        if (id != K_CRC) return -1;
        for (uint64_t i = 0; i < st->nfolders; i++) {
            if (st->folders[i].nsub != 1 || !st->folders[i].crc_def) cnt += st->folders[i].nsub;
        }
        if (parse_digests(c, cnt, NULL) || rd_num(c, &id)) return -1;
    }
    return 0;
}

static int parse_streams(cur_t *c, streams_t *st) {
    uint64_t id;
    for (;;) {
        if (rd_num(c, &id)) return -1;
        if (id == K_END) break;
        if (id == K_PACK) {
            if (parse_pack_info(c, st)) return -1;
        } else if (id == K_UNPACK) {
            if (parse_unpack_info(c, st)) return -1;
        } else if (id == K_SUBSTREAMS) {
            if (parse_substreams(c, st)) return -1;
        } else {
            return -1;
        }
    }
    if (!st->sub_sizes && st->nfolders) {
        st->sub_sizes = (uint64_t *)calloc((size_t)st->nfolders + 1, sizeof(uint64_t));
        if (!st->sub_sizes) return -1;
        for (uint64_t i = 0; i < st->nfolders; i++) st->sub_sizes[i] = st->folders[i].unpack;
        st->nsub_total = st->nfolders;
    }
    return 0;
}

static int is_pkg_name(const uint8_t *name, size_t nchars) {
    static const char ext[] = ".pkg";
    if (nchars < 4) return 0;
    for (size_t t = 0; t < 4; t++) {
        const uint8_t *ch = name + (nchars - 4 + t) * 2;
        int lo = ch[0];
        if (ch[1] != 0) return 0;
        if (lo >= 'A' && lo <= 'Z') lo += 'a' - 'A';
        if (lo != ext[t]) return 0;
    }
    return 1;
}

/* Returns 0 found, 1 not found, -1 malformed, -2 found but not stored raw. */
static int find_pkg(cur_t *c, const streams_t *st, uint32_t index, uint64_t *off, uint64_t *size) {
    uint64_t nfiles, fi = 0, j = 0, acc = 0, pi = 0, packoff = 0, k = 0, seen = 0;
    const uint8_t *names = NULL, *names_end = NULL, *np;
    uint8_t *empty;
    int rc = 1;
    if (rd_num(c, &nfiles) || nfiles > SZ_MAX_ITEMS) return -1;
    empty = (uint8_t *)calloc((size_t)nfiles + 1, 1);
    if (!empty) return -1;
    for (;;) {
        uint64_t type, psz;
        const uint8_t *next;
        if (rd_num(c, &type)) { rc = -1; goto out; }
        if (type == K_END) break;
        if (rd_num(c, &psz) || psz > (uint64_t)(c->end - c->p)) { rc = -1; goto out; }
        next = c->p + psz;
        if (type == K_EMPTY_STREAM) {
            cur_t t = {c->p, next};
            if (rd_bits(&t, nfiles, empty)) { rc = -1; goto out; }
        } else if (type == K_NAMES) {
            if (psz < 1 || *c->p != 0) { rc = -1; goto out; }
            names = c->p + 1;
            names_end = next;
        }
        c->p = next;
    }
    if (!names) goto out;
    np = names;
    for (uint64_t i = 0; i < nfiles; i++) {
        const uint8_t *ns = np;
        size_t nchars;
        uint64_t ssz;
        while (np + 1 < names_end && (np[0] || np[1])) np += 2;
        if (np + 1 >= names_end) { rc = -1; goto out; }
        nchars = (size_t)(np - ns) / 2;
        np += 2;
        if (empty[i]) continue;
        while (fi < st->nfolders && j >= st->folders[fi].nsub) {
            for (uint64_t q = 0; q < st->folders[fi].npacked; q++) {
                if (pi + q >= st->npack) { rc = -1; goto out; }
                packoff += st->pack_sizes[pi + q];
                if (packoff > SZ_MAX_VALUE) { rc = -1; goto out; }
            }
            pi += st->folders[fi].npacked;
            fi++;
            j = 0;
            acc = 0;
        }
        if (fi >= st->nfolders || k >= st->nsub_total) { rc = -1; goto out; }
        ssz = st->sub_sizes[k];
        if (is_pkg_name(ns, nchars)) {
            if (seen == index) {
                const folder_t *f = &st->folders[fi];
                if (!f->raw || pi >= st->npack || st->pack_sizes[pi] != f->unpack) { rc = -2; goto out; }
                *off = SZ_START_HDR_SIZE + st->pack_pos + packoff + acc;
                *size = ssz;
                rc = 0;
                goto out;
            }
            seen++;
        }
        acc += ssz;
        j++;
        k++;
    }
out:
    free(empty);
    return rc;
}

static int parse_header(cur_t *c, uint32_t index, uint64_t *off, uint64_t *size) {
    streams_t st;
    uint64_t id;
    int rc = 1;
    memset(&st, 0, sizeof(st));
    for (;;) {
        if (rd_num(c, &id)) { rc = -1; break; }
        if (id == K_END) break;
        if (id == K_ARCHIVE_PROPS) {
            for (;;) {
                uint64_t type, psz;
                if (rd_num(c, &type)) { rc = -1; goto out; }
                if (type == K_END) break;
                if (rd_num(c, &psz) || skip(c, psz)) { rc = -1; goto out; }
            }
        } else if (id == K_ADDITIONAL) {
            streams_t tmp;
            int r;
            memset(&tmp, 0, sizeof(tmp));
            r = parse_streams(c, &tmp);
            free_streams(&tmp);
            if (r) { rc = -1; break; }
        } else if (id == K_MAIN) {
            if (parse_streams(c, &st)) { rc = -1; break; }
        } else if (id == K_FILES) {
            rc = find_pkg(c, &st, index, off, size);
            break;
        } else {
            rc = -1;
            break;
        }
    }
out:
    free_streams(&st);
    return rc;
}

#define SZ_TAIL_SIZE (256U * 1024U)

typedef struct {
    sevenzip_read_fn rd;
    void *ctx;
    uint8_t *tail;
    uint64_t tail_off, tail_len;
    int tried;
} sz_io_t;

/* Serves reads from a buffer holding the end of the archive, where 7z keeps its headers. */
static ssize_t io_read(sz_io_t *io, uint64_t archive_size, void *dst, size_t n, uint64_t off) {
    if (!io->tried && n < SZ_TAIL_SIZE) {
        uint64_t len = archive_size < SZ_TAIL_SIZE ? archive_size : SZ_TAIL_SIZE;
        io->tried = 1;
        io->tail = (uint8_t *)malloc((size_t)len);
        if (io->tail) {
            if (io->rd(io->ctx, io->tail, (size_t)len, archive_size - len) == (ssize_t)len) {
                io->tail_off = archive_size - len;
                io->tail_len = len;
            } else {
                free(io->tail);
                io->tail = NULL;
            }
        }
    }
    if (io->tail && off >= io->tail_off && n <= io->tail_len && off - io->tail_off <= io->tail_len - n) {
        memcpy(dst, io->tail + (off - io->tail_off), n);
        return (ssize_t)n;
    }
    return io->rd(io->ctx, dst, n, off);
}

static int find_raw_pkg(sz_io_t *io, uint64_t archive_size, uint32_t pkg_index,
                        uint64_t *out_offset, uint64_t *out_size, char *err, size_t err_sz) {
    sevenzip_read_fn rd = io->rd;
    void *ctx = io->ctx;
    static const uint8_t sig[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    uint8_t start[SZ_START_HDR_SIZE];
    uint64_t next_off = 0, next_size = 0, off = 0, size = 0;
    uint8_t *buf = NULL;
    size_t len;
    int rc = -1;
    if (!rd || !out_offset || !out_size) return -1;
    if (archive_size < SZ_START_HDR_SIZE || rd(ctx, start, sizeof(start), 0) != (ssize_t)sizeof(start) ||
        memcmp(start, sig, sizeof(sig)) != 0 || start[6] != 0) {
        set_err(err, err_sz, "Not a 7z archive");
        return -1;
    }
    for (int i = 0; i < 8; i++) {
        next_off |= (uint64_t)start[12 + i] << (8 * i);
        next_size |= (uint64_t)start[20 + i] << (8 * i);
    }
    if (next_size == 0) {
        set_err(err, err_sz, "The 7z archive is empty");
        return -1;
    }
    if (next_size > SZ_HEADER_MAX || next_off > archive_size - SZ_START_HDR_SIZE ||
        next_size > archive_size - SZ_START_HDR_SIZE - next_off) {
        set_err(err, err_sz, "The 7z header is damaged or incomplete");
        return -1;
    }
    len = (size_t)next_size;
    buf = (uint8_t *)malloc(len);
    if (!buf || io_read(io, archive_size, buf, len, SZ_START_HDR_SIZE + next_off) != (ssize_t)len) {
        free(buf);
        set_err(err, err_sz, "Could not read the 7z header");
        return -1;
    }
    for (int depth = 0; depth < 4; depth++) {
        cur_t c = {buf, buf + len};
        uint64_t id;
        streams_t st;
        uint64_t poff, plen;
        uint8_t *nb;
        int ok, lz;
        uint64_t ulen;
        uint8_t props[5];
        if (rd_num(&c, &id)) break;
        if (id == K_HEADER) {
            rc = parse_header(&c, pkg_index, &off, &size);
            goto done;
        }
        if (id != K_ENCODED) break;
        memset(&st, 0, sizeof(st));
        if (parse_streams(&c, &st)) {
            free_streams(&st);
            break;
        }
        ok = st.nfolders == 1 && st.npack >= 1 && st.folders[0].raw && st.pack_sizes[0] == st.folders[0].unpack;
        poff = SZ_START_HDR_SIZE + st.pack_pos;
        plen = st.npack ? st.pack_sizes[0] : 0;
        lz = st.nfolders == 1 && st.npack >= 1 && st.folders[0].lzma && st.folders[0].nout == 1;
        ulen = st.nfolders ? st.folders[0].unpack : 0;
        if (lz) memcpy(props, st.folders[0].props, 5);
        free_streams(&st);
        if (!ok && lz && ulen > 0 && ulen <= SZ_HEADER_MAX && plen > 0 && plen <= SZ_HEADER_MAX &&
            poff <= archive_size && plen <= archive_size - poff) {
            uint8_t *pk = (uint8_t *)malloc((size_t)plen);
            nb = (uint8_t *)malloc((size_t)ulen);
            if (!pk || !nb || io_read(io, archive_size, pk, (size_t)plen, poff) != (ssize_t)plen ||
                lzma_decode(props, pk, (size_t)plen, nb, (size_t)ulen) != 0) {
                free(pk);
                free(nb);
                free(buf);
                set_err(err, err_sz, "Could not decode the compressed 7z header");
                return -1;
            }
            free(pk);
            free(buf);
            buf = nb;
            len = (size_t)ulen;
            continue;
        }
        if (!ok) {
            free(buf);
            set_err(err, err_sz, "The 7z header uses an unsupported compression method");
            return -1;
        }
        if (plen == 0 || plen > SZ_HEADER_MAX || poff > archive_size || plen > archive_size - poff) break;
        nb = (uint8_t *)malloc((size_t)plen);
        if (!nb || io_read(io, archive_size, nb, (size_t)plen, poff) != (ssize_t)plen) {
            free(nb);
            free(buf);
            set_err(err, err_sz, "Could not read the 7z header");
            return -1;
        }
        free(buf);
        buf = nb;
        len = (size_t)plen;
    }
    rc = -1;
done:
    free(buf);
    if (rc == 0) {
        if (off > archive_size || size == 0 || size > archive_size - off) {
            set_err(err, err_sz, "The 7z header is damaged or incomplete");
            return -1;
        }
        *out_offset = off;
        *out_size = size;
        return 0;
    }
    if (rc == 1) set_err(err, err_sz, pkg_index ? "The 7z archive has no .pkg at that pkg_index"
                                                : "No .pkg file found in the 7z archive");
    else if (rc == -2) set_err(err, err_sz, "The .pkg in the 7z archive is compressed; store it uncompressed (-mx0)");
    else set_err(err, err_sz, "Unsupported or damaged 7z header");
    return -1;
}

int sevenzip_find_raw_pkg(sevenzip_read_fn rd, void *ctx, uint64_t archive_size, uint32_t pkg_index,
                          uint64_t *out_offset, uint64_t *out_size, char *err, size_t err_sz) {
    sz_io_t io = {rd, ctx, NULL, 0, 0, 0};
    int r = find_raw_pkg(&io, archive_size, pkg_index, out_offset, out_size, err, err_sz);
    free(io.tail);
    return r;
}
