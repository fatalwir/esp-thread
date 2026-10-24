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
#include "esp_netif_net_stack.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/netif.h"
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

/* lwIP sets a PPP netif administratively up in ppp_new() and leaves it up
 * whether or not a session exists. libopenthread_br.a takes that flag as "the
 * backbone is running", so without this the border router keeps advertising
 * routes into a dead link. Follow the session instead. LCP/IPCP/IPV6CP go
 * out through ppp_write(), not the netif, so negotiation is unaffected. */
static esp_err_t set_netif_up(void *ctx)
{
    struct netif *netif = esp_netif_get_netif_impl(s_ppp_netif);

    if (ctx != NULL) {
        netif_set_up(netif);
    } else {
        netif_set_down(netif);
    }
    return ESP_OK;
}

static void on_ppp_status(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch (event_id) {
    case NETIF_PPP_PHASE_RUNNING:
        ESP_LOGI(TAG, "PPP link up");
        esp_netif_tcpip_exec(set_netif_up, (void *)1);
        xEventGroupSetBits(s_ppp_events, PPP_CONNECTED_BIT);
        break;
    case NETIF_PPP_PHASE_DISCONNECT:
        ESP_LOGW(TAG, "PPP link down");
        esp_netif_tcpip_exec(set_netif_up, NULL);
        xEventGroupClearBits(s_ppp_events, PPP_CONNECTED_BIT);
        break;
    case NETIF_PPP_PHASE_DEAD:
        /* lwIP never restarts a session on its own: LCP gives up after ~60 s
         * of unanswered Configure-Requests, and a session also ends when the
         * peer hangs up, reboots or the cable is replugged. Without this the
         * board stays silent from then on. ppp_connect() is only accepted in
         * the DEAD phase, which is where we are now. */
        xEventGroupClearBits(s_ppp_events, PPP_CONNECTED_BIT);
        ESP_LOGI(TAG, "PPP session ended; dialling again");
        esp_netif_action_start(s_ppp_netif, NULL, 0, NULL);
        break;
    case NETIF_PPP_ERRORPEERDEAD:
        ESP_LOGW(TAG, "PPP peer stopped answering LCP echoes");
        xEventGroupClearBits(s_ppp_events, PPP_CONNECTED_BIT);
        break;
    case NETIF_PPP_ERRORAUTHFAIL:
        ESP_LOGE(TAG, "router rejected user \"%s\"; check OT_BR_PPP_USER/PASSWORD", CONFIG_OT_BR_PPP_USER);
        break;
    default:
        ESP_LOGD(TAG, "PPP event %d", (int)event_id);
        break;
    }
}

static void on_ppp_got_ip(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    const ip_event_got_ip_t *event = data;
    esp_netif_dns_info_t dns = { 0 };

    esp_netif_get_dns_info(s_ppp_netif, ESP_NETIF_DNS_MAIN, &dns);
    ESP_LOGI(TAG, "router assigned " IPSTR ", gateway " IPSTR ", DNS " IPSTR,
             IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw), IP2STR(&dns.ip.u_addr.ip4));
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
    ESP_RETURN_ON_ERROR(esp_netif_ppp_set_params(s_ppp_netif, &ppp_cfg), TAG, "ppp_set_params");

    /* We are the PPP client: the upstream router authenticates us and assigns
     * our IPv4 address, gateway and DNS server over IPCP (esp-netif asks for
     * the peer's DNS by default). Offer PAP and CHAP and let the router pick. */
    if (strlen(CONFIG_OT_BR_PPP_USER) > 0) {
        ESP_RETURN_ON_ERROR(esp_netif_ppp_set_auth(s_ppp_netif,
                                                   NETIF_PPP_AUTHTYPE_PAP | NETIF_PPP_AUTHTYPE_CHAP,
                                                   CONFIG_OT_BR_PPP_USER, CONFIG_OT_BR_PPP_PASSWORD),
                            TAG, "ppp_set_auth");
    }

    esp_netif_driver_ifconfig_t driver_cfg = {
        /* No driver object of our own; the handle only has to be non-NULL. */
        .handle = (esp_netif_iodriver_handle)s_ppp_netif,
        .transmit = ppp_transmit,
    };
    ESP_RETURN_ON_ERROR(esp_netif_set_driver_config(s_ppp_netif, &driver_cfg), TAG, "driver config");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                                   on_ppp_status, NULL), TAG, "event register");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                                   on_ppp_got_ip, NULL), TAG, "event register");

    /* 3. Start. For a PPP netif esp_netif_action_start() lands in
     * esp_netif_start_ppp(), which calls ppp_connect(): we start LCP, the
     * router answers (MikroTik's serial ppp-server needs no handshake before
     * LCP). lwIP brings the netif up by itself once negotiation completes, so
     * no esp_netif_action_connected() here. */
    esp_netif_action_start(s_ppp_netif, NULL, 0, NULL);

    BaseType_t ok = xTaskCreate(ppp_rx_task, "ppp_rx", CONFIG_OT_BR_PPP_RX_TASK_STACK,
                                NULL, CONFIG_OT_BR_PPP_RX_TASK_PRIO, &s_rx_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "rx task");

    ESP_LOGI(TAG, "PPP started on USB Serial/JTAG; dialling until the router answers");
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
