/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * PPP-over-USB-Serial/JTAG backbone link for the Thread Border Router.
 *
 * The ESP32-C6 presents its native USB Serial/JTAG port (the connector
 * labelled "USB" on the DevKitC-1) to the upstream router as a standard
 * CDC-ACM serial device, and speaks PPP over it. The resulting esp_netif
 * becomes the border router's backbone ("infrastructure") interface.
 *
 * The UART port stays free for the console, boot log and OpenThread CLI.
 */

#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the USB Serial/JTAG driver, create the PPP netif and start
 *        dialling the upstream router.
 *
 * This board is the PPP *client*: the router runs the PPP server,
 * authenticates it and assigns its addresses. The link is redialled for as
 * long as the board runs, so it recovers from either side restarting.
 *
 * @return ESP_OK on success
 */
esp_err_t ppp_modem_start(void);

/**
 * @brief Block until the PPP link reaches the RUNNING phase.
 *
 * @param timeout_ms  How long to wait, or portMAX_DELAY-equivalent when 0.
 * @return ESP_OK if the link came up, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t ppp_modem_wait_connected(uint32_t timeout_ms);

/**
 * @brief The PPP esp_netif handle, or NULL before ppp_modem_start().
 */
esp_netif_t *ppp_modem_get_netif(void);

#ifdef __cplusplus
}
#endif
