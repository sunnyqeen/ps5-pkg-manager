#ifndef INSTALL_QUEUE_H
#define INSTALL_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include "pkg_parser.h"

#define INSTALL_QUEUE_MAX_JOBS 256

typedef enum {
    QUEUE_QUEUED, QUEUE_CHECKING, QUEUE_PREPARING, QUEUE_INSTALLING, QUEUE_CANCELING,
    QUEUE_COMPLETED, QUEUE_SUBMITTED, QUEUE_FAILED, QUEUE_BLOCKED, QUEUE_CANCELED, QUEUE_ABORTED
} install_queue_state_t;

/* Browser files are referenced by an opaque key, never a live upload URI.
 * Only the selected browser job may create/attach an upload session. */
typedef struct {
    char path[1024];
    char owner[65];
    char file_key[512];
    char title_id[32];
    char title_name[256];
    char content_id[64];
    char kind[16];
    char version[32];
    uint64_t total_bytes;
} install_queue_request_t;

#define INSTALL_QUEUE_FINISHED_TTL_DEFAULT 3600

int install_queue_init(void);
void install_queue_shutdown(void);
int install_queue_get_finished_ttl(void);
void install_queue_set_finished_ttl(int seconds);
int install_queue_add(const install_queue_request_t *requests, size_t count, uint64_t *ids);
int install_queue_add_json(const char *json, uint64_t *ids, size_t *count);
int install_queue_add_paths(const char *base, const char *update);
int install_queue_cancel(uint64_t id);
int install_queue_retry(uint64_t id);
void install_queue_clear_finished(void);
void install_queue_heartbeat(const char *owner);
void install_queue_disconnect(const char *owner);
int install_queue_browser_selected(uint64_t id, const char *owner);
int install_queue_attach(uint64_t id, const char *owner, const char *live_path);
int install_queue_open_upload(uint64_t id, const char *owner, const char *filename,
                             uint64_t total, char *sid, size_t sid_size);
char *install_queue_to_json(void);

#endif
