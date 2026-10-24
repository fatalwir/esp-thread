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

## Roles: the router is in charge

This board is the PPP **client**. The upstream router runs the PPP **server**
and decides everything about the link, so the board acts as a plain interface
between your network and the Thread mesh:

| What                             | Set by the router through                                     |
| -------------------------------- | ------------------------------------------------------------- |
| Access                           | PAP/CHAP login (`/ppp secret` on RouterOS)                     |
| IPv4 address and gateway         | IPCP (`remote-address` in the PPP profile or secret)           |
| DNS server                       | IPCP (`dns-server` in the PPP profile)                         |
| IPv6 prefix for the mesh (OMR)   | DHCPv6-PD (`dhcpv6-pd-pool` in the PPP profile)                |

The firmware holds no addresses of its own. The login goes in
`network.defaults` at the repository root (`CONFIG_OT_BR_PPP_USER`,
`CONFIG_OT_BR_PPP_PASSWORD`), next to the Thread credentials.

DNS queries from mesh nodes go to this board's DNS server, which forwards them
to the router's. The DNS64 client is off on purpose: it is meant for
Thread-only nodes and would overwrite the router's DNS server (lwIP keeps one
global list) with an address inside the NAT64 prefix.

The board starts LCP itself. lwIP gives up after ~60 s without an answer, and
a session also ends when the router hangs up or reboots; `ppp_modem.c` dials
again every time the session reaches the DEAD phase, so the board keeps trying
until the router answers and reconnects by itself after a drop. It also takes
the netif down while there is no session, so the border router reports the
backbone as not running (`ot br infraif`) and withdraws its routes instead of
advertising into a dead link.

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

### MikroTik RouterOS

Tested with RouterOS 7.24 (CHR in VirtualBox, with the board's USB port
passed through). The board shows up under `/port` as `usb1` or similar:

```
/ip pool add name=thread-ppp ranges=192.168.77.2-192.168.77.14
/ipv6 pool add name=thread-pd prefix=<your-prefix>/48 prefix-length=64
/ppp profile add name=thread local-address=192.168.77.1 remote-address=thread-ppp \
    dns-server=192.168.77.1 dhcpv6-pd-pool=thread-pd
/ppp secret add name=esp-ot-br password=<password> service=async profile=thread
/interface ppp-server add name=ppp-thread-in port=usb1 null-modem=yes \
    authentication=chap,pap profile=thread disabled=no
/system scheduler add name=thread-ppp-enable start-time=startup \
    on-event=":delay 15s; /interface ppp-server enable [find name=ppp-thread-in]"
```

- **Startup script.** RouterOS 7.24 comes back from every reboot with the
  serial `ppp-server` disabled, nothing logged; most likely the USB port does
  not exist yet when the configuration is loaded. Without the scheduler entry
  the link never returns after a router reboot.
- **Fixed address.** Put `remote-address=` on the `/ppp secret` instead of
  using a pool.
- **DNS.** `dns-server=192.168.77.1` points at the router itself, which needs
  `/ip dns set allow-remote-requests=yes`. Any other resolver works too.
- **IPv6 prefix.** For a globally routable prefix, take the pool from your
  ISP's delegation. RouterOS installs the route for the delegated /64 towards
  the board by itself.
- **IPv4 to the internet** (NAT64, SNTP) needs your usual masquerade rule to
  cover the PPP pool, and the firewall must accept DNS from the PPP interface.

Measured on the CHR: the link comes up ~10 s after the server is enabled; the
board gets its address, gateway and DNS from the profile, syncs SNTP through
the router, the router delegates a /64 that becomes the mesh's favoured OMR
prefix, the router reaches mesh addresses in it, and mesh DNS queries are
forwarded to the router.

**Open issue, seen on the CHR only:** after the board resets (its USB device
re-enumerates) or the `ppp-server` is disabled and re-enabled, RouterOS
received nothing more from the port until the router rebooted, although the
board kept dialling (checked by reading the port directly on the host). This
may well be the VirtualBox USB passthrough; it needs checking on real
hardware.

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
mesh.

Compiled in is not the same as running: the PD client is off at runtime until
`otBorderRoutingDhcp6PdSetEnabled()` is called, which `esp_ot_br.c` does right
after border router init. Tested against RouterOS's `dhcpv6-pd-pool`: the
router delegates a /64, routes it to the board, and the board publishes it as
the favoured OMR prefix. Check with `ot br pd state` and `ot br omrprefix`.

### If neither works

The border router falls back to generating its own ULA-based OMR prefix. Mesh
addressing and NAT64 still function — nodes can reach IPv4 hosts on your LAN —
you simply have no globally routable prefix from upstream.

### Note on cellular guidance found elsewhere

Most IPv6-over-PPP material online is written for the case where the ESP is a
PPP *client* dialing a carrier through a cellular modem, and talks about PDP
contexts, APNs and `IPV4V6` context types. None of that applies here: there is
no modem; the router's PPP server takes its place. The parts that do carry over
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
which is why the board takes an IPv4 address from the router over IPCP even
though Thread itself is IPv6-only. IPv4 over PPP is the most mature part of PPP.

OpenThread's own `nat64` CLI commands are not built, since Espressif uses its
own translator. Inspect it with:

```
ot br nat64prefix
ot dns resolve4 example.com
```

Not yet verified end to end: a mesh node reaching an IPv4 host through this
board's translator. A ping from the board's own CLI into its NAT64 prefix gets
no reply, which may only mean the translator does not handle traffic the
border router originates itself; it needs a second Thread node to test.

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
  unaffected. RouterOS 7 can repeat mDNS between interfaces
  (`/ip dns set mdns-repeat-ifaces=...`), which may bridge that gap; untested.
  Otherwise the W5500 Ethernet backbone is the answer if service discovery
  from LAN clients matters.
- **Relies on the prebuilt BR library's view of `struct netif`.**
  `libopenthread_br.a` ships as a binary and reads `netif->flags` and
  `netif->num` at fixed offsets (527 and 530 on this ESP-IDF). It is built
  without PPP, with 4 `client_data` slots. `CONFIG_LWIP_PPP_SUPPORT` adds a
  fifth for esp-netif, which shifts both fields by 4 bytes: the border router
  then registers interface index 1 (it reads `hwaddr_len`, 0 on PPP), believes
  the backbone is down, and `ot br state` stays `stopped`, with no error
  logged. `sdkconfig.defaults` sets
  `CONFIG_LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP=y` to drop the ACD slot and
  restore the layout (nothing on this board uses DHCP), and `esp_ot_br.c` has
  a `_Static_assert` on both offsets. If an ESP-IDF update changes the
  library, check `ot br infraif` shows the right index and `is-running:yes`
  while PPP is up. If border router init fails, `br_backbone_task` logs the
  error and leaves the mesh and CLI running so you can inspect it.

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
