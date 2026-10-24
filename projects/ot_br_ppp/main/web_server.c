/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * Web interface. See web_server.h.
 *
 * The page itself (web/index.html) is embedded in the firmware; everything
 * else is a small JSON API under /api/ that the page calls:
 *
 *   GET  /api/status            PPP, Thread, border router, firmware
 *   GET  /api/settings          stored settings (passwords never returned)
 *   POST /api/settings          store settings, applied after a reboot
 *   GET  /api/thread            active dataset, as fields and as TLV hex
 *   POST /api/thread            set the active dataset and restart Thread
 *   POST /api/ota               raw .bin in the body; reboots into it
 *   GET  /api/console?since=N   console output after offset N
 *   POST /api/console           run one command line
 *   POST /api/reboot
 *   POST /api/factory-reset     forget settings and the Thread dataset
 */

#include "web_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "br_settings.h"
#include "cJSON.h"
#include "console_capture.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "openthread/border_routing.h"
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/thread.h"
#include "ppp_modem.h"
#include "sdkconfig.h"

#define TAG "web_server"

#define WEB_USER          "admin"
#define OT_LOCK_TIMEOUT   pdMS_TO_TICKS(2000)
#define BODY_MAX          2048
#define CONSOLE_CHUNK_MAX 4096
#define OTA_BUF_LEN       4096

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static bool authorized(httpd_req_t *req)
{
    char header[192];
    unsigned char decoded[160];
    size_t decoded_len = 0;
    char expected[sizeof(WEB_USER) + BR_SETTINGS_STR_LEN];

    snprintf(expected, sizeof(expected), WEB_USER ":%s", br_settings()->web_password);

    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        strncmp(header, "Basic ", 6) == 0 &&
        mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, (const unsigned char *)header + 6,
                              strlen(header + 6)) == 0) {
        decoded[decoded_len] = '\0';
        /* Compare in constant time once the lengths match. */
        size_t expected_len = strlen(expected);
        if (decoded_len == expected_len) {
            unsigned char diff = 0;
            for (size_t i = 0; i < expected_len; i++) {
                diff |= decoded[i] ^ (unsigned char)expected[i];
            }
            if (diff == 0) {
                return true;
            }
        }
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"esp-ot-br\", charset=\"UTF-8\"");
    httpd_resp_sendstr(req, "Unauthorized");
    return false;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (text == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message);
    httpd_resp_set_status(req, status);
    return send_json(req, json);
}

/* Read a small request body into a NUL-terminated heap buffer. */
static char *read_body(httpd_req_t *req)
{
    if (req->content_len > BODY_MAX) {
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        return NULL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(body);
            return NULL;
        }
        received += n;
    }
    body[received] = '\0';
    return body;
}

static void to_hex(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

/* Parse exactly len bytes of hex; "0x" prefix and surrounding spaces allowed. */
static bool from_hex(const char *text, uint8_t *out, size_t len)
{
    while (*text == ' ') {
        text++;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned int byte;
        if (sscanf(text + 2 * i, "%2x", &byte) != 1) {
            return false;
        }
        out[i] = (uint8_t)byte;
    }
    const char *rest = text + 2 * len;
    while (*rest == ' ') {
        rest++;
    }
    return *rest == '\0';
}

static void restart_task(void *arg)
{
    bool factory_reset = (bool)(intptr_t)arg;

    vTaskDelay(pdMS_TO_TICKS(1000)); /* let the HTTP response go out */
    if (factory_reset) {
        br_settings_erase();
        esp_openthread_lock_acquire(portMAX_DELAY);
        otInstanceFactoryReset(esp_openthread_get_instance()); /* reboots */
    }
    esp_restart();
}

static void restart_soon(bool factory_reset)
{
    xTaskCreate(restart_task, "restart", 3072, (void *)(intptr_t)factory_reset, 5, NULL);
}

/* ------------------------------------------------------------------ */
/* GET /                                                               */
/* ------------------------------------------------------------------ */

static esp_err_t index_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* EMBED_TXTFILES appends a NUL that is not part of the page. */
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

/* Browsers ask for it on every visit; answer without a 404 in the log. */
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* GET /api/status                                                     */
/* ------------------------------------------------------------------ */

static const char *ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static const char *br_state_name(otBorderRoutingState state)
{
    switch (state) {
    case OT_BORDER_ROUTING_STATE_DISABLED: return "disabled";
    case OT_BORDER_ROUTING_STATE_STOPPED:  return "stopped";
    case OT_BORDER_ROUTING_STATE_RUNNING:  return "running";
    default:                               return "uninitialized";
    }
}

static const char *pd_state_name(otBorderRoutingDhcp6PdState state)
{
    switch (state) {
    case OT_BORDER_ROUTING_DHCP6_PD_STATE_STOPPED: return "stopped";
    case OT_BORDER_ROUTING_DHCP6_PD_STATE_RUNNING: return "running";
    case OT_BORDER_ROUTING_DHCP6_PD_STATE_IDLE:    return "idle";
    default:                                       return "disabled";
    }
}

static void add_prefix(cJSON *obj, const char *name, const otIp6Prefix *prefix)
{
    char text[OT_IP6_PREFIX_STRING_SIZE];

    otIp6PrefixToString(prefix, text, sizeof(text));
    cJSON_AddStringToObject(obj, name, text);
}

static void add_firmware_status(cJSON *root)
{
    cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;

    esp_ota_get_state_partition(running, &state);
    cJSON_AddStringToObject(fw, "version", desc->version);
    cJSON_AddStringToObject(fw, "idf", desc->idf_ver);
    char built[48];
    snprintf(built, sizeof(built), "%s %s", desc->date, desc->time);
    cJSON_AddStringToObject(fw, "built", built);
    /* The build date only changes when esp_app_desc.c recompiles; the ELF
     * hash tells two builds of the same commit apart. */
    char sha[17];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    cJSON_AddStringToObject(fw, "elf_sha256", sha);
    cJSON_AddStringToObject(fw, "partition", running->label);
    cJSON_AddStringToObject(fw, "state", ota_state_name(state));
}

static void add_system_status(cJSON *root)
{
    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    time_t now = time(NULL);

    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(sys, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "min_free_heap", esp_get_minimum_free_heap_size());
    if (now > 1700000000) { /* only once SNTP has set the clock */
        char text[32];
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S UTC", gmtime(&now));
        cJSON_AddStringToObject(sys, "time", text);
    }
}

static void add_ppp_status(cJSON *root)
{
    cJSON *ppp = cJSON_AddObjectToObject(root, "ppp");
    esp_netif_t *netif = ppp_modem_get_netif();
    bool up = ppp_modem_is_connected();

    cJSON_AddBoolToObject(ppp, "up", up);
    if (netif == NULL || !up) {
        return;
    }
    esp_netif_ip_info_t ip;
    esp_netif_dns_info_t dns;
    char text[16];
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        cJSON_AddStringToObject(ppp, "ip", esp_ip4addr_ntoa(&ip.ip, text, sizeof(text)));
        cJSON_AddStringToObject(ppp, "gateway", esp_ip4addr_ntoa(&ip.gw, text, sizeof(text)));
    }
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        cJSON_AddStringToObject(ppp, "dns", esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, text, sizeof(text)));
    }
}

/* Caller holds the OpenThread lock. */
static void add_thread_status(cJSON *root, otInstance *ot)
{
    cJSON *thread = cJSON_AddObjectToObject(root, "thread");
    char text[OT_IP6_ADDRESS_STRING_SIZE];

    cJSON_AddStringToObject(thread, "role", otThreadDeviceRoleToString(otThreadGetDeviceRole(ot)));
    cJSON_AddStringToObject(thread, "network_name", otThreadGetNetworkName(ot));
    cJSON_AddNumberToObject(thread, "channel", otLinkGetChannel(ot));
    snprintf(text, sizeof(text), "0x%04x", otLinkGetPanId(ot));
    cJSON_AddStringToObject(thread, "pan_id", text);
    to_hex(otThreadGetExtendedPanId(ot)->m8, OT_EXT_PAN_ID_SIZE, text);
    cJSON_AddStringToObject(thread, "ext_pan_id", text);
    snprintf(text, sizeof(text), "0x%04x", otThreadGetRloc16(ot));
    cJSON_AddStringToObject(thread, "rloc16", text);
    snprintf(text, sizeof(text), "0x%08" PRIx32, otThreadGetPartitionId(ot));
    cJSON_AddStringToObject(thread, "partition_id", text);

    cJSON *addresses = cJSON_AddArrayToObject(thread, "addresses");
    for (const otNetifAddress *a = otIp6GetUnicastAddresses(ot); a != NULL; a = a->mNext) {
        otIp6AddressToString(&a->mAddress, text, sizeof(text));
        cJSON_AddItemToArray(addresses, cJSON_CreateString(text));
    }
}

/* Caller holds the OpenThread lock. */
static void add_br_status(cJSON *root, otInstance *ot)
{
    cJSON *br = cJSON_AddObjectToObject(root, "border_router");
    otIp6Prefix prefix;
    otRoutePreference preference;
    uint32_t if_index;
    bool if_running;

    cJSON_AddStringToObject(br, "state", br_state_name(otBorderRoutingGetState(ot)));
    if (otBorderRoutingGetInfraIfInfo(ot, &if_index, &if_running) == OT_ERROR_NONE) {
        cJSON_AddBoolToObject(br, "backbone_running", if_running);
    }
    cJSON_AddStringToObject(br, "dhcp6_pd", pd_state_name(otBorderRoutingDhcp6PdGetState(ot)));
    if (otBorderRoutingGetFavoredOmrPrefix(ot, &prefix, &preference) == OT_ERROR_NONE) {
        add_prefix(br, "omr_prefix", &prefix);
    }
    if (otBorderRoutingGetFavoredOnLinkPrefix(ot, &prefix) == OT_ERROR_NONE) {
        add_prefix(br, "on_link_prefix", &prefix);
    }
    if (otBorderRoutingGetFavoredNat64Prefix(ot, &prefix, &preference) == OT_ERROR_NONE) {
        add_prefix(br, "nat64_prefix", &prefix);
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    add_firmware_status(root);
    add_system_status(root);
    add_ppp_status(root);
    if (esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        otInstance *ot = esp_openthread_get_instance();
        add_thread_status(root, ot);
        add_br_status(root, ot);
        esp_openthread_lock_release();
    }
    return send_json(req, root);
}

/* ------------------------------------------------------------------ */
/* /api/settings                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t settings_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    br_settings_t s;
    if (br_settings_load(&s) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "cannot read settings");
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "ppp_user", s.ppp_user);
    cJSON_AddBoolToObject(json, "ppp_password_set", s.ppp_password[0] != '\0');
    cJSON_AddStringToObject(json, "hostname", s.hostname);
    cJSON_AddStringToObject(json, "sntp_server", s.sntp_server);
    cJSON_AddBoolToObject(json, "dhcp6_pd", s.dhcp6_pd);
    return send_json(req, json);
}

/* Copy a string field from the request, if present. Empty passwords mean
 * "keep the current one", so they are skipped by the caller. */
static bool take_str(cJSON *json, const char *name, char *dst, size_t len, bool skip_empty)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsString(item)) {
        return true;
    }
    if (strlen(item->valuestring) >= len) {
        return false;
    }
    if (skip_empty && item->valuestring[0] == '\0') {
        return true;
    }
    strlcpy(dst, item->valuestring, len);
    return true;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char *body = read_body(req);
    cJSON *json = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (json == NULL) {
        return send_error(req, "400 Bad Request", "expected a JSON object");
    }

    br_settings_t s;
    br_settings_load(&s);
    bool ok = take_str(json, "ppp_user", s.ppp_user, sizeof(s.ppp_user), false) &&
              take_str(json, "ppp_password", s.ppp_password, sizeof(s.ppp_password), true) &&
              take_str(json, "web_password", s.web_password, sizeof(s.web_password), true) &&
              take_str(json, "hostname", s.hostname, sizeof(s.hostname), false) &&
              take_str(json, "sntp_server", s.sntp_server, sizeof(s.sntp_server), false);
    const cJSON *pd = cJSON_GetObjectItemCaseSensitive(json, "dhcp6_pd");
    if (cJSON_IsBool(pd)) {
        s.dhcp6_pd = cJSON_IsTrue(pd);
    }
    cJSON_Delete(json);

    if (!ok) {
        return send_error(req, "400 Bad Request", "a value is too long");
    }
    if (s.hostname[0] == '\0') {
        return send_error(req, "400 Bad Request", "hostname must not be empty");
    }
    if (br_settings_save(&s) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "cannot store settings");
    }
    return settings_get(req);
}

/* ------------------------------------------------------------------ */
/* /api/thread                                                         */
/* ------------------------------------------------------------------ */

static esp_err_t thread_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    otOperationalDataset dataset;
    otOperationalDatasetTlvs tlvs;
    otError err_fields, err_tlvs;

    if (!esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        return send_error(req, "503 Service Unavailable", "OpenThread is busy");
    }
    otInstance *ot = esp_openthread_get_instance();
    err_fields = otDatasetGetActive(ot, &dataset);
    err_tlvs = otDatasetGetActiveTlvs(ot, &tlvs);
    esp_openthread_lock_release();

    cJSON *json = cJSON_CreateObject();
    if (err_fields != OT_ERROR_NONE || err_tlvs != OT_ERROR_NONE) {
        cJSON_AddBoolToObject(json, "commissioned", false);
        return send_json(req, json);
    }

    char text[2 * OT_OPERATIONAL_DATASET_MAX_LENGTH + 1];
    cJSON_AddBoolToObject(json, "commissioned", true);
    cJSON_AddStringToObject(json, "network_name", dataset.mNetworkName.m8);
    cJSON_AddNumberToObject(json, "channel", dataset.mChannel);
    snprintf(text, sizeof(text), "0x%04x", dataset.mPanId);
    cJSON_AddStringToObject(json, "pan_id", text);
    to_hex(dataset.mExtendedPanId.m8, sizeof(dataset.mExtendedPanId.m8), text);
    cJSON_AddStringToObject(json, "ext_pan_id", text);
    to_hex(dataset.mNetworkKey.m8, sizeof(dataset.mNetworkKey.m8), text);
    cJSON_AddStringToObject(json, "network_key", text);
    to_hex(dataset.mPskc.m8, sizeof(dataset.mPskc.m8), text);
    cJSON_AddStringToObject(json, "pskc", text);
    to_hex(dataset.mMeshLocalPrefix.m8, sizeof(dataset.mMeshLocalPrefix.m8), text);
    cJSON_AddStringToObject(json, "mesh_local_prefix", text);
    cJSON_AddNumberToObject(json, "active_timestamp", (double)dataset.mActiveTimestamp.mSeconds);
    to_hex(tlvs.mTlvs, tlvs.mLength, text);
    cJSON_AddStringToObject(json, "tlvs", text);
    return send_json(req, json);
}

/* Build the new dataset from the request. Either "tlvs" (a complete dataset
 * as hex, as printed by `ot dataset active -x`), or individual fields applied
 * on top of the current dataset. */
static const char *dataset_from_request(cJSON *json, otInstance *ot, otOperationalDataset *dataset)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "tlvs");
    if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
        otOperationalDatasetTlvs tlvs;
        size_t len = strlen(item->valuestring) / 2;
        if (len > sizeof(tlvs.mTlvs) || !from_hex(item->valuestring, tlvs.mTlvs, len)) {
            return "tlvs is not valid hex";
        }
        tlvs.mLength = len;
        return otDatasetParseTlvs(&tlvs, dataset) == OT_ERROR_NONE ? NULL : "tlvs is not a valid dataset";
    }

    if (otDatasetGetActive(ot, dataset) != OT_ERROR_NONE) {
        memset(dataset, 0, sizeof(*dataset));
    }

    item = cJSON_GetObjectItemCaseSensitive(json, "network_name");
    if (cJSON_IsString(item)) {
        if (strlen(item->valuestring) == 0 || strlen(item->valuestring) > OT_NETWORK_NAME_MAX_SIZE) {
            return "network_name must be 1-16 characters";
        }
        memset(&dataset->mNetworkName, 0, sizeof(dataset->mNetworkName));
        memcpy(dataset->mNetworkName.m8, item->valuestring, strlen(item->valuestring));
        dataset->mComponents.mIsNetworkNamePresent = true;
    }
    item = cJSON_GetObjectItemCaseSensitive(json, "channel");
    if (cJSON_IsNumber(item)) {
        if (item->valueint < 11 || item->valueint > 26) {
            return "channel must be 11-26";
        }
        dataset->mChannel = item->valueint;
        dataset->mComponents.mIsChannelPresent = true;
    }
    item = cJSON_GetObjectItemCaseSensitive(json, "pan_id");
    if (cJSON_IsString(item)) {
        uint8_t pan[2];
        if (!from_hex(item->valuestring, pan, sizeof(pan))) {
            return "pan_id must be 4 hex digits";
        }
        dataset->mPanId = (pan[0] << 8) | pan[1];
        dataset->mComponents.mIsPanIdPresent = true;
    }

    struct {
        const char *name;
        uint8_t    *dst;
        size_t      len;
        bool       *present;
        const char *error;
    } hex_fields[] = {
        {"ext_pan_id", dataset->mExtendedPanId.m8, sizeof(dataset->mExtendedPanId.m8),
         &dataset->mComponents.mIsExtendedPanIdPresent, "ext_pan_id must be 16 hex digits"},
        {"network_key", dataset->mNetworkKey.m8, sizeof(dataset->mNetworkKey.m8),
         &dataset->mComponents.mIsNetworkKeyPresent, "network_key must be 32 hex digits"},
        {"pskc", dataset->mPskc.m8, sizeof(dataset->mPskc.m8),
         &dataset->mComponents.mIsPskcPresent, "pskc must be 32 hex digits"},
        {"mesh_local_prefix", dataset->mMeshLocalPrefix.m8, sizeof(dataset->mMeshLocalPrefix.m8),
         &dataset->mComponents.mIsMeshLocalPrefixPresent, "mesh_local_prefix must be 16 hex digits"},
    };
    for (size_t i = 0; i < sizeof(hex_fields) / sizeof(hex_fields[0]); i++) {
        item = cJSON_GetObjectItemCaseSensitive(json, hex_fields[i].name);
        if (cJSON_IsString(item)) {
            if (!from_hex(item->valuestring, hex_fields[i].dst, hex_fields[i].len)) {
                return hex_fields[i].error;
            }
            *hex_fields[i].present = true;
        }
    }

    /* Keep the current timestamp. A newer one would make this node's dataset
     * win over the network's when it rejoins, changing every other node too;
     * that is what `dataset mgmtsetcommand` on the console is for. */
    if (!dataset->mComponents.mIsActiveTimestampPresent) {
        dataset->mActiveTimestamp.mSeconds = 1;
        dataset->mComponents.mIsActiveTimestampPresent = true;
    }
    return NULL;
}

static esp_err_t thread_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char *body = read_body(req);
    cJSON *json = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (json == NULL) {
        return send_error(req, "400 Bad Request", "expected a JSON object");
    }
    if (!esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        cJSON_Delete(json);
        return send_error(req, "503 Service Unavailable", "OpenThread is busy");
    }

    otInstance *ot = esp_openthread_get_instance();
    otOperationalDataset dataset;
    const char *error = dataset_from_request(json, ot, &dataset);
    cJSON_Delete(json);
    if (error == NULL) {
        /* Leave the network, switch datasets, and join with the new one. */
        otThreadSetEnabled(ot, false);
        if (otDatasetSetActive(ot, &dataset) != OT_ERROR_NONE) {
            error = "OpenThread rejected the dataset";
        }
        otIp6SetEnabled(ot, true);
        otThreadSetEnabled(ot, true);
    }
    esp_openthread_lock_release();

    if (error != NULL) {
        return send_error(req, "400 Bad Request", error);
    }
    ESP_LOGI(TAG, "Thread dataset changed from the web interface");
    return thread_get(req);
}

/* ------------------------------------------------------------------ */
/* POST /api/ota                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return send_error(req, "500 Internal Server Error", "no OTA partition");
    }
    if (req->content_len == 0 || req->content_len > target->size) {
        return send_error(req, "400 Bad Request", "image is empty or larger than the OTA partition");
    }

    char *buf = malloc(OTA_BUF_LEN);
    if (buf == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        free(buf);
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OTA: writing %u bytes to %s", (unsigned)req->content_len, target->label);
    size_t received = 0;
    while (received < req->content_len && err == ESP_OK) {
        int n = httpd_req_recv(req, buf, OTA_BUF_LEN);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, n);
        received += n;
    }
    free(buf);

    if (err != ESP_OK) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "OTA: transfer failed after %u bytes: %s", (unsigned)received, esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            return send_error(req, "400 Bad Request", "not a valid firmware image for this chip");
        }
        return send_error(req, "500 Internal Server Error", "transfer failed");
    }
    err = esp_ota_end(ota); /* checks the image */
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(target);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: image rejected: %s", esp_err_to_name(err));
        return send_error(req, "400 Bad Request", err == ESP_ERR_OTA_VALIDATE_FAILED ?
                          "not a valid firmware image for this chip" : esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OTA: done, rebooting into %s", target->label);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "result", "rebooting");
    cJSON_AddStringToObject(json, "partition", target->label);
    esp_err_t ret = send_json(req, json);
    restart_soon(false);
    return ret;
}

/* ------------------------------------------------------------------ */
/* /api/console                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t console_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char query[32];
    char value[16];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK) {
        since = strtoul(value, NULL, 10);
    }

    char *chunk = malloc(CONSOLE_CHUNK_MAX + 1);
    if (chunk == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    uint32_t next;
    size_t len = console_capture_read(since, chunk, CONSOLE_CHUNK_MAX, &next);
    chunk[len] = '\0';

    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "next", next);
    cJSON_AddStringToObject(json, "data", chunk);
    free(chunk);
    return send_json(req, json);
}

static esp_err_t console_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char *line = read_body(req);
    if (line == NULL) {
        return send_error(req, "400 Bad Request", "expected a command line");
    }
    line[strcspn(line, "\r\n")] = '\0';
    esp_err_t err = (line[0] != '\0') ? console_capture_exec(line) : ESP_OK;
    free(line);
    if (err != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "console is busy");
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* POST /api/reboot, /api/factory-reset                                */
/* ------------------------------------------------------------------ */

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "reboot requested from the web interface");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    restart_soon(false);
    return ESP_OK;
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "factory reset requested from the web interface");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    restart_soon(true);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

esp_err_t web_server_start(void)
{
    if (br_settings()->web_password[0] == '\0') {
        ESP_LOGW(TAG, "no web password set (CONFIG_OT_BR_WEB_PASSWORD); web interface is off");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_OT_BR_WEB_PORT;
    config.stack_size = 8192;
    config.max_uri_handlers = 13;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;

    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");

    const httpd_uri_t uris[] = {
        {.uri = "/",                  .method = HTTP_GET,  .handler = index_get},
        {.uri = "/favicon.ico",       .method = HTTP_GET,  .handler = favicon_get},
        {.uri = "/api/status",        .method = HTTP_GET,  .handler = status_get},
        {.uri = "/api/settings",      .method = HTTP_GET,  .handler = settings_get},
        {.uri = "/api/settings",      .method = HTTP_POST, .handler = settings_post},
        {.uri = "/api/thread",        .method = HTTP_GET,  .handler = thread_get},
        {.uri = "/api/thread",        .method = HTTP_POST, .handler = thread_post},
        {.uri = "/api/ota",           .method = HTTP_POST, .handler = ota_post},
        {.uri = "/api/console",       .method = HTTP_GET,  .handler = console_get},
        {.uri = "/api/console",       .method = HTTP_POST, .handler = console_post},
        {.uri = "/api/reboot",        .method = HTTP_POST, .handler = reboot_post},
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register %s", uris[i].uri);
    }

    ESP_LOGI(TAG, "web interface on port %d (user \"" WEB_USER "\")", CONFIG_OT_BR_WEB_PORT);
    return ESP_OK;
}
