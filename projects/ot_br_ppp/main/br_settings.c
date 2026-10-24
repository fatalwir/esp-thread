/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * Runtime settings of the border router, kept in NVS. See br_settings.h.
 */

#include "br_settings.h"

#include <string.h>

#include "esp_check.h"
#include "nvs.h"
#include "sdkconfig.h"

#define TAG       "br_settings"
#define NAMESPACE "br_settings"

#ifndef CONFIG_OT_BR_SNTP_SERVER
#define CONFIG_OT_BR_SNTP_SERVER ""
#endif
#ifndef CONFIG_OT_BR_ALLOW_THREAD_DIAG
#define CONFIG_OT_BR_ALLOW_THREAD_DIAG 0
#endif

static br_settings_t s_settings;

static void set_defaults(br_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->ppp_user, CONFIG_OT_BR_PPP_USER, sizeof(s->ppp_user));
    strlcpy(s->ppp_password, CONFIG_OT_BR_PPP_PASSWORD, sizeof(s->ppp_password));
    strlcpy(s->web_password, CONFIG_OT_BR_WEB_PASSWORD, sizeof(s->web_password));
    strlcpy(s->hostname, CONFIG_OT_BR_MDNS_HOSTNAME, sizeof(s->hostname));
    strlcpy(s->sntp_server, CONFIG_OT_BR_SNTP_SERVER, sizeof(s->sntp_server));
    s->dhcp6_pd = true;
    s->thread_diag = CONFIG_OT_BR_ALLOW_THREAD_DIAG;
}

/* Overwrite dst with the stored string, if there is one. */
static void load_str(nvs_handle_t nvs, const char *key, char *dst, size_t len)
{
    size_t stored_len = len;
    char buf[BR_SETTINGS_STR_LEN];

    if (nvs_get_str(nvs, key, buf, &stored_len) == ESP_OK) {
        strlcpy(dst, buf, len);
    }
}

esp_err_t br_settings_load(br_settings_t *out)
{
    nvs_handle_t nvs;

    set_defaults(out);
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* nothing saved yet */
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");

    load_str(nvs, "ppp_user", out->ppp_user, sizeof(out->ppp_user));
    load_str(nvs, "ppp_pass", out->ppp_password, sizeof(out->ppp_password));
    load_str(nvs, "web_pass", out->web_password, sizeof(out->web_password));
    load_str(nvs, "hostname", out->hostname, sizeof(out->hostname));
    load_str(nvs, "sntp", out->sntp_server, sizeof(out->sntp_server));
    uint8_t flag;
    if (nvs_get_u8(nvs, "dhcp6_pd", &flag) == ESP_OK) {
        out->dhcp6_pd = flag != 0;
    }
    if (nvs_get_u8(nvs, "thread_diag", &flag) == ESP_OK) {
        out->thread_diag = flag != 0;
    }

    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t br_settings_save(const br_settings_t *in)
{
    nvs_handle_t nvs;

    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(nvs, "ppp_user", in->ppp_user);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "ppp_pass", in->ppp_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "web_pass", in->web_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "hostname", in->hostname);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "sntp", in->sntp_server);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "dhcp6_pd", in->dhcp6_pd);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "thread_diag", in->thread_diag);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t br_settings_erase(void)
{
    nvs_handle_t nvs;

    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t br_settings_init(void)
{
    return br_settings_load(&s_settings);
}

const br_settings_t *br_settings(void)
{
    return &s_settings;
}
