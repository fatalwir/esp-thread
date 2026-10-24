/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * The HTTPS server's key and self-signed certificate.
 *
 * Created on the board on first use (ECDSA P-256) and kept in NVS, so the
 * certificate a browser was told to trust stays the same across reboots,
 * firmware updates and factory resets. Nothing secret is in the repository
 * or the firmware image.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load the certificate and key, creating them if there are none yet.
 *
 * @param hostname  Name put in the certificate (CN, and as DNS names
 *                  "<hostname>" and "<hostname>.local"); only used when
 *                  creating.
 * @param cert_pem  Receives a heap-allocated, NUL-terminated PEM certificate.
 * @param key_pem   Receives a heap-allocated, NUL-terminated PEM private key.
 */
esp_err_t tls_identity_get(const char *hostname, char **cert_pem, char **key_pem);

#ifdef __cplusplus
}
#endif
