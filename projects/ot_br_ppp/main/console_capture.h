/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * A copy of the UART console for the web interface.
 *
 * printf(), vprintf(), puts() and putchar() are wrapped at link time
 * (-Wl,--wrap, see CMakeLists.txt), which covers ESP_LOG output, OpenThread
 * CLI output and esp_console command output. Everything still goes to the
 * UART; a copy lands in a ring buffer that the web console reads. Commands
 * from the web console run in a worker task through esp_console_run(), the
 * same way the UART REPL runs them.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the command worker. Output is captured from the first print
 *        on, whether or not this has been called.
 */
esp_err_t console_capture_start(void);

/**
 * @brief Copy captured output, starting at absolute offset @p since.
 *
 * @param since  Offset returned as @p next by the previous call, or 0.
 * @param buf    Destination.
 * @param len    Size of @p buf.
 * @param next   Offset to pass on the next call.
 * @return       Number of bytes copied. If @p since is older than what the
 *               ring buffer still holds, copying starts at the oldest byte.
 */
size_t console_capture_read(uint32_t since, char *buf, size_t len, uint32_t *next);

/**
 * @brief Queue a command line, as typed at the UART console.
 */
esp_err_t console_capture_exec(const char *line);

/**
 * @brief Whether both consoles accept OpenThread factory diagnostics
 *        (`ot diag ...`). `ot diag stop` is always accepted. Takes effect at
 *        once.
 */
void console_capture_set_diag_allowed(bool allowed);

#ifdef __cplusplus
}
#endif
