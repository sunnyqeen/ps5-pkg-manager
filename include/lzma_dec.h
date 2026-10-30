#ifndef LZMA_DEC_H
#define LZMA_DEC_H

#include <stddef.h>
#include <stdint.h>

/* Minimal one-shot LZMA1 decoder for 7z encoded headers.
 * props is the 5-byte coder property (lc/lp/pb byte + dictionary size).
 * The whole input and exactly out_len output bytes are held in memory, so the
 * output buffer doubles as the dictionary. Returns 0 on success, -1 on error. */
int lzma_decode(const uint8_t props[5], const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

#endif
