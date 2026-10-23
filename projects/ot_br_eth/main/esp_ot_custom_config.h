/*
 * SPDX-FileCopyrightText: 2026 Ondrej Kolar
 * SPDX-License-Identifier: MIT
 *
 * OpenThread macros that ESP-IDF's Kconfig does not expose.
 *
 * Selected via CONFIG_OPENTHREAD_HEADER_CUSTOM + CUSTOM_HEADER_PATH/FILE_NAME.
 * Values here override both the OpenThread submodule defaults and
 * openthread-core-esp32x-ftd-config.h.
 *
 * CONFIG_OPENTHREAD_BORDER_ROUTER=y already enables most of what a border
 * router needs, including netdiag and meshdiag, so only the genuinely
 * unreachable knobs are listed.
 */

#pragma once

/* `history` CLI: rolling log of RX/TX, neighbour and netdata changes. */
#ifndef OPENTHREAD_CONFIG_HISTORY_TRACKER_ENABLE
#define OPENTHREAD_CONFIG_HISTORY_TRACKER_ENABLE 1
#endif

/* `dns server` CLI plus the DNS-SD server answering queries from mesh nodes.
 * DNS_UPSTREAM_QUERY_ENABLE (already 1 for border routers) forwards anything
 * non-local to the upstream resolver. */
#ifndef OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE
#define OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE 1
#endif
