/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * OpenThread macros that ESP-IDF's Kconfig does not expose.
 *
 * Selected via:
 *   CONFIG_OPENTHREAD_HEADER_CUSTOM=y
 *   CONFIG_OPENTHREAD_CUSTOM_HEADER_PATH="main"
 *   CONFIG_OPENTHREAD_CUSTOM_HEADER_FILE_NAME="esp_ot_custom_config.h"
 *
 * Values here have the highest priority: they override both the OpenThread
 * submodule defaults and openthread-core-esp32x-ftd-config.h.
 *
 * Note that CONFIG_OPENTHREAD_BORDER_ROUTER=y already turns on a lot by
 * itself in openthread-core-esp32x-ftd-config.h -- BORDER_ROUTING,
 * NAT64_BORDER_ROUTING, SRP_SERVER, DNS_UPSTREAM_QUERY and BACKBONE_ROUTER --
 * and TMF_NETDIAG_CLIENT / MESH_DIAG default to BORDER_ROUTING_ENABLE, so
 * `networkdiagnostic` and `meshdiag` come along for free. Only the genuinely
 * unreachable knobs are listed below.
 */

#pragma once

/* `history` CLI: rolling log of RX/TX, neighbour and netdata changes.
 * Off by default upstream regardless of border router mode. */
#ifndef OPENTHREAD_CONFIG_HISTORY_TRACKER_ENABLE
#define OPENTHREAD_CONFIG_HISTORY_TRACKER_ENABLE 1
#endif

/* `dns server` CLI plus the DNS-SD server that answers service discovery
 * queries coming from mesh nodes. DNS_UPSTREAM_QUERY_ENABLE (already 1 for
 * border routers) forwards anything non-local to the upstream resolver. */
#ifndef OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE
#define OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE 1
#endif

/*
 * Deliberately NOT enabled here:
 *
 *   OPENTHREAD_CONFIG_TCP_ENABLE
 *     This is OpenThread's own TCP stack (the `tcp` CLI), which is a separate
 *     implementation from lwIP's. ESP-IDF forces it to 0. Ordinary sockets,
 *     esp_http_client and esp_https_ota all use lwIP's TCP, which is enabled
 *     and unaffected. Turning this on costs flash for a stack you would not
 *     otherwise use.
 */
