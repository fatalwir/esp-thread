# ot_br_eth — Thread Border Router with a W5500 Ethernet backbone

A single ESP32-C6-DevKitC-1 acting as a full OpenThread Border Router: its own
802.15.4 radio for Thread, and a WIZnet W5500 over SPI as the backbone
("infrastructure") interface into your LAN.

Derived from `esp-idf/examples/openthread/ot_br`. For how it compares with the
PPP variant, see "Ethernet or PPP?" at the bottom.

## Wiring

Five signals plus power, all on the **J1** side of the DevKitC-1. Match by the
silkscreened GPIO number, not by header position.

| W5500 pin | Also labelled | ESP32-C6 | Direction |
|---|---|---|---|
| **SCLK** | SCK, CLK | **GPIO6** | C6 → W5500 |
| **MOSI** | SI, DI, SDI | **GPIO7** | C6 → W5500 |
| **MISO** | SO, DO, SDO | **GPIO1** | W5500 → C6 |
| **SCS** | CS, SS, NSS, nCS | **GPIO10** | C6 → W5500 |
| **INT** | INTn, IRQ | **GPIO11** | W5500 → C6 |
| **RST** | RSTn | *(not connected)* | pull up to 3V3 — see below |
| **3V3** | VCC | **3V3** | see power note |
| **GND** | | **GND** | |

```
      ESP32-C6-DevKitC-1                 W5500 Lite
     ┌────────────────────┐            ┌──────────────┐
     │            GPIO6   ├────────────┤ SCLK         │
     │            GPIO7   ├────────────┤ MOSI         │
     │            GPIO1   ├────────────┤ MISO         │
     │            GPIO10  ├────────────┤ SCS          │
     │            GPIO11  ├────────────┤ INT          │
     │            3V3     ├──────┬─────┤ 3V3 ──┐      │
     │            GND     ├──────┼─────┤ GND   ┴10k   │
     │            GPIO8 ✗ │      │     │ RST ──┘      │
     │        (RGB LED)   │      │     │          RJ45├── to LAN
     └────────────────────┘      │     └──────────────┘
                                 └── RST pulled up to 3V3, never to the ESP

     GPIO0 left spare — the only slack on this side.
```

### Why RST is not wired to the ESP

**GPIO8 is the DevKitC-1's RGB LED**, which this build uses for the Thread role
indicator (see below). Driving WS2812 data into a reset line would hold the
W5500 in reset, so GPIO8 must stay dedicated to the LED.

Nothing is lost: the SPI-Ethernet path in `esp_eth` has no reset GPIO, so
firmware never drives RST regardless. Tie the W5500's `RST` to 3V3 through a
**10 kΩ pull-up** and leave the J1 pin-9 position unconnected. The Lite module
already carries that pull-up; a bare-chip design on your own PCB needs one.

This also keeps every electrically active signal off the strapping pins, which
is otherwise impossible — see the pin analysis below.

Do not connect `5V` if the module exposes one alongside `3V3`; pick one.

**Logic levels.** This build is wired against a **W5500 Lite** module — a bare
3.3 V board, so every signal above connects straight to the C6 with no level
shifting.

Two things to confirm on your particular board, since "Lite" covers a few
variants: that the supply pin is **3V3 and not 5V** (these modules are 3.3 V
only — 5 V on VCC will destroy them, unlike the shield-style boards which carry
their own regulator), and that you match the **silkscreened signal names**
rather than counting header positions, because the 2×6 header arrangement
differs between vendors.

Arduino-shield-style W5500 boards are a different animal: built for 5 V hosts,
with signal pins that may be driven at 5 V, which would damage the C6. The
table above assumes the bare module.

**Power.** A W5500 draws roughly 130–180 mA with the link up. The DevKit's 3V3
regulator should cope, but brownouts or a flapping link point straight at this —
feed the module from a separate 3V3 supply and tie the grounds together.

**Pin choice is not arbitrary.** The stock `protocol_examples_common` defaults
(SCLK 14 / MOSI 13 / MISO 12 / CS 15 / INT 4) are written for other targets and
are actively wrong on a C6: **GPIO12 and GPIO13 are the chip's USB D−/D+**, and
GPIO4 and GPIO15 are strapping pins. Every pin above avoids the strapping pins
(4, 5, 8, 9, 15), the USB pair (12, 13), UART0 (16, 17) and the SPI flash range
(24–30). They are all adjustable in `menuconfig` under *Example Connection
Configuration*.

### How these five were chosen

J1 breaks out GPIO 0, 1, 4–8 and 10–13. Three groups are unavailable:

| Pins | Why not |
|---|---|
| 12, 13 | USB D−/D+ — using them costs the USB port and JTAG |
| 4, 5 | strapping pins; levels at reset are not yours to control |
| 8 | RGB LED, claimed by the OpenThread role indicator |

That leaves **0, 1, 6, 7, 10, 11** — six pins for five signals, so GPIO0 is the
single spare.

**SCLK on GPIO6 and MOSI on GPIO7** because those are SPI2's native IO_MUX pins
(`esp-idf/components/soc/esp32c6/include/soc/spi_pins.h`):

```c
#define SPI2_IOMUX_PIN_NUM_MISO 2
#define SPI2_IOMUX_PIN_NUM_CLK  6
#define SPI2_IOMUX_PIN_NUM_MOSI 7
#define SPI2_IOMUX_PIN_NUM_CS   16
```

Be clear about what that does and does not buy you: **nothing, today.** The SPI
driver only takes the fast IO_MUX path when *every* pin matches, and it cannot
here — CS's native pin is GPIO16 (the console UART) and MISO's is GPIO2 (not on
this side of the board). So the bus runs through the GPIO matrix either way,
which is what caps the clock at roughly 26 MHz. Using the native pins for the two
signals that can have them is convention and future-proofing, not performance.

If you ever move the console to USB Serial/JTAG, GPIO16 frees up, and CS=GPIO16
with MISO=GPIO2 would complete the IO_MUX set and unlock the 80 MHz path. That is
the arrangement to aim for on a custom PCB.

SPI clock is set to **20 MHz**, deliberately conservative for jumper wires. The
W5500 will run much faster on a proper PCB — raise
`CONFIG_EXAMPLE_ETH_SPI_CLOCK_MHZ` if you want to, lower it if you see
instability. Keep the leads short; SPI over long dupont wires is the usual
culprit for intermittent faults.

### Checking the wiring before anything else

The driver probes the W5500's `VERSIONR` register (0x0039) at init and expects
`0x04`. That gives an unambiguous verdict in the boot log:

- **No version error, link comes up** → wiring is good.
- `W5500 version mismatched, expected 0x04, got 0x00` → MISO is reading as all
  zeros. Usually MISO not connected, or MOSI/MISO swapped.
- `W5500 version mismatched, expected 0x04, got 0xff` → MISO floating high.
  Usually CS not connected, wrong CS pin, or the module unpowered.
- `spi transmit failed` → SPI bus setup problem, not the module — check the host
  number and that no other peripheral claims those pins.

Get a clean version read before spending time on Thread or network config.

## Build and flash

From the workspace root, after activating the environment (see the top-level
README):

```sh
cd projects/ot_br_eth
idf.py set-target esp32c6      # only once
idf.py -p <PORT> build flash monitor
```

Console, `ot` CLI and flashing all go over the **UART** connector. The USB
connector is unused by this build and stays free for JTAG debugging.

The Thread dataset matches `projects/ot_cli` and `projects/ot_br_ppp`, so this
board joins the same mesh with no provisioning.

## Boot order

The mesh comes up independently of the backbone. `app_main` calls
`ot_network_auto_start()` before `esp_openthread_border_router_start()`, so
Thread forms even with the Ethernet cable unplugged — you get a working `ot`
CLI to inspect either way. The stock example ties the two together and gives you
no mesh at all until the backbone connects.

`esp_openthread_border_router_start()` runs in its own task, so nothing blocks.
It brings up the W5500 through `example_connect()`, sets that netif as the
backbone, and calls `esp_openthread_border_router_init()`.

## A Kconfig trap worth knowing about

`CONFIG_EXAMPLE_CONNECT_THREAD` **must be `n`**, and it is *not* off by default
— it defaults to `y` when OpenThread is enabled. The problem is this line in
`protocol_examples_common/Kconfig.projbuild`:

```
config EXAMPLE_CONNECT_IPV4
    bool
    depends on LWIP_IPV4
    default n if EXAMPLE_CONNECT_THREAD
    default y
```

Leave `EXAMPLE_CONNECT_THREAD` on and `EXAMPLE_CONNECT_IPV4` silently goes to
`n`, `example_connect()` never obtains a DHCPv4 lease, and **NAT64 ends up with
no IPv4 path** — which defeats most of the point of having a border router.
`EXAMPLE_CONNECT_IPV4` is a hidden symbol with no prompt, so it cannot be forced
from `sdkconfig.defaults`; the only lever is `EXAMPLE_CONNECT_THREAD=n`.

Verify after any config change:

```sh
grep -E "EXAMPLE_CONNECT_(IPV4|THREAD)" sdkconfig
```

You want `CONFIG_EXAMPLE_CONNECT_IPV4=y`.

## What you get

Once the link is up and DHCP has run:

- **NAT64 + DNS64** — mesh nodes reach IPv4 hosts on your LAN. This is what
  makes `esp_https_ota` from a plain HTTP server on your network work.
- **SRP → mDNS** — services registered by mesh nodes are advertised onto the
  LAN and are visible to ordinary hosts (`avahi-browse`, Bonjour). This is the
  part the PPP variant fundamentally cannot do.
- **Global prefix from upstream** via ordinary Router Advertisements from your
  LAN router. No DHCPv6-PD needed, though the built-in PD client is compiled
  in as well.
- **`networkdiagnostic` and `meshdiag`** CLI commands, free with border router
  mode.

Check from a mesh node:

```
ot netdata show      # expect an OMR prefix with the 'r' flag, plus routes
```

and on the border router:

```
ot nat64 state
ot br omrprefix
```

## Thread role on the RGB LED

Enabled (`CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE=y`). The on-board WS2812 on
GPIO8 is driven via RMT and shows the device's current Thread role at a glance:

| Role | Colour | RGB |
|---|---|---|
| Leader | red | 40, 0, 0 |
| Router | blue | 0, 0, 40 |
| Child | green | 0, 40, 0 |
| Detached | dim white | 20, 20, 20 |

Handy for telling at a glance, without a console, whether the board formed its
own network (Leader) or joined an existing one (Child, later Router).

All four colours are adjustable in `menuconfig` under *OpenThread Device Role
Indicator*, as is the GPIO — though on this DevKit the LED is physically wired to
GPIO8, so changing the pin only stops the LED working.

This is the constraint that keeps the W5500's RST off GPIO8.

## Time synchronisation

SNTP starts at boot and retries on its own until the link is up
(`CONFIG_OT_BR_SNTP_ENABLE`, server configurable). It sets the clock on **this
board only**.

For mesh nodes, either give each an SNTP client that goes out through this
border router (recommended — no Thread-level changes), or enable
`CONFIG_OPENTHREAD_TIME_SYNC` on *every* node including this one. The latter
changes what goes on the air, so a partial rollout misbehaves.

## Ethernet or PPP?

`projects/ot_br_ppp` does the same over a PPP link on the USB port, with no
extra hardware. Ethernet has these advantages:

1. **Any LAN port works.** The PPP variant needs a peer that runs a PPP client
   on a USB CDC-ACM serial device.
2. **mDNS reaches the LAN.** Ethernet is a real broadcast segment; a
   point-to-point PPP link is not.
3. **Ordinary Router Advertisements give it a global prefix.** Over PPP that
   depends on how the peer handles Neighbor Discovery, or on DHCPv6-PD.
4. **No dependence on the closed BR library accepting an unusual netif type.**
