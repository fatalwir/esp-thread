/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * Runtime settings of the border router, kept in NVS.
 *
 * Every field starts out as its Kconfig value (and so as whatever
 * network.defaults sets). Values saved from the web interface override
 * those and survive firmware updates. Changes take effect after a reboot.
 *
 * The Thread dataset is not here: OpenThread keeps it in its own settings.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BR_SETTINGS_STR_LEN 64

typedef struct {
    char ppp_user[BR_SETTINGS_STR_LEN];
    char ppp_password[BR_SETTINGS_STR_LEN];
    char web_password[BR_SETTINGS_STR_LEN];
    char hostname[BR_SETTINGS_STR_LEN];
    char sntp_server[BR_SETTINGS_STR_LEN];
    bool dhcp6_pd;
} br_settings_t;

/**
 * @brief Load the settings into the copy br_settings() returns. Call once,
 *        after nvs_flash_init().
 */
esp_err_t br_settings_init(void);

/**
 * @brief The settings this boot runs with.
 */
const br_settings_t *br_settings(void);

/**
 * @brief Read what is currently stored (which may differ from what this boot
 *        runs with until the next reboot).
 */
esp_err_t br_settings_load(br_settings_t *out);

/**
 * @brief Store new settings. They take effect after a reboot.
 */
esp_err_t br_settings_save(const br_settings_t *in);

/**
 * @brief Forget all stored settings, so the Kconfig values apply again.
 */
esp_err_t br_settings_erase(void);

#ifdef __cplusplus
}
#endif
