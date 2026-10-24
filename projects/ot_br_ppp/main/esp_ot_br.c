/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * OpenThread Border Router with a PPP-over-USB backbone.
 *
 * Derived from esp-idf/examples/openthread/ot_br (CC0-1.0). The differences:
 *
 *   - single SoC: the ESP32-C6's own 802.15.4 radio is the Thread radio
 *     (CONFIG_OPENTHREAD_RADIO_NATIVE), so there is no RCP and no UART spinel
 *     link, and none of the RCP reset/coexistence plumbing is needed;
 *   - the backbone ("infrastructure") interface is a PPP link over the native
 *     USB Serial/JTAG port instead of Wi-Fi or Ethernet, so example_connect()
 *     and the ot_examples_br component are replaced by ppp_modem.c;
 *   - the Thread mesh is brought up independently of the backbone, so the
 *     network keeps running when the upstream router is unplugged.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "openthread/border_routing.h"
#include "ot_examples_common.h"
#include "ppp_modem.h"

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
#endif

#define TAG "esp_ot_br_ppp"

/* The prebuilt libopenthread_br.a reads netif->flags and netif->num at fixed
 * offsets, taken from esp_openthread_infra_if_init() in
 * lib/esp32c6/libopenthread_br.a (esp-thread-lib b7bc06a). Any config that
 * changes struct netif ahead of them (PPP adds a client_data slot, for one)
 * makes the border router silently never start. See sdkconfig.defaults. If
 * ESP-IDF updates the library, re-check these against its disassembly. */
_Static_assert(offsetof(struct netif, flags) == 527 && offsetof(struct netif, num) == 530,
               "struct netif layout no longer matches libopenthread_br.a");

/* ------------------------------------------------------------------ */
/* Wall-clock time                                                     */
/* ------------------------------------------------------------------ */

#if CONFIG_OT_BR_SNTP_ENABLE
static void start_sntp(void)
{
    /* Only reachable once the PPP link is up and the upstream router is
     * forwarding. Thread nodes on the mesh can either run their own SNTP
     * client through this border router, or take network time from the mesh
     * itself when CONFIG_OPENTHREAD_TIME_SYNC is enabled on every node. */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_OT_BR_SNTP_SERVER);
    config.start = true;
    config.server_from_dhcp = false;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return;
    }

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(CONFIG_OT_BR_SNTP_SYNC_TIMEOUT_MS)) == ESP_OK) {
        time_t now = 0;
        time(&now);
        ESP_LOGI(TAG, "time synchronised: %s", ctime(&now));
    } else {
        ESP_LOGW(TAG, "SNTP did not sync within %d ms; it will keep retrying",
                 CONFIG_OT_BR_SNTP_SYNC_TIMEOUT_MS);
    }
}
#endif /* CONFIG_OT_BR_SNTP_ENABLE */

/* ------------------------------------------------------------------ */
/* Border router bring-up                                              */
/* ------------------------------------------------------------------ */

static void br_backbone_task(void *ctx)
{
    (void)ctx;

    ESP_ERROR_CHECK(ppp_modem_start());

    /* Block until the upstream router has answered. Without a backbone
     * there is nothing for the border routing logic to attach to. */
    ESP_LOGI(TAG, "waiting for the PPP link...");
    ESP_ERROR_CHECK(ppp_modem_wait_connected(0 /* wait forever */));

    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_openthread_set_backbone_netif(ppp_modem_get_netif());
    esp_err_t err = esp_openthread_border_router_init();
    if (err == ESP_OK) {
        /* Ask the router to delegate the mesh's prefix (DHCPv6-PD), so the
         * router decides the addressing on the Thread side too. Without a
         * delegation the border router falls back to its own ULA prefix. */
        otBorderRoutingDhcp6PdSetEnabled(esp_openthread_get_instance(), true);
    }
    esp_openthread_lock_release();

    if (err != ESP_OK) {
        /* For example if the prebuilt libopenthread_br.a rejects the PPP
         * netif. Keep the mesh and the CLI alive so the failure can be
         * inspected. */
        ESP_LOGE(TAG, "esp_openthread_border_router_init() failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Thread mesh and CLI stay up; backbone functions are unavailable.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "border router active on the PPP backbone");

#if CONFIG_OT_BR_SNTP_ENABLE
    start_sntp();
#endif

    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    /* Used eventfds:
     *  - netif
     *  - task queue
     *  - border router
     *  - radio driver (native radio) */
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 4,
    };

    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* mDNS is how the border router advertises Thread services (registered by
     * mesh nodes over SRP) onto the backbone. Note that over a point-to-point
     * PPP link those announcements reach the upstream router and stop there:
     * there is no shared segment behind it for them to propagate onto. */
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_OT_BR_MDNS_HOSTNAME));

#if CONFIG_OPENTHREAD_CLI
    ot_console_start();
    ot_register_external_commands();
#endif

    static esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
            .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
            .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
        },
    };
    ESP_ERROR_CHECK(esp_openthread_start(&config));

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_cli_custom_command_init();
#endif
#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
    ESP_ERROR_CHECK(esp_openthread_state_indicator_init(esp_openthread_get_instance()));
#endif

    /* Form/join the mesh straight away, independent of the backbone. */
#if CONFIG_OPENTHREAD_NETWORK_AUTO_START
    ot_network_auto_start();
#endif

    /* Backbone and border routing come up asynchronously. */
    xTaskCreate(br_backbone_task, "br_backbone", 4096, NULL, 5, NULL);
}
