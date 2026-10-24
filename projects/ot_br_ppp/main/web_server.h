/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * Web interface: status, settings, Thread dataset, firmware update (OTA) and
 * a copy of the console, behind HTTP Basic authentication (user "admin").
 * Served over HTTP and, with a self-signed certificate, over HTTPS.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the HTTP and HTTPS servers on all interfaces. Does nothing
 *        and returns ESP_ERR_INVALID_STATE when no web password is
 *        configured. HTTP stays up if HTTPS fails to start.
 */
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif
