# esp-thread

OpenThread test projects for the ESP32-C6. All three use the same Thread test
network, so they form one mesh.

| Project | Description |
| ------- | ----------- |
| [`projects/ot_cli`](projects/ot_cli) | Thread node with the OpenThread CLI |
| [`projects/ot_br_eth`](projects/ot_br_eth) | Border router with a W5500 Ethernet backbone |
| [`projects/ot_br_ppp`](projects/ot_br_ppp) | Border router with a PPP-over-USB backbone |

`esp-idf/` is a submodule pointing at the `ot-ppp-fix` branch of
[fatalwir/esp-idf](https://github.com/fatalwir/esp-idf/tree/ot-ppp-fix):
ESP-IDF master plus a fix for border routing over a PPP backbone.

## Setup

The toolchain and Python environment are installed into `.espressif/` inside
the workspace, not into your home directory.

Linux:

```sh
git clone --recursive https://github.com/fatalwir/esp-thread.git
cd esp-thread
IDF_TOOLS_PATH="$PWD/.espressif" ./esp-idf/install.sh esp32c6
. ./activate_idf.sh
```

Windows (PowerShell):

```powershell
git clone --recursive https://github.com/fatalwir/esp-thread.git
cd esp-thread
$env:IDF_TOOLS_PATH = "$PWD\.espressif"; .\esp-idf\install.ps1 esp32c6
. .\activate_idf.ps1
```

Activate again in every new terminal.

## Build

```sh
cd projects/ot_br_ppp
idf.py build flash monitor
```

See each project's README for hardware and wiring.

## Note

The Thread network key and PSKc in the `sdkconfig.defaults` files are
placeholder test values. Do not use them for a real network.

## License

Unless a file states otherwise, this repository is licensed under the
[MIT License](LICENSE).

Code taken from the ESP-IDF examples by Espressif Systems stays under
[CC0-1.0](LICENSES/CC0-1.0.txt):

- all of `projects/ot_cli`, which is the ESP-IDF `ot_cli` example;
- the `esp_ot_config.h` files in all projects.

Each source file names its license and copyright holders in its SPDX header.

The `esp-idf` submodule is not part of this repository. ESP-IDF is licensed
under Apache-2.0, with some third-party components under their own licenses.
