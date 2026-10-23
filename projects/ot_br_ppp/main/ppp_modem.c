/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * PPP-over-USB-Serial/JTAG backbone link. See ppp_modem.h.
 */

#include "ppp_modem.h"

#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TAG "ppp_modem"

#define PPP_CONNECTED_BIT BIT0

static esp_netif_t     *s_ppp_netif;
static EventGroupHandle_t s_ppp_events;
static TaskHandle_t     s_rx_task;

/* ------------------------------------------------------------------ */
/* esp_netif IO driver: TX towards the wire                            */
/* ------------------------------------------------------------------ */

static esp_err_t ppp_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    const uint8_t *data = buffer;
    size_t sent = 0;

    while (sent < len) {
        int n = usb_serial_jtag_write_bytes(data + sent, len - sent, pdMS_TO_TICKS(1000));
        if (n <= 0) {
            /* Host not draining the CDC endpoint (cable out, or the peer
             * closed the port). Drop the frame; PPP will retransmit. */
            ESP_LOGD(TAG, "tx stalled after %u/%u bytes", (unsigned)sent, (unsigned)len);
            return ESP_FAIL;
        }
        sent += n;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* RX pump: wire -> lwIP                                               */
/* ------------------------------------------------------------------ */

static void ppp_rx_task(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(CONFIG_OT_BR_PPP_RX_CHUNK_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory for rx buffer");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        int len = usb_serial_jtag_read_bytes(buf, CONFIG_OT_BR_PPP_RX_CHUNK_SIZE, pdMS_TO_TICKS(100));
        if (len > 0) {
            esp_netif_receive(s_ppp_netif, buf, len, NULL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* PPP state tracking                                                  */
/* ------------------------------------------------------------------ */

static void on_ppp_status(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch (event_id) {
    case NETIF_PPP_PHASE_RUNNING:
        ESP_LOGI(TAG, "PPP link up");
        xEventGroupSetBits(s_ppp_events, PPP_CONNECTED_BIT);
        break;
    case NETIF_PPP_PHASE_DEAD:
    case NETIF_PPP_PHASE_DISCONNECT:
        ESP_LOGW(TAG, "PPP link down (phase %d)", (int)event_id);
        xEventGroupClearBits(s_ppp_events, PPP_CONNECTED_BIT);
        break;
    case NETIF_PPP_ERRORPEERDEAD:
        ESP_LOGW(TAG, "PPP peer stopped answering LCP echoes");
        xEventGroupClearBits(s_ppp_events, PPP_CONNECTED_BIT);
        break;
    default:
        ESP_LOGD(TAG, "PPP event %d", (int)event_id);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t ppp_modem_start(void)
{
    ESP_RETURN_ON_FALSE(s_ppp_netif == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    s_ppp_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_ppp_events != NULL, ESP_ERR_NO_MEM, TAG, "event group");

    /* 1. USB Serial/JTAG as a raw byte pipe.
     *
     * CONFIG_ESP_CONSOLE_SECONDARY_NONE must be set, otherwise the logging
     * subsystem mirrors console output onto this same endpoint and corrupts
     * the PPP stream. sdkconfig.defaults sets it. */
    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_cfg.rx_buffer_size = CONFIG_OT_BR_PPP_RX_BUFFER_SIZE;
    usb_cfg.tx_buffer_size = CONFIG_OT_BR_PPP_TX_BUFFER_SIZE;
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb_cfg), TAG, "usb_serial_jtag install");

    /* 2. PPP netif. */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_ppp_netif != NULL, ESP_ERR_NO_MEM, TAG, "esp_netif_new");

    esp_netif_ppp_config_t ppp_cfg = {
        .ppp_phase_event_enabled = true,
        .ppp_error_event_enabled = true,
    };
#ifdef CONFIG_LWIP_PPP_SERVER_SUPPORT
    /* Addresses we offer over IPCP. IPv6 needs none of this -- IPV6CP only
     * negotiates interface identifiers -- but NAT64 needs IPv4 on the
     * backbone, and a v4 pair gives you a second way to reach the board. */
    esp_netif_str_to_ip4(CONFIG_OT_BR_PPP_LOCAL_IP4, &ppp_cfg.ppp_our_ip4_addr);
    esp_netif_str_to_ip4(CONFIG_OT_BR_PPP_PEER_IP4, &ppp_cfg.ppp_their_ip4_addr);
#endif
    ESP_RETURN_ON_ERROR(esp_netif_ppp_set_params(s_ppp_netif, &ppp_cfg), TAG, "ppp_set_params");

    esp_netif_driver_ifconfig_t driver_cfg = {
        /* No driver object of our own; the handle only has to be non-NULL. */
        .handle = (esp_netif_iodriver_handle)s_ppp_netif,
        .transmit = ppp_transmit,
    };
    ESP_RETURN_ON_ERROR(esp_netif_set_driver_config(s_ppp_netif, &driver_cfg), TAG, "driver config");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                                   on_ppp_status, NULL), TAG, "event register");

    /* 3. Start. For a PPP netif esp_netif_action_start() lands in
     * esp_netif_start_ppp(), which calls ppp_listen() because
     * CONFIG_LWIP_PPP_SERVER_SUPPORT is enabled -- i.e. we are the server and
     * we wait for the peer to initiate. lwIP brings the netif up by itself
     * once negotiation completes, so no esp_netif_action_connected() here. */
    esp_netif_action_start(s_ppp_netif, NULL, 0, NULL);

    BaseType_t ok = xTaskCreate(ppp_rx_task, "ppp_rx", CONFIG_OT_BR_PPP_RX_TASK_STACK,
                                NULL, CONFIG_OT_BR_PPP_RX_TASK_PRIO, &s_rx_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "rx task");

    ESP_LOGI(TAG, "PPP server listening on USB Serial/JTAG; waiting for peer to dial in");
    return ESP_OK;
}

esp_err_t ppp_modem_wait_connected(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ppp_events != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_ppp_events, PPP_CONNECTED_BIT, pdFALSE, pdTRUE, ticks);

    return (bits & PPP_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_netif_t *ppp_modem_get_netif(void)
{
    return s_ppp_netif;
}
