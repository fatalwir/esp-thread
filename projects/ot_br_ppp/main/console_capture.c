/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * A copy of the UART console for the web interface. See console_capture.h.
 */

#include "console_capture.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TAG "console_capture"

#define FORMAT_BUF_LEN 256
#define EXEC_QUEUE_LEN 4

static char         s_ring[CONFIG_OT_BR_CONSOLE_BUFFER_SIZE];
static uint32_t     s_total; /* bytes ever written; the ring holds the last ones */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_exec_queue;
static volatile bool s_diag_allowed;

/* Must not print: it runs inside every printf. */
static void append(const char *data, size_t len)
{
    if (len > sizeof(s_ring)) {
        data += len - sizeof(s_ring);
        len = sizeof(s_ring);
    }

    portENTER_CRITICAL_SAFE(&s_lock);
    size_t pos = s_total % sizeof(s_ring);
    size_t first = sizeof(s_ring) - pos;
    if (first > len) {
        first = len;
    }
    memcpy(&s_ring[pos], data, first);
    memcpy(s_ring, data + first, len - first);
    s_total += len;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

static void append_vformat(const char *format, va_list args)
{
    char buf[FORMAT_BUF_LEN];
    va_list copy;

    va_copy(copy, args);
    int len = vsnprintf(buf, sizeof(buf), format, copy);
    va_end(copy);
    if (len <= 0) {
        return;
    }
    if ((size_t)len < sizeof(buf)) {
        append(buf, len);
        return;
    }

    /* Longer than the stack buffer: format again on the heap, or keep what
     * fitted if there is no memory for that. */
    char *big = malloc(len + 1);
    if (big == NULL) {
        append(buf, sizeof(buf) - 1);
        return;
    }
    vsnprintf(big, len + 1, format, args);
    append(big, len);
    free(big);
}

/* ------------------------------------------------------------------ */
/* Link-time wrappers (-Wl,--wrap=...)                                 */
/* ------------------------------------------------------------------ */

int __real_vprintf(const char *format, va_list args);
int __real_puts(const char *s);
int __real_putchar(int c);

int __wrap_vprintf(const char *format, va_list args)
{
    va_list copy;

    va_copy(copy, args);
    int ret = __real_vprintf(format, copy);
    va_end(copy);
    append_vformat(format, args);
    return ret;
}

int __wrap_printf(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    int ret = __wrap_vprintf(format, args);
    va_end(args);
    return ret;
}

int __wrap_puts(const char *s)
{
    int ret = __real_puts(s);

    append(s, strlen(s));
    append("\n", 1);
    return ret;
}

int __wrap_putchar(int c)
{
    int ret = __real_putchar(c);
    char ch = (char)c;

    append(&ch, 1);
    return ret;
}

/* esp_console_run() is wrapped too: both the UART REPL and the web console
 * go through it, which makes it the one place to refuse `ot diag`. */
esp_err_t __real_esp_console_run(const char *cmdline, int *cmd_ret);

static const char *skip_word(const char *p, const char *word)
{
    while (isspace((unsigned char)*p)) {
        p++;
    }
    size_t len = strlen(word);
    if (strncmp(p, word, len) != 0 || (p[len] != '\0' && !isspace((unsigned char)p[len]))) {
        return NULL;
    }
    p += len;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static bool is_blocked_diag(const char *line)
{
    const char *p = skip_word(line, CONFIG_OPENTHREAD_CONSOLE_COMMAND_PREFIX);
    if (p == NULL || (p = skip_word(p, "diag")) == NULL) {
        return false;
    }
    /* Leaving diagnostics mode is always allowed. */
    return skip_word(p, "stop") == NULL;
}

esp_err_t __wrap_esp_console_run(const char *cmdline, int *cmd_ret)
{
    if (!s_diag_allowed && is_blocked_diag(cmdline)) {
        printf("Factory diagnostics are disabled; enable them in the web interface (Settings).\n");
        *cmd_ret = 0;
        return ESP_OK;
    }
    return __real_esp_console_run(cmdline, cmd_ret);
}

void console_capture_set_diag_allowed(bool allowed)
{
    s_diag_allowed = allowed;
}

/* ------------------------------------------------------------------ */
/* Reading and running commands                                        */
/* ------------------------------------------------------------------ */

size_t console_capture_read(uint32_t since, char *buf, size_t len, uint32_t *next)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    uint32_t total = s_total;
    uint32_t oldest = (total > sizeof(s_ring)) ? total - sizeof(s_ring) : 0;
    if (since < oldest || since > total) {
        since = oldest;
    }
    size_t avail = total - since;
    if (avail > len) {
        avail = len;
    }
    for (size_t i = 0; i < avail; i++) {
        buf[i] = s_ring[(since + i) % sizeof(s_ring)];
    }
    portEXIT_CRITICAL_SAFE(&s_lock);

    *next = since + avail;
    return avail;
}

static void exec_task(void *arg)
{
    (void)arg;
    char *line;

    while (true) {
        xQueueReceive(s_exec_queue, &line, portMAX_DELAY);
        printf("web> %s\n", line);
        int ret = 0;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        free(line);
    }
}

esp_err_t console_capture_start(void)
{
    s_exec_queue = xQueueCreate(EXEC_QUEUE_LEN, sizeof(char *));
    ESP_RETURN_ON_FALSE(s_exec_queue != NULL, ESP_ERR_NO_MEM, TAG, "queue");
    BaseType_t ok = xTaskCreate(exec_task, "web_console", 4096, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t console_capture_exec(const char *line)
{
    ESP_RETURN_ON_FALSE(s_exec_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    char *copy = strdup(line);
    ESP_RETURN_ON_FALSE(copy != NULL, ESP_ERR_NO_MEM, TAG, "strdup");
    if (xQueueSend(s_exec_queue, &copy, 0) != pdTRUE) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
