#include "lzma_dec.h"

#include <stdlib.h>
#include <string.h>

#define PROB_INIT 1024
#define NUM_LEN_TO_POS_STATES 4
#define END_POS_MODEL_INDEX 14
#define NUM_FULL_DISTANCES (1 << (END_POS_MODEL_INDEX >> 1))
#define LITERAL_CODER_SIZE 0x300

typedef uint16_t prob_t;

typedef struct {
    const uint8_t *p, *end;
    uint32_t range, code;
    int err;
} rc_t;

typedef struct {
    prob_t choice, choice2;
    prob_t low[16][8];
    prob_t mid[16][8];
    prob_t high[256];
} len_t;

static void rc_norm(rc_t *r) {
    if (r->range < (1u << 24)) {
        uint8_t b = 0;
        r->range <<= 8;
        if (r->p < r->end) b = *r->p++;
        else r->err = 1;
        r->code = (r->code << 8) | b;
    }
}

static int rc_init(rc_t *r, const uint8_t *in, size_t n) {
    if (n < 5 || in[0] != 0) return -1;
    r->code = ((uint32_t)in[1] << 24) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 8) | in[4];
    r->range = 0xFFFFFFFFu;
    r->p = in + 5;
    r->end = in + n;
    r->err = 0;
    return r->code == r->range ? -1 : 0;
}

static unsigned rc_bit(rc_t *r, prob_t *prob) {
    uint32_t bound = (r->range >> 11) * (*prob);
    unsigned sym;
    if (r->code < bound) {
        r->range = bound;
        *prob = (prob_t)(*prob + ((2048 - *prob) >> 5));
        sym = 0;
    } else {
        r->range -= bound;
        r->code -= bound;
        *prob = (prob_t)(*prob - (*prob >> 5));
        sym = 1;
    }
    rc_norm(r);
    return sym;
}

static uint32_t rc_direct(rc_t *r, unsigned n) {
    uint32_t res = 0;
    while (n--) {
        uint32_t t;
        r->range >>= 1;
        r->code -= r->range;
        t = 0u - (r->code >> 31);
        r->code += r->range & t;
        rc_norm(r);
        res = (res << 1) + t + 1;
    }
    return res;
}

static unsigned bt_dec(rc_t *r, prob_t *probs, unsigned nbits) {
    unsigned m = 1;
    for (unsigned i = 0; i < nbits; i++) m = (m << 1) + rc_bit(r, probs + m);
    return m - (1u << nbits);
}

static unsigned bt_rev(rc_t *r, prob_t *probs, unsigned nbits) {
    unsigned m = 1, sym = 0;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned b = rc_bit(r, probs + m);
        m = (m << 1) + b;
        sym |= b << i;
    }
    return sym;
}

static unsigned len_dec(rc_t *r, len_t *l, unsigned pos_state) {
    if (!rc_bit(r, &l->choice)) return bt_dec(r, l->low[pos_state], 3);
    if (!rc_bit(r, &l->choice2)) return 8 + bt_dec(r, l->mid[pos_state], 3);
    return 16 + bt_dec(r, l->high, 8);
}

static void fill_probs(prob_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = PROB_INIT;
}

static void len_init(len_t *l) {
    fill_probs((prob_t *)l, sizeof(*l) / sizeof(prob_t));
}

int lzma_decode(const uint8_t props[5], const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    unsigned d = props[0], lc, lp, pb, pos_mask, lit_mask;
    prob_t *lit = NULL;
    prob_t is_match[12 << 4], is_rep[12], is_rep_g0[12], is_rep_g1[12], is_rep_g2[12], is_rep0_long[12 << 4];
    prob_t pos_slot[NUM_LEN_TO_POS_STATES][64], pos_dec[1 + NUM_FULL_DISTANCES - END_POS_MODEL_INDEX], align[16];
    len_t len_probs, rep_len_probs;
    rc_t rc;
    unsigned state = 0;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    size_t pos = 0;
    int rc_res = -1;

    if (!props || !in || !out || d >= 9 * 5 * 5) return -1;
    lc = d % 9;
    d /= 9;
    lp = d % 5;
    pb = d / 5;
    pos_mask = (1u << pb) - 1;
    lit_mask = (1u << lp) - 1;
    if (rc_init(&rc, in, in_len) != 0) return -1;

    lit = (prob_t *)malloc(sizeof(prob_t) * ((size_t)LITERAL_CODER_SIZE << (lc + lp)));
    if (!lit) return -1;
    fill_probs(lit, (size_t)LITERAL_CODER_SIZE << (lc + lp));
    fill_probs(is_match, sizeof(is_match) / sizeof(prob_t));
    fill_probs(is_rep, sizeof(is_rep) / sizeof(prob_t));
    fill_probs(is_rep_g0, sizeof(is_rep_g0) / sizeof(prob_t));
    fill_probs(is_rep_g1, sizeof(is_rep_g1) / sizeof(prob_t));
    fill_probs(is_rep_g2, sizeof(is_rep_g2) / sizeof(prob_t));
    fill_probs(is_rep0_long, sizeof(is_rep0_long) / sizeof(prob_t));
    fill_probs(&pos_slot[0][0], sizeof(pos_slot) / sizeof(prob_t));
    fill_probs(pos_dec, sizeof(pos_dec) / sizeof(prob_t));
    fill_probs(align, sizeof(align) / sizeof(prob_t));
    len_init(&len_probs);
    len_init(&rep_len_probs);

    while (pos < out_len) {
        unsigned pos_state = (unsigned)pos & pos_mask;
        unsigned len;
        if (rc.err) goto done;
        if (!rc_bit(&rc, &is_match[(state << 4) + pos_state])) {
            unsigned prev = pos ? out[pos - 1] : 0;
            prob_t *probs = lit + (size_t)LITERAL_CODER_SIZE * ((((unsigned)pos & lit_mask) << lc) + (prev >> (8 - lc)));
            unsigned sym = 1;
            if (state >= 7) {
                unsigned match_byte = out[pos - rep0 - 1];
                do {
                    unsigned match_bit = (match_byte >> 7) & 1, bit;
                    match_byte <<= 1;
                    bit = rc_bit(&rc, &probs[((1 + match_bit) << 8) + sym]);
                    sym = (sym << 1) | bit;
                    if (match_bit != bit) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | rc_bit(&rc, &probs[sym]);
            out[pos++] = (uint8_t)sym;
            state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
            continue;
        }
        if (rc_bit(&rc, &is_rep[state])) {
            if (pos == 0) goto done;
            if (!rc_bit(&rc, &is_rep_g0[state])) {
                if (!rc_bit(&rc, &is_rep0_long[(state << 4) + pos_state])) {
                    if (rep0 >= pos) goto done;
                    state = state < 7 ? 9 : 11;
                    out[pos] = out[pos - rep0 - 1];
                    pos++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc_bit(&rc, &is_rep_g1[state])) {
                    dist = rep1;
                } else {
                    if (!rc_bit(&rc, &is_rep_g2[state])) {
                        dist = rep2;
                    } else {
                        dist = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = len_dec(&rc, &rep_len_probs, pos_state);
            state = state < 7 ? 8 : 11;
        } else {
            unsigned slot;
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = len_dec(&rc, &len_probs, pos_state);
            state = state < 7 ? 7 : 10;
            slot = bt_dec(&rc, pos_slot[len < NUM_LEN_TO_POS_STATES ? len : NUM_LEN_TO_POS_STATES - 1], 6);
            if (slot < 4) {
                rep0 = slot;
            } else {
                unsigned ndirect = (slot >> 1) - 1;
                uint32_t dist = (2u | (slot & 1)) << ndirect;
                if (slot < END_POS_MODEL_INDEX) {
                    dist += bt_rev(&rc, pos_dec + dist - slot, ndirect);
                } else {
                    dist += rc_direct(&rc, ndirect - 4) << 4;
                    dist += bt_rev(&rc, align, 4);
                }
                rep0 = dist;
            }
            if (rep0 == 0xFFFFFFFFu) goto done; /* end marker before the expected size */
        }
        len += 2;
        if (rep0 >= pos || len > out_len - pos) goto done;
        for (unsigned i = 0; i < len; i++, pos++) out[pos] = out[pos - rep0 - 1];
    }
    rc_res = rc.err ? -1 : 0;
done:
    free(lit);
    return rc_res;
}
