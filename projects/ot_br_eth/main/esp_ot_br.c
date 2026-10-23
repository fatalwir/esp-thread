/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * OpenThread Border Router with a W5500 SPI Ethernet backbone.
 *
 * Derived from esp-idf/examples/openthread/ot_br (CC0-1.0), with three changes:
 *
 *   - single SoC: the ESP32-C6's own 802.15.4 radio is the Thread radio
 *     (CONFIG_OPENTHREAD_RADIO_NATIVE), so there is no RCP, no UART spinel
 *     link, and none of the RCP reset or Wi-Fi coexistence plumbing;
 *   - the Thread mesh is started independently of the backbone, so the mesh
 *     keeps running when the Ethernet cable is unplugged;
 *   - SNTP, so the border router has wall-clock time.
 *
 * The backbone itself is stock: example_connect() brings up the W5500 and
 * ot_examples_br hands its netif to esp_openthread_border_router_init().
 */

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
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "ot_examples_br.h"
#include "ot_examples_common.h"

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
#endif

#define TAG "esp_ot_br_eth"

/* ------------------------------------------------------------------ */
/* Wall-clock time                                                     */
/* ------------------------------------------------------------------ */

#if CONFIG_OT_BR_SNTP_ENABLE
static void on_time_synced(struct timeval *tv)
{
    (void)tv;
    time_t now = 0;
    time(&now);
    ESP_LOGI(TAG, "time synchronised: %s", ctime(&now));
}

static void start_sntp(void)
{
    /* Started unconditionally at boot. The SNTP service retries on its own
     * until Ethernet and DHCP are up, so this needs no ordering against
     * example_connect(). */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_OT_BR_SNTP_SERVER);
    config.start = true;
    config.wait_for_sync = false;
    config.server_from_dhcp = false;
    config.sync_cb = on_time_synced;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
}
#endif /* CONFIG_OT_BR_SNTP_ENABLE */

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

    /* mDNS carries SRP registrations from the mesh out onto the LAN. Unlike
     * the PPP variant, this actually reaches your other LAN hosts: Ethernet is
     * a real broadcast segment. */
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

    /* Form/join the mesh straight away. The stock example leaves this to the
     * border router path, which means no mesh at all until the backbone is up;
     * doing it here keeps the Thread side alive with the cable unplugged. */
#if CONFIG_OPENTHREAD_NETWORK_AUTO_START
    ot_network_auto_start();
#endif

    /* Brings up the W5500 via example_connect(), sets it as the backbone netif
     * and calls esp_openthread_border_router_init(). Runs in its own task, so
     * this does not block. */
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    ESP_ERROR_CHECK(esp_openthread_border_router_start());
#endif

#if CONFIG_OT_BR_SNTP_ENABLE
    start_sntp();
#endif
}
