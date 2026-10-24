/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * The HTTPS server's key and self-signed certificate. See tls_identity.h.
 */

#include "tls_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"

#define TAG       "tls_identity"
#define NAMESPACE "br_tls"

#define KEY_PEM_MAX  512
#define CERT_PEM_MAX 1024

/* Fixed validity: the clock is not set yet when the certificate is made. */
#define NOT_BEFORE "20250101000000"
#define NOT_AFTER  "20491231235959"

static char *load_str(nvs_handle_t nvs, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(nvs, key, NULL, &len) != ESP_OK) {
        return NULL;
    }
    char *value = malloc(len);
    if (value != NULL && nvs_get_str(nvs, key, value, &len) != ESP_OK) {
        free(value);
        value = NULL;
    }
    return value;
}

static esp_err_t create(const char *hostname, char *cert_pem, char *key_pem)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    mbedtls_pk_context pk;
    mbedtls_x509write_cert crt;
    esp_err_t err = ESP_FAIL;

    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);

    psa_crypto_init();
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    if (psa_generate_key(&attr, &key_id) != PSA_SUCCESS || mbedtls_pk_copy_from_psa(key_id, &pk) != 0) {
        ESP_LOGE(TAG, "key generation failed");
        goto out;
    }
    if (mbedtls_pk_write_key_pem(&pk, (unsigned char *)key_pem, KEY_PEM_MAX) != 0) {
        ESP_LOGE(TAG, "cannot encode the key");
        goto out;
    }

    char name[96];
    snprintf(name, sizeof(name), "CN=%s,O=esp-ot-br", hostname);
    char local_name[80];
    snprintf(local_name, sizeof(local_name), "%s.local", hostname);
    mbedtls_x509_san_list san_local = {
        .node = {.type = MBEDTLS_X509_SAN_DNS_NAME,
                 .san.unstructured_name = {.p = (unsigned char *)local_name, .len = strlen(local_name)}},
        .next = NULL,
    };
    mbedtls_x509_san_list san_host = {
        .node = {.type = MBEDTLS_X509_SAN_DNS_NAME,
                 .san.unstructured_name = {.p = (unsigned char *)hostname, .len = strlen(hostname)}},
        .next = &san_local,
    };
    unsigned char serial[16];
    psa_generate_random(serial, sizeof(serial));
    serial[0] &= 0x7f; /* positive */

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    if (mbedtls_x509write_crt_set_subject_name(&crt, name) != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&crt, name) != 0 ||
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0 ||
        mbedtls_x509write_crt_set_validity(&crt, NOT_BEFORE, NOT_AFTER) != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0 ||
        mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_host) != 0 ||
        mbedtls_x509write_crt_pem(&crt, (unsigned char *)cert_pem, CERT_PEM_MAX) != 0) {
        ESP_LOGE(TAG, "cannot create the certificate");
        goto out;
    }
    err = ESP_OK;

out:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    if (key_id != 0) {
        psa_destroy_key(key_id);
    }
    return err;
}

esp_err_t tls_identity_get(const char *hostname, char **cert_pem, char **key_pem)
{
    nvs_handle_t nvs;

    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    *cert_pem = load_str(nvs, "cert");
    *key_pem = load_str(nvs, "key");
    if (*cert_pem != NULL && *key_pem != NULL) {
        nvs_close(nvs);
        return ESP_OK;
    }
    free(*cert_pem);
    free(*key_pem);

    ESP_LOGI(TAG, "creating a self-signed certificate for \"%s\"", hostname);
    *cert_pem = calloc(1, CERT_PEM_MAX);
    *key_pem = calloc(1, KEY_PEM_MAX);
    esp_err_t err = (*cert_pem && *key_pem) ? create(hostname, *cert_pem, *key_pem) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "cert", *cert_pem);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "key", *key_pem);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(*cert_pem);
        free(*key_pem);
        *cert_pem = *key_pem = NULL;
    }
    return err;
}
