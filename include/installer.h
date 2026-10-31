#ifndef INSTALLER_H
#define INSTALLER_H

/* Up to 1 MiB of recent history for /api/log (2,048 lines x 512 bytes). */
#define INSTALL_LOG_MAX_LINES 2048

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "pkg_parser.h"

#define PKG_DEFAULT_TMP_DIR "/data/pkgmgr/tmp"

typedef struct {
    int is_installing;
    char pkg_path[1024];
    char title_id[32];
    char title_name[256];
    char content_id[64];
    char pkg_kind[16]; /* "base", "update", "dlc", "unknown" (for display naming) */
    char pkg_version[32]; /* PKG APP_VER for update completion gating */
    char status_str[32];
    uint64_t downloaded_bytes;
    uint64_t stream_served_bytes; /* bytes actually sent over our stream (never adopts stale sys values) */
    uint64_t total_bytes;
    float progress_percent;
    int32_t error_code;
    time_t start_time;
    time_t last_poll_time;
    int completed;
    int failed;
    int is_multipart;
    uint32_t current_part;
    uint32_t total_parts;
    int waiting_for_disc;
    int is_direct_storage;
    char prompt_message[256];
} installer_status_t;

#ifdef __cplusplus
extern "C" {
#endif

int installer_init(const char *server_url);
int installer_start(const char *pkg_path);
/* pass:http(s)://... hands that URL to the system installer with no stream.
 * Returns 1 and copies the URL, 0 when path is not passthrough, -1 when the
 * prefix is present but the URL is not http:// or https://. */
int installer_passthrough_url(const char *path, char *out, size_t out_sz);
/* Compatibility entry point: adds two independent jobs to the shared queue. */
int installer_start_batch(const char *base_pkg_path, const char *update_pkg_path);
/* NEW: start from a live RAM session ("live:<id>"); see installer.c. */
int installer_start_live(const char *live_uri);
int installer_cancel(void);
int installer_cancel_path(const char *expected_path);
/* Includes worker and cancellation cleanup, even after status stops installing. */
int installer_is_busy(void);
int installer_detach_direct_storage(int *out_update_skipped);
void installer_record_poll(void);
void installer_get_status(installer_status_t *out);
char *installer_status_to_json(void);
void installer_notify_bytes_streamed(uint64_t bytes_read);
/* Called by a stream reader; records failure without joining its own thread. */
void installer_notify_source_error(const char *expected_path);
const char *installer_strerror(int code);
int installer_is_nospace_error(int code);
void install_log(const char *fmt, ...);
char *install_log_get_text(size_t *out_len);
void install_log_clear(void);
void install_log_set_file_path(const char *path);
void installer_shutdown(void);
int installer_is_filesystem_install(const char *pkg_path, int is_multipart);
int installer_is_network_connected(void);
void installer_set_network_offline_cached(int offline);
int system_get_storage_info(uint64_t *out_free, uint64_t *out_total, uint64_t *out_used);
int system_get_nvme_storage_info(uint64_t *out_free, uint64_t *out_total, uint64_t *out_used);
int system_get_usb_storage_info(uint64_t *out_free, uint64_t *out_total, uint64_t *out_used);

#ifdef __cplusplus
}
#endif

#endif /* INSTALLER_H */
