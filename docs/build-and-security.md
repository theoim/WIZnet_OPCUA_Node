# Build and security

[한국어](build-and-security_kor.md) · [README](../README.md)

Build the firmware yourself to get encrypted connections, a different IP
address, or an image for another board.

| Step | Contents |
|---|---|
| 1 | Install the tools and clone |
| 2 | Choose the board and the security setting |
| 3 | Generate certificates (security builds only) |
| 4 | Build and flash |
| 5 | Connect with Basic256Sha256 |
| 6 | Change the IP address |

## Step 1: Install the tools and clone

| Tool | Notes |
|---|---|
| Arm GNU Toolchain (`arm-none-eabi-gcc`) | Tested with 10.3-2021.10 |
| CMake | 3.13 or later |
| A make tool | MinGW Make, GNU Make or Ninja |
| Python 3 | The open62541 build generates its source with Python |
| Git | Submodules are required |
| Bash and OpenSSL | Security builds only. Git Bash on Windows includes both |

```bash
git clone --recursive https://github.com/theoim/WIZnet_OPCUA_Node.git
cd WIZnet_OPCUA_Node
```

> **Clone with `--recursive`.** The Pico SDK, Mbed TLS, open62541 and ioLibrary
> are submodules pinned to the tested commits. Without them the build stops at
> configure. If you already cloned, run `git submodule update --init --recursive`.

## Step 2: Choose the board and the security setting

| Option | Values |
|---|---|
| `BOARD_NAME` | `W6300_EVB_PICO2` (default), `W5500_EVB_PICO2`, `W6100_EVB_PICO2`, `W55RP20_EVB_PICO`, `W5500_EVB_PICO`, `W6100_EVB_PICO`, `W6300_EVB_PICO` |
| `OPCUA_ENABLE_ENCRYPTION` | `OFF` (default, None only) or `ON` (adds Basic256Sha256) |

> **Security builds are for RP2350 boards.** On RP2040 boards the build stops on
> purpose: the RP2040 has no hardware random number generator, and the RSA
> handshake takes about 5 s (see [Test results](test-results.md)). So build
> RP2040 boards with `OPCUA_ENABLE_ENCRYPTION=OFF`.

## Step 3: Generate certificates (security builds only)

```bash
bash tools/make_opcua_certs.sh
```

The script writes `examples/opcua_node/src/opcua_certs.c` and
`examples/opcua_node/inc/opcua_certs.h`. Both are git-ignored.

> **The image holds the private key.** Anyone who has the `.uf2` file has the
> key. That is why security images are not published as releases: generate
> your own certificate for each deployment.

The certificate names the board's address (`192.168.11.2`) and the application
URI `urn:WIZnet:opcua-node`. A client rejects the certificate with
`Bad_CertificateUriInvalid` if the URI does not match, so change both together
(Step 6).

## Step 4: Build and flash

```bash
cmake -S . -B build -G "MinGW Makefiles" -DBOARD_NAME=W6300_EVB_PICO2 -DOPCUA_ENABLE_ENCRYPTION=ON
cmake --build build --target opcua_node
```

The image is `build/examples/opcua_node/opcua_node.uf2`. Flash it with BOOTSEL as
in the README. The boot log shows `security=` and the build commit, so you can
tell which image is running.

On Linux or macOS use `-G "Unix Makefiles"`, or leave `-G` out.

## Step 5: Connect with Basic256Sha256

1. Add the server in UaExpert as in the README.
2. Choose **Basic256Sha256 - Sign & Encrypt**.
3. UaExpert shows the server certificate. Choose **Trust Server Certificate**.
4. Connect.

The server also offers `Basic256Sha256 - Sign`, `ECC_nistP256` and `None`. Use
Basic256Sha256: it is the policy common OPC UA clients support.

> **The server accepts any client certificate.** The channel is encrypted, but
> the server does not check client certificates against a trust list. Keep the
> node on a network you control.

## Step 6: Change the IP address

1. Change the defaults in `examples/opcua_node/src/opcua_settings.c`
   (`out->ip`, `out->gw`, `out->sn`).
2. For a security build, change `HOST_IP` in `tools/make_opcua_certs.sh` to the
   same address and run the script again.
3. Build and flash.

> **Saved settings win over the defaults.** The board saves its settings to flash
> after an `NTP a.b.c.d` command or an unexpected reset. If settings were saved,
> the new default address is not used. Erase the flash first with Raspberry Pi's
> `flash_nuke.uf2`
> ([how](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html#resetting-flash-memory)),
> then flash the new image.
