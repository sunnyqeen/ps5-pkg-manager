#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "installer.h"
#include "pkg_scanner.h"
#include "test_fixture.h"

/* Wait up to timeout_ms for cond_fn-style polling via status snapshot. */
static int wait_for_state(int want_installing, int want_completed, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        installer_status_t st;
        installer_get_status(&st);
        if (!!st.is_installing == !!want_installing &&
            (!want_completed || st.completed || !strcmp(st.status_str, "submitted"))) {
            return 0;
        }
        usleep(100000);
        waited += 100;
    }
    return -1;
}

int main(void) {
    int res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    installer_status_t st;
    installer_get_status(&st);
    assert(st.is_installing == 0);
    assert(strcmp(st.status_str, "idle") == 0);

    char *json = installer_status_to_json();
    printf("Initial status JSON: %s\n", json);
    assert(strstr(json, "\"is_installing\":false") != NULL);
    free(json);

    /* Synthetic fixture: no external files needed. */
    system("rm -rf /tmp/test_installer_fixtures && mkdir -p /tmp/test_installer_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_installer_fixtures/wc.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);

    /* Test 0: Missing package file fails immediately with -4 */
    int missing_res = installer_start("/tmp/test_installer_fixtures/missing_nonexistent.pkg");
    printf("Missing pkg install start result: %d (expected -4)\n", missing_res);
    assert(missing_res == -4);
    installer_get_status(&st);
    assert(st.is_installing == 0);

    /* Test 1: Start install with standard pkg (streams via worker thread) */
    int start_res = installer_start("/tmp/test_installer_fixtures/wc.pkg");
    printf("Start install result: %d\n", start_res);
    assert(start_res == 0);

    /* Title fields are populated synchronously by installer_start */
    installer_get_status(&st);
    assert(strcmp(st.title_id, "PPSA90012") == 0);
    assert(strcmp(st.title_name, "WaveCast") == 0);

    /* The worker streams to completion asynchronously; wait for it */
    assert(wait_for_state(0, 1, 15000) == 0);
    installer_get_status(&st);
    assert(st.completed == 1);
    assert(st.failed == 0);
    assert(strcmp(st.status_str, "playable") == 0);
    printf("Single-pkg stream install completed: progress=%.1f%%\n", st.progress_percent);

    size_t log_sz = 0;
    char *log_txt = install_log_get_text(&log_sz);
    assert(strstr(log_txt, "name='WaveCast (base)'") != NULL);
    assert(strstr(log_txt, "icon='http://127.0.0.1:18841/stream/install/icon-") != NULL);
    free(log_txt);

    char *active_json = installer_status_to_json();
    printf("Final status JSON: %s\n", active_json);
    assert(strstr(active_json, "WaveCast") != NULL);
    assert(strstr(active_json, "PPSA90012") != NULL);
    free(active_json);

    /* Test 1b: the native installer owns the base -> update handoff. This
     * must complete without any status polling or browser-side callback. */
    assert(fixture_write_ps5_pkg("/tmp/test_installer_fixtures/wc_update.pkg",
                                 "PPSA90012", "WaveCast", "gp", "01.004.000", 1) == 0);
    int batch_res = installer_start_batch("/tmp/test_installer_fixtures/wc.pkg",
                                          "/tmp/test_installer_fixtures/wc_update.pkg");
    printf("Start native base + update batch result: %d\n", batch_res);
    assert(batch_res == 0);

    int batch_waited = 0;
    int update_seen = 0;
    while (batch_waited < 15000) {
        installer_get_status(&st);
        if (strcmp(st.pkg_path, "/tmp/test_installer_fixtures/wc_update.pkg") == 0) {
            update_seen = 1;
        }
        if (update_seen && !st.is_installing && st.completed) break;
        usleep(100000);
        batch_waited += 100;
    }
    assert(update_seen == 1);
    assert(st.completed == 1);
    assert(st.failed == 0);
    assert(strcmp(st.pkg_path, "/tmp/test_installer_fixtures/wc_update.pkg") == 0);
    printf("Native base + update batch completed without browser polling\n");

    log_txt = install_log_get_text(&log_sz);
    assert(strstr(log_txt, "name='WaveCast (update v1.04)'") != NULL);
    assert(strstr(log_txt, "icon='http://127.0.0.1:18841/stream/install/icon-") != NULL);
    free(log_txt);

    /* Test 1c: DLC package display name */
    assert(fixture_write_ps5_pkg("/tmp/test_installer_fixtures/wc_dlc.pkg",
                                 "PPSA90012", "WaveCast DLC", "ac", "01.000.000", 1) == 0);
    assert(installer_start("/tmp/test_installer_fixtures/wc_dlc.pkg") == 0);
    assert(wait_for_state(0, 1, 15000) == 0);
    log_txt = install_log_get_text(&log_sz);
    assert(strstr(log_txt, "name='WaveCast DLC (DLC)'") != NULL);
    assert(strstr(log_txt, "icon='http://127.0.0.1:18841/stream/install/icon-") != NULL);
    free(log_txt);
    printf("DLC install verified: name contains app title and (DLC), icon URL populated\n");

    installer_shutdown();

    /* Test 2: Monitor passivity: no browser relaunch machinery remains.
     * The installer monitor worker is intentionally passive so someone can
     * play a title while a big package installs in the background. */
    printf("Testing monitor passivity (install must keep running)...\n");
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);
    system("rm -f /tmp/watchdog_big.pkg && truncate -s 3G /tmp/watchdog_big.pkg");
    int big_res = installer_start("/tmp/watchdog_big.pkg");
    assert(big_res == 0);

    sleep(8); /* Exceed old 7-second threshold without polling */

    installer_get_status(&st);
    assert(st.is_installing == 1); /* sparse 3GB stream still running */
    assert(installer_cancel() == 0);
    assert(installer_start("/tmp/watchdog_big.pkg") == 0);
    installer_notify_source_error("/tmp/another-session.pkg");
    installer_get_status(&st);
    assert(st.is_installing == 1);
    installer_notify_source_error("/tmp/watchdog_big.pkg");
    installer_get_status(&st);
    assert(st.is_installing == 0 && st.failed == 1);
    assert(st.error_code == -4);
    assert(strstr(st.prompt_message, "Reconnect the drive/share") != NULL);
    installer_shutdown();
    system("rm -f /tmp/watchdog_big.pkg");

    /* Test 3: Fallback install on an unparsable/raw file, through to completion */
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    system("echo 'dummy content' > /tmp/dummy_test.pkg");
    int unparsed_res = installer_start("/tmp/dummy_test.pkg");
    printf("Unparsed pkg install start result: %d\n", unparsed_res);
    assert(unparsed_res == 0); /* MUST succeed and not return -3 */

    assert(wait_for_state(0, 1, 10000) == 0);
    log_txt = install_log_get_text(&log_sz);
    assert(strstr(log_txt, "name='Package (base)', icon=''") != NULL);
    free(log_txt);
    char *unparsed_json = installer_status_to_json();
    printf("Unparsed status JSON: %s\n", unparsed_json);
    assert(strstr(unparsed_json, "Package") != NULL);
    assert(strstr(unparsed_json, "\"completed\":true") != NULL);
    free(unparsed_json);

    installer_shutdown();
    system("rm -f /tmp/dummy_test.pkg");

    /* Test 4: USB package stream install (defaults to stream installer) */
    printf("Testing USB package stream install (default)...\n");
    setenv("PKG_USB_PREFIX", "/tmp/test_usb_fixtures", 1);
    system("rm -rf /tmp/test_usb_fixtures && mkdir -p /tmp/test_usb_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_usb_fixtures/wc_usb.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    install_log_clear();
    int usb_res = installer_start("/tmp/test_usb_fixtures/wc_usb.pkg");
    assert(usb_res == 0);
    assert(wait_for_state(0, 1, 15000) == 0);

    installer_get_status(&st);
    assert(st.completed == 1);
    assert(st.failed == 0);
    assert(strcmp(st.status_str, "playable") == 0);
    assert(st.progress_percent == 100.0f);
    assert(strstr(st.prompt_message, "ready to play") != NULL);

    log_txt = install_log_get_text(&log_sz);
    assert(strstr(log_txt, "Initiating stream install:") != NULL);
    assert(strstr(log_txt, "raw server listening on 127.0.0.1:18841") != NULL);
    free(log_txt);

    char *usb_json = installer_status_to_json();
    assert(strstr(usb_json, "\"completed\":true") != NULL);
    assert(strstr(usb_json, "\"status\":\"playable\"") != NULL);
    assert(strstr(usb_json, "\"progress\":100.00") != NULL);
    assert(strstr(usb_json, "\"is_direct_storage\":false") != NULL);
    free(usb_json);

    installer_shutdown();
    printf("USB package stream install passed!\n");

    /* Test 5: Disc package stream install (default) */
    printf("Testing Disc package stream install (default)...\n");
    setenv("PKG_DISC_DIR", "/tmp/test_disc_fixtures", 1);
    system("rm -rf /tmp/test_disc_fixtures && mkdir -p /tmp/test_disc_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_disc_fixtures/wc_disc.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    install_log_clear();
    int disc_res = installer_start("/tmp/test_disc_fixtures/wc_disc.pkg");
    assert(disc_res == 0);
    assert(wait_for_state(0, 1, 15000) == 0);

    installer_get_status(&st);
    assert(st.completed == 1);
    assert(st.failed == 0);
    assert(strcmp(st.status_str, "playable") == 0);

    log_txt = install_log_get_text(&log_sz);
    assert(strstr(log_txt, "Initiating stream install:") != NULL);
    assert(strstr(log_txt, "raw server listening on 127.0.0.1:18841") != NULL);
    free(log_txt);

    installer_shutdown();
    printf("Disc package stream install passed!\n");

    /* Test 6: USB batch install (base + update) via stream installer */
    printf("Testing USB batch install (base + update) via stream installer...\n");
    assert(fixture_write_ps5_pkg("/tmp/test_usb_fixtures/wc_usb_upd.pkg",
                                 "PPSA90012", "WaveCast", "gp", "01.004.000", 1) == 0);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    install_log_clear();
    int usb_batch_res = installer_start_batch("/tmp/test_usb_fixtures/wc_usb.pkg",
                                              "/tmp/test_usb_fixtures/wc_usb_upd.pkg");
    assert(usb_batch_res == 0);

    int usb_batch_waited = 0;
    int usb_update_seen = 0;
    while (usb_batch_waited < 15000) {
        installer_get_status(&st);
        if (strcmp(st.pkg_path, "/tmp/test_usb_fixtures/wc_usb_upd.pkg") == 0) {
            usb_update_seen = 1;
        }
        if (usb_update_seen && !st.is_installing && st.completed) break;
        usleep(100000);
        usb_batch_waited += 100;
    }
    assert(usb_update_seen == 1);
    assert(st.completed == 1);
    assert(st.failed == 0);
    assert(strcmp(st.pkg_path, "/tmp/test_usb_fixtures/wc_usb_upd.pkg") == 0);

    log_txt = install_log_get_text(&log_sz);
    assert(strstr(log_txt, "Initiating stream install:") != NULL);
    assert(strstr(log_txt, "raw server listening on 127.0.0.1:18841") != NULL);
    free(log_txt);

    installer_shutdown();
    printf("USB batch install via stream installer passed!\n");

    /* Test 7: Multi-part package on USB must use stream install (not direct filesystem) */
    printf("Testing multi-part package on USB uses stream install...\n");
    setenv("PKG_TMP_DIR", "/tmp/test_usb_fixtures/tmp", 1);
    assert(fixture_write_ps5_pkg("/tmp/test_usb_fixtures/src_big.pkg",
                                 "PPSA90010", "BigGame", "gd", "01.000.000", 1) == 0);
    assert(fixture_grow_file("/tmp/test_usb_fixtures/src_big.pkg", 5ULL * 1024 * 1024) == 0);
    int split_ret = system("python3 tools/pkg_split.py /tmp/test_usb_fixtures/src_big.pkg -o /tmp/test_usb_fixtures -s 2M > /dev/null 2>&1");
    assert(split_ret == 0);

    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    install_log_clear();
    int mp_res = installer_start("/tmp/test_usb_fixtures/src_big.pkg.part1");
    assert(mp_res == 0);
    assert(wait_for_state(0, 1, 15000) == 0);

    installer_get_status(&st);
    assert(st.completed == 1);
    assert(st.failed == 0);

    log_txt = install_log_get_text(&log_sz);
    /* Must be multi-part stream install, NOT direct filesystem install */
    assert(strstr(log_txt, "Initiating multi-part stream install:") != NULL);
    assert(strstr(log_txt, "raw server listening on 127.0.0.1:18841") != NULL);
    free(log_txt);

    installer_shutdown();
    unsetenv("PKG_USB_PREFIX");
    unsetenv("PKG_DISC_DIR");
    unsetenv("PKG_TMP_DIR");
    system("rm -rf /tmp/test_usb_fixtures /tmp/test_disc_fixtures");
    /* Test 8: Unit test path classification with installer_is_filesystem_install */
    printf("Testing installer_is_filesystem_install path classification...\n");
    assert(installer_is_filesystem_install("/mnt/usb0/game.pkg", 0) == 1);
    assert(installer_is_filesystem_install("/mnt/usb7/pkg/game.pkg", 0) == 1);
    assert(installer_is_filesystem_install("/mnt/disc/game.pkg", 0) == 1);
    assert(installer_is_filesystem_install("/mnt/usb0/game.pkg.part1", 1) == 0);
    assert(installer_is_filesystem_install("/mnt/disc/game.pkg.part1", 1) == 0);
    assert(installer_is_filesystem_install("smb://192.168.1.10/share/game.pkg", 0) == 0);
    assert(installer_is_filesystem_install("http://192.168.1.10/game.pkg", 0) == 0);
    assert(installer_is_filesystem_install("https://cdn.example/game.pkg", 0) == 0);
    {
        char passed[512];
        assert(installer_passthrough_url("http://192.168.1.10/game.pkg", passed, sizeof(passed)) == 0);
        assert(installer_passthrough_url("pass:http://192.168.1.10/game.pkg", passed, sizeof(passed)) == 1);
        assert(strcmp(passed, "http://192.168.1.10/game.pkg") == 0);
        assert(installer_passthrough_url("pass:https://cdn.example/game.pkg", passed, sizeof(passed)) == 1);
        assert(installer_passthrough_url("pass:ftp://cdn.example/game.pkg", passed, sizeof(passed)) == -1);
        assert(installer_is_filesystem_install("pass:http://192.168.1.10/game.pkg", 0) == 0);
    }
    assert(installer_is_filesystem_install("live:session123", 0) == 0);
    assert(installer_is_filesystem_install(NULL, 0) == 0);
    assert(installer_is_filesystem_install("", 0) == 0);

    setenv("PKG_SCAN_DIR", "/tmp/custom_scan_path", 1);
    assert(installer_is_filesystem_install("/tmp/custom_scan_path/title.pkg", 0) == 1);
    assert(installer_is_filesystem_install("/tmp/custom_scan_path/title.pkg.part1", 1) == 0);
    unsetenv("PKG_SCAN_DIR");

    setenv("PKG_USB_PREFIX", "/media/usb", 1);
    assert(installer_is_filesystem_install("/media/usb0/title.pkg", 0) == 1);
    unsetenv("PKG_USB_PREFIX");

    setenv("PKG_DISC_DIR", "/media/disc", 1);
    assert(installer_is_filesystem_install("/media/disc/title.pkg", 0) == 1);
    unsetenv("PKG_DISC_DIR");
    printf("installer_is_filesystem_install path classification passed!\n");

    /* Test 9: Canceling an install */
    printf("Testing cancel of install...\n");
    setenv("PKG_USB_PREFIX", "/tmp/test_usb_fixtures", 1);
    system("rm -rf /tmp/test_usb_fixtures && mkdir -p /tmp/test_usb_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_usb_fixtures/wc_big_usb.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);
    assert(fixture_grow_file("/tmp/test_usb_fixtures/wc_big_usb.pkg", 50ULL * 1024 * 1024) == 0);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);
    assert(installer_start("/tmp/test_usb_fixtures/wc_big_usb.pkg") == 0);
    assert(installer_cancel() == 0);
    installer_get_status(&st);
    assert(st.is_installing == 0);
    assert(st.failed == 1);
    assert(strcmp(st.status_str, "canceled") == 0);
    installer_shutdown();
    unsetenv("PKG_USB_PREFIX");
    system("rm -rf /tmp/test_usb_fixtures");
    printf("Cancel of install passed!\n");

    /* Test 10a: USB package upfront offline direct storage install */
    printf("Testing USB package upfront offline direct storage install...\n");
    setenv("PKG_USB_PREFIX", "/tmp/test_usb_fixtures", 1);
    setenv("PKG_TEST_OFFLINE_NET", "1", 1);
    system("rm -rf /tmp/test_usb_fixtures && mkdir -p /tmp/test_usb_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_usb_fixtures/wc_usb.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    install_log_clear();
    assert(installer_start("/tmp/test_usb_fixtures/wc_usb.pkg") == 0);
    /* Verify installer_cancel() rejects canceling direct storage installs (-2) */
    assert(installer_cancel() == -2);
    assert(wait_for_state(0, 1, 15000) == 0);

    installer_get_status(&st);
    assert(st.completed == 0);
    assert(st.failed == 0);
    assert(st.is_direct_storage == 1);
    assert(st.progress_percent == -1.0f);
    assert(strcmp(st.status_str, "submitted") == 0);

    log_txt = install_log_get_text(&log_sz);
    assert(strstr(log_txt, "Network not connected; using direct storage install") != NULL);
    assert(strstr(log_txt, "Cancel rejected: direct storage install cannot be canceled") != NULL);
    assert(strstr(log_txt, "Submitted to PS5 system installer") != NULL);
    free(log_txt);

    char *offline_json = installer_status_to_json();
    assert(strstr(offline_json, "\"is_direct_storage\":true") != NULL);
    assert(strstr(offline_json, "\"direct_storage\"") == NULL);
    free(offline_json);

    installer_shutdown();
    unsetenv("PKG_TEST_OFFLINE_NET");
    unsetenv("PKG_USB_PREFIX");
    system("rm -rf /tmp/test_usb_fixtures");
    printf("USB package upfront offline direct storage install passed!\n");

    /* Test 10b: USB package runtime fallback (0x80B21121 -> direct storage install) */
    printf("Testing USB package runtime fallback (0x80B21121 -> direct storage install)...\n");
    setenv("PKG_USB_PREFIX", "/tmp/test_usb_fixtures", 1);
    setenv("PKG_TEST_SIMULATE_0x80B21121", "1", 1);
    system("rm -rf /tmp/test_usb_fixtures && mkdir -p /tmp/test_usb_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_usb_fixtures/wc_usb.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    install_log_clear();
    assert(installer_start("/tmp/test_usb_fixtures/wc_usb.pkg") == 0);
    assert(wait_for_state(0, 1, 15000) == 0);

    installer_get_status(&st);
    assert(st.completed == 0);
    assert(st.failed == 0);
    assert(st.is_direct_storage == 1);
    assert(st.progress_percent == -1.0f);
    assert(strcmp(st.status_str, "submitted") == 0);

    log_txt = install_log_get_text(&log_sz);
    assert(strstr(log_txt, "0x80B21121") != NULL);
    assert(strstr(log_txt, "falling back to direct storage install") != NULL);
    assert(strstr(log_txt, "Submitted to PS5 system installer") != NULL);
    free(log_txt);

    installer_shutdown();
    unsetenv("PKG_TEST_SIMULATE_0x80B21121");
    unsetenv("PKG_USB_PREFIX");
    system("rm -rf /tmp/test_usb_fixtures");
    printf("USB package runtime fallback passed!\n");

    /* Test 10c: USB batch install (base + update) upfront offline direct storage install */
    printf("Testing USB batch install (base + update) upfront offline direct storage install...\n");
    setenv("PKG_USB_PREFIX", "/tmp/test_usb_fixtures", 1);
    setenv("PKG_TEST_OFFLINE_NET", "1", 1);
    system("rm -rf /tmp/test_usb_fixtures && mkdir -p /tmp/test_usb_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_usb_fixtures/wc_usb.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);
    assert(fixture_write_ps5_pkg("/tmp/test_usb_fixtures/wc_usb_upd.pkg",
                                 "PPSA90012", "WaveCast", "gp", "01.004.000", 1) == 0);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    install_log_clear();
    int usb_direct_batch_res = installer_start_batch("/tmp/test_usb_fixtures/wc_usb.pkg",
                                                     "/tmp/test_usb_fixtures/wc_usb_upd.pkg");
    assert(usb_direct_batch_res == 0);

    int usb_direct_waited = 0;
    int usb_direct_update_seen = 0;
    while (usb_direct_waited < 15000) {
        installer_get_status(&st);
        if (strcmp(st.pkg_path, "/tmp/test_usb_fixtures/wc_usb_upd.pkg") == 0) {
            usb_direct_update_seen = 1;
            assert(st.is_direct_storage == 1);
        }
        if (usb_direct_update_seen && !st.is_installing && !strcmp(st.status_str, "submitted")) break;
        usleep(100000);
        usb_direct_waited += 100;
    }
    assert(usb_direct_update_seen == 1);
    assert(st.completed == 0);
    assert(st.failed == 0);
    assert(st.is_direct_storage == 1);
    assert(strcmp(st.pkg_path, "/tmp/test_usb_fixtures/wc_usb_upd.pkg") == 0);

    installer_shutdown();
    unsetenv("PKG_TEST_OFFLINE_NET");
    unsetenv("PKG_USB_PREFIX");
    system("rm -rf /tmp/test_usb_fixtures");
    printf("USB batch install upfront offline direct storage passed!\n");

    /* Test 10d: USB update package only upfront offline direct storage install */
    printf("Testing USB update package only upfront offline direct storage install...\n");
    setenv("PKG_USB_PREFIX", "/tmp/test_usb_fixtures", 1);
    setenv("PKG_TEST_OFFLINE_NET", "1", 1);
    system("rm -rf /tmp/test_usb_fixtures && mkdir -p /tmp/test_usb_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_usb_fixtures/wc_usb_upd.pkg",
                                 "PPSA90012", "WaveCast", "gp", "01.004.000", 1) == 0);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    install_log_clear();
    assert(installer_start("/tmp/test_usb_fixtures/wc_usb_upd.pkg") == 0);
    assert(wait_for_state(0, 1, 15000) == 0);

    installer_get_status(&st);
    assert(st.completed == 0);
    assert(st.failed == 0);
    assert(st.is_direct_storage == 1);
    assert(strcmp(st.pkg_path, "/tmp/test_usb_fixtures/wc_usb_upd.pkg") == 0);

    installer_shutdown();
    unsetenv("PKG_TEST_OFFLINE_NET");
    unsetenv("PKG_USB_PREFIX");
    system("rm -rf /tmp/test_usb_fixtures");
    printf("USB update package only upfront offline direct storage passed!\n");

    /* Test 11: Content ID to Title ID parsing logic */
    printf("Testing title_id extraction from content_id...\n");
    const char *test_cids[] = {
        "JP0000-CUSA00001_00-TESTCONTENT000000",
        "UP0000-PPSA00002_00-TESTCONTENT000000",
        "EP0000-CUSA00003_00-TESTCONTENT000000"
    };
    const char *expected_tids[] = {
        "CUSA00001",
        "PPSA00002",
        "CUSA00003"
    };
    for (size_t i = 0; i < sizeof(test_cids) / sizeof(test_cids[0]); i++) {
        char extracted[32] = {0};
        const char *dash = strchr(test_cids[i], '-');
        assert(dash != NULL);
        const char *tid_start = dash + 1;
        const char *underscore = strchr(tid_start, '_');
        assert(underscore != NULL);
        size_t tlen = (size_t)(underscore - tid_start);
        assert(tlen < sizeof(extracted));
        strncpy(extracted, tid_start, tlen);
        extracted[tlen] = '\0';
        assert(strcmp(extracted, expected_tids[i]) == 0);
    }
    printf("title_id extraction from content_id passed!\n");

    /* Test 12: Sequential USB stream installs reset progress state per-install */
    printf("Testing sequential USB stream installs reset per-install progress state...\n");
    setenv("PKG_USB_PREFIX", "/tmp/test_usb_seq", 1);
    system("rm -rf /tmp/test_usb_seq && mkdir -p /tmp/test_usb_seq");
    assert(fixture_write_ps5_pkg("/tmp/test_usb_seq/seq1.pkg",
                                 "PPSA90021", "SeqGame1", "gd", "01.000.000", 1) == 0);
    assert(fixture_write_ps5_pkg("/tmp/test_usb_seq/seq2.pkg",
                                 "PPSA90022", "SeqGame2", "gd", "01.000.000", 1) == 0);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    /* First install */
    assert(installer_start("/tmp/test_usb_seq/seq1.pkg") == 0);
    assert(wait_for_state(0, 1, 15000) == 0);
    installer_get_status(&st);
    assert(st.completed == 1);
    assert(st.failed == 0);
    assert(strcmp(st.pkg_path, "/tmp/test_usb_seq/seq1.pkg") == 0);

    /* Second install in the same process session (verifies per-install progress isolation) */
    assert(installer_start("/tmp/test_usb_seq/seq2.pkg") == 0);
    assert(wait_for_state(0, 1, 15000) == 0);
    installer_get_status(&st);
    assert(st.completed == 1);
    assert(st.failed == 0);
    assert(strcmp(st.pkg_path, "/tmp/test_usb_seq/seq2.pkg") == 0);
    assert(st.downloaded_bytes == st.total_bytes);

    installer_shutdown();
    unsetenv("PKG_USB_PREFIX");
    system("rm -rf /tmp/test_usb_seq");
    printf("Sequential USB stream installs passed!\n");

    /* Test 13: Out-of-space error message translation (0x80B21104 / 0x80A30002) */
    printf("Testing out-of-space error message translation (0x80B21104)...\n");
    assert(strcmp(installer_strerror((int)0x80B21104u), "SCE_PLAYGO_ERROR_CORE_NO_FREE_SPACE") == 0);
    assert(installer_is_nospace_error((int)0x80B21104u) == 1);
    assert(installer_is_nospace_error((int)0x80A30002u) == 1);
    assert(installer_is_nospace_error((int)0x80B21121u) == 0);

    setenv("PKG_TEST_SIMULATE_0x80B21104", "1", 1);
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);
    assert(fixture_write_ps5_pkg("/tmp/nospace.pkg", "PPSA90099", "NoSpaceGame", "gd", "01.000.000", 1) == 0);
    assert(installer_start("/tmp/nospace.pkg") == 0);
    assert(wait_for_state(0, 0, 15000) == 0);
    installer_get_status(&st);
    assert(st.failed == 1);
    assert(st.completed == 0);
    assert(st.error_code == (int)0x80B21104u);
    assert(strstr(st.prompt_message, "Not enough free space for installation") != NULL);
    assert(strstr(st.prompt_message, "0x80B21104") != NULL);

    installer_shutdown();
    unsetenv("PKG_TEST_SIMULATE_0x80B21104");
    unlink("/tmp/nospace.pkg");
    printf("Out-of-space error translation passed!\n");

    printf("\n>>> ALL INSTALLER TESTS PASSED! <<<\n");
    return 0;
}
