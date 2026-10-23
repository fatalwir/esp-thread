# ot_br_ppp — Thread Border Router with a PPP-over-USB backbone

A single ESP32-C6-DevKitC-1 acting as a full OpenThread Border Router, using
its own 802.15.4 radio for Thread and a PPP link over its native USB port as
the backbone ("infrastructure") interface towards an upstream router or host.

Derived from `esp-idf/examples/openthread/ot_br`.

## Port assignment

| Connector      | Peripheral                       | Used for                                                    |
| -------------- | -------------------------------- | ----------------------------------------------------------- |
| **UART** | USB↔UART bridge → UART0        | ROM/bootloader output,`ESP_LOG`, the `ot` CLI, flashing |
| **USB**  | native USB Serial/JTAG (CDC-ACM) | **PPP only** — nothing else may write here           |

`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` is load-bearing. ESP-IDF defaults to
`ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`, which mirrors log output onto the USB
endpoint even when UART is the primary console, and would inject text into the
PPP frame stream.

## Why this shape

The C6 has a single RF path, so a Wi-Fi backbone forces Wi-Fi and 802.15.4 to
share it and hurts both. A wired backbone avoids that entirely, which is why
`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=n` here — the radio is dedicated to Thread.

PPP was chosen over Ethernet (W5500) because it needs no extra hardware. The
tradeoffs are real and documented under "Limitations" below.

## Roles: who dials whom

`CONFIG_LWIP_PPP_SERVER_SUPPORT=y` is what picks the role, and it is a
**compile-time** choice. In `esp_netif_lwip_ppp.c`:

```c
#ifdef CONFIG_LWIP_PPP_SERVER_SUPPORT
    esp_err_t err = ppp_listen(ppp_ctx->ppp);
#else
    err_t err = ppp_connect(ppp_ctx->ppp, 0);
#endif
```

So **this board is the PPP server** and the upstream peer must be the PPP
**client**.

## Build and flash

From the workspace root, after activating the environment (see the top-level
README):

```sh
cd projects/ot_br_ppp
idf.py set-target esp32c6      # only once
idf.py -p <PORT> build flash monitor
```

Flash over the **UART** port.

The Thread dataset matches `projects/ot_cli`, so this board joins the same mesh.
As an FTD it can be promoted to Router later; it only becomes Leader if it is
the first node up.

## Connecting the upstream peer

Plug the board's **USB** connector into the peer, not the UART one. The two
enumerate as different devices:

| Connector | Enumerates as                            | Carries                 |
| --------- | ---------------------------------------- | ----------------------- |
| **USB**   | USB CDC-ACM serial, VID:PID `303a:1001`  | PPP                     |
| **UART**  | the board's USB-to-UART bridge (CP2102N) | console, logs, `ot` CLI |

A PPP client started on the UART bridge is talking to the console, not to PPP.
On Linux the USB connector appears as `/dev/ttyACM*`; the peer needs a USB
CDC-ACM driver for it.

## Getting a global IPv6 prefix from upstream — SLAAC vs DHCPv6-PD

**IPV6CP only negotiates interface identifiers.** When the PPP link comes up,
both ends have link-local (`fe80::/10`) addresses and nothing more. Unlike
IPCP for IPv4, PPP's IPv6 control protocol never assigns a global address or a
prefix. Something has to run *on top of* the established link to provide that.

There are two ways, and the choice matters more here than it would on Ethernet.

### Option A — Router Advertisements (SLAAC)

The upstream router sends RAs on its PPP interface; the border router learns
an on-link prefix and a default route from them.

This works on Ethernet but is the riskier option over PPP, because it drags in
OpenThread's router-reachability machinery. Every router the BR learns from an
RA gets tracked in `RxRaTracker` and periodically probed with a **unicast
Neighbor Solicitation**, expecting a Neighbor Advertisement back
(`rx_ra_tracker.cpp`, `SendNeighborSolicitToRouter`). The escalation is 5
probes 1 second apart, then a 2 second grace period; if none are answered the
router is declared unreachable and **everything it advertised is torn down** —
on-link prefixes deprecated, route prefixes' valid lifetimes cleared.

Many stacks do not run ND on point-to-point links at all, since there is no
link-layer address to resolve. If the peer does not answer NS on the PPP
interface, you get a slow flap: prefix learned, five probes, prefix discarded,
repeat. The symptom in the log is:

```
No response to all Neighbor Solicitations attempts from router ... - marking it unreachable
```

### Option B — DHCPv6 Prefix Delegation (recommended first try)

OpenThread has a built-in DHCPv6-PD client, and **it is already compiled into
this firmware**. `openthread-core-esp32x-ftd-config.h:492,502` sets both:

```c
#define OPENTHREAD_CONFIG_BORDER_ROUTING_DHCP6_PD_ENABLE        1
#define OPENTHREAD_CONFIG_BORDER_ROUTING_DHCP6_PD_CLIENT_ENABLE 1
```

Upstream defaults for both are `0` — Espressif enables them under
`CONFIG_OPENTHREAD_BORDER_ROUTER`. The platform side lives in the prebuilt
`libopenthread_br.a` (`otPlatInfraIfDhcp6PdClientSend`,
`otPlatInfraIfDhcp6PdClientSetListeningEnabled`, backed by lwIP UDP).

|                                | RA / SLAAC                       | DHCPv6-PD                           |
| ------------------------------ | -------------------------------- | ----------------------------------- |
| Transport                      | ICMPv6 multicast ND              | unicast UDP                         |
| Needs the peer to answer NS/NA | yes — see the flap above        | no                                  |

DHCPv6-PD sidesteps the reachability-probe problem entirely, and a delegated
prefix is exactly what the BR wants in order to publish an OMR prefix to the
mesh. It also needs only a configuration change on the peer, not on the
firmware.

**Not yet tested:** DHCPv6-PD over this PPP link. Espressif's PD platform code
is inside the closed `libopenthread_br.a`.

### If neither works

The border router falls back to generating its own ULA-based OMR prefix. Mesh
addressing and NAT64 still function — nodes can reach IPv4 hosts on your LAN —
you simply have no globally routable prefix from upstream.

### Note on cellular guidance found elsewhere

Most IPv6-over-PPP material online is written for the case where the ESP is a
PPP *client* dialing a carrier through a cellular modem, and talks about PDP
contexts, APNs and `IPV4V6` context types. None of that applies here: there is
no modem, and this board is the PPP **server**. The parts that do carry over
are the LCP → IPv6CP sequence and the fact that IPV6CP yields link-local only.

## NAT64 — what it does and why you want it

Thread is IPv6-only. There is no IPv4 anywhere on the mesh, yet most of what a
node wants to talk to is IPv4: an HTTP server at `192.168.x.y`, `8.8.8.8`, a
package mirror. NAT64 is the bridge.

**How it works.** The border router claims an IPv6 prefix (`64:ff9b::/96` by
convention) and advertises it in the Thread Network Data. A node that wants
`192.168.1.50` sends to `64:ff9b::192.168.1.50` — the IPv4 address literally
embedded in the last 32 bits. The BR receives that packet, rewrites it as
IPv4, tracks the session, forwards it out the backbone, and translates the
reply back.

**DNS64 is the companion half.** A node resolving `example.com` gets only an A
record if the host is IPv4-only, which it cannot use. DNS64 synthesises an AAAA
record by stuffing that IPv4 address into the NAT64 prefix, so name resolution
transparently produces something routable. That is exactly what
`CONFIG_OPENTHREAD_DNS64_CLIENT` and `CONFIG_OPENTHREAD_DNS_SERVER_ADDR="8.8.8.8"`
are for — that `8.8.8.8` is synthesised into the NAT64 prefix at runtime
(`esp_openthread_state.c`), which is why it does nothing until a BR exists.

**It is already enabled here.** `CONFIG_OPENTHREAD_BORDER_ROUTER=y` turns on
`OPENTHREAD_CONFIG_NAT64_BORDER_ROUTING_ENABLE` in
`openthread-core-esp32x-ftd-config.h`. Espressif implements the translator
itself (stateful, with TCP/UDP/ICMP session tables) inside `libopenthread_br.a`.

**Relevance to the PPP design:** NAT64 needs IPv4 connectivity on the backbone,
which is why `ppp_modem.c` negotiates an IPv4 pair over IPCP even though Thread
itself is IPv6-only. IPv4 over PPP is the most mature part of PPP.

Inspect it from the CLI:

```
ot nat64 state
ot nat64 cidr            # the IPv4 pool used for translation
ot nat64 mappings        # active sessions
ot br nat64prefix
ot dns resolve4 example.com
```

This is also the path a node needs to run `esp_https_ota` against an IPv4 HTTP
server on your LAN.

## Time synchronisation

Two independent mechanisms; they solve different problems.

**1. SNTP on the border router** (enabled, `CONFIG_OT_BR_SNTP_ENABLE=y`).
Starts once PPP is up and gives *this board* real wall-clock time. Configure
the server under `Thread Border Router over PPP → Services`. If DNS is not
working yet, put a literal IP there.

**2. Time on the mesh nodes** — pick one:

- **SNTP on each node.** Ordinary UDP through the border router; needs NAT64 or
  an IPv6 NTP server. No Thread-level changes, does not disturb the mesh. This
  is the recommended route.
- **`CONFIG_OPENTHREAD_TIME_SYNC`** — Thread-native time distribution
  (`ot networktime`), accurate to well under a millisecond and works with no
  upstream at all. Deliberately left **off**: it changes what goes on the air,
  so **every node must be rebuilt with it** or the mesh misbehaves. If you want
  it, enable it here *and* in `projects/ot_cli` together.

## What is enabled that ot_cli did not have

Free with `CONFIG_OPENTHREAD_BORDER_ROUTER=y`: border routing, NAT64, SRP
server, DNS upstream query, backbone router — and, because they default to
`OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE`, also `TMF_NETDIAG_CLIENT` and
`MESH_DIAG`, i.e. the `networkdiagnostic` and `meshdiag` commands.

Turned on explicitly: commissioner, joiner, link metrics, MAC filter, border
agent, DNS client, radio stats. Via `main/esp_ot_custom_config.h` (macros with
no Kconfig switch): history tracker, DNS-SD server.

Deliberately off: OpenThread's own TCP stack. That is a separate implementation
from lwIP's; sockets, `esp_http_client` and `esp_https_ota` all use lwIP's TCP,
which is enabled. See the comment in `esp_ot_custom_config.h`.

## Web UI — not included

The border router web GUI lives in the
[esp-thread-br](https://github.com/espressif/esp-thread-br) repository as an
in-tree component. It is **not published to the ESP component registry**, so it
cannot be pulled in via `idf_component.yml` the way `mdns` is. Adding it means
cloning that repo and referencing the component by path.

It should be compatible with this design — it serves HTTP over TCP unicast,
which routes fine over PPP — but it was not wired up here because it is a
separate integration with its own dependencies. The 3 MB app partition leaves
room for it.

Until then, `meshdiag topology` and `networkdiagnostic get` from the CLI cover
most of what the web UI shows.

## Limitations

- **SRP → mDNS does not reach your LAN.** The BR advertises mesh services via
  multicast mDNS on the backbone. Over a point-to-point PPP link that reaches
  the peer and stops — there is no shared segment behind it, and routers do not
  normally relay it. Routing, RAs, prefix advertisement and NAT64 are
  unaffected. If service discovery from LAN clients matters to you, that is the
  argument for the W5500 Ethernet backbone instead.
- **Relies on the prebuilt BR library accepting a PPP netif.**
  `libopenthread_br.a` ships as a binary. Its symbol table shows only generic
  lwIP netif APIs (`raw_bind_netif`, `mld6_joingroup_netif`,
  `netif_get_by_index`) and no Ethernet-specific calls, but whether
  `esp_openthread_infra_if_init()` makes other assumptions about the netif
  cannot be seen from outside. If border router init fails, `br_backbone_task`
  logs the error and leaves the mesh and CLI running so you can inspect it.

## ESP-IDF fix for PPP backbones

Stock ESP-IDF's `otPlatGetInfraIfLinkLayerAddress()` ignores the return value
of `esp_netif_get_mac()`. A PPP netif has no MAC, so the Router Advertisements,
Router Solicitations and Neighbor Solicitations this board sends on the
backbone would carry a Source Link-Layer Address Option built from
uninitialised memory.

The `esp-idf` submodule of this repository points at a branch with the fix
([fatalwir/esp-idf@5710f968](https://github.com/fatalwir/esp-idf/commit/5710f968daeef62228c5027cfca86c03700b59ad)):
when the backbone has no MAC, the function returns `OT_ERROR_FAILED` and
OpenThread omits the option, as RFC 4861 intends for links without link-layer
addresses. Stock ESP-IDF builds this project too, but its ND messages then
carry the junk option.
