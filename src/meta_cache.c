#include "meta_cache.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define META_KEY_MAX 512
#define META_VALIDATOR_MAX 32

typedef struct {
    uint8_t *data;
    uint64_t off;
    size_t len;
    uint64_t size;
    uint64_t stamp;
    char key[META_KEY_MAX];
    char validator[META_VALIDATOR_MAX];
} meta_slot_t;

static meta_slot_t g_slots[META_CACHE_SLOTS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_clock;

ssize_t meta_cache_read(const char *key, uint64_t size, const char *validator,
                        void *buf, size_t count, uint64_t offset,
                        meta_cache_fetch_fn fetch, void *ctx) {
    meta_slot_t *m = NULL, *v = NULL;
    if (!key || !buf || !fetch || count == 0 || count > META_CACHE_MAX_READ || size == 0 ||
        offset >= size || count > size - offset || strlen(key) >= META_KEY_MAX)
        return -2;
    if (!validator) validator = "";

    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < META_CACHE_SLOTS; i++) {
        meta_slot_t *c = &g_slots[i];
        if (c->len == 0 || strcmp(c->key, key) != 0) continue;
        if (c->size != size || strncmp(c->validator, validator, META_VALIDATOR_MAX - 1) != 0) {
            c->len = 0;
            c->stamp = 0;
            continue;
        }
        if (!m && offset >= c->off && offset + count <= c->off + c->len) m = c;
    }
    if (!m) {
        size_t blk = META_CACHE_BLOCK;
        ssize_t n;
        for (int i = 0; i < META_CACHE_SLOTS; i++)
            if (!v || g_slots[i].stamp < v->stamp) v = &g_slots[i];
        if (blk > size - offset) blk = (size_t)(size - offset);
        if (!v->data) v->data = (uint8_t *)malloc(META_CACHE_BLOCK);
        if (!v->data) {
            pthread_mutex_unlock(&g_mu);
            return -2;
        }
        v->len = 0;
        n = fetch(ctx, v->data, blk, offset);
        if (n < 0) {
            pthread_mutex_unlock(&g_mu);
            return -1;
        }
        if ((size_t)n < count) {
            pthread_mutex_unlock(&g_mu);
            return -2;
        }
        v->off = offset;
        v->len = (size_t)n;
        v->size = size;
        strcpy(v->key, key);
        strncpy(v->validator, validator, META_VALIDATOR_MAX - 1);
        v->validator[META_VALIDATOR_MAX - 1] = 0;
        m = v;
    }
    m->stamp = ++g_clock;
    memcpy(buf, m->data + (offset - m->off), count);
    pthread_mutex_unlock(&g_mu);
    return (ssize_t)count;
}
