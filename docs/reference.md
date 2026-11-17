# Reference

[한국어](reference_kor.md) · [README](../README.md)

## Address space

Namespace URI `urn:wiznet:opcua-node`, usually namespace index 2. All variables are read-only.

```
Objects
└── WIZnet-OPCUA-Node                 device name
    ├── Device                        diagnostics
    └── Channels                      input values
```

### Channels

| Node | NodeId | Type |
|---|---|---|
| `Channel_1` … `Channel_3` | `ns=2;s=Channels/Channel_1` … | Float |

Each channel has two properties: `EngineeringUnits` (when a unit is set) and
`EURange` (`[low, high]`, default `[0, 100]`).

### Device (diagnostics)

| Node | Type | Meaning |
|---|---|---|
| `DeviceName` | String | Device name |
| `IPAddress` | String | Current IPv4 address |
| `FwVersion` | String | Firmware build |
| `Uptime_s` | UInt32 | Seconds since boot |
| `FreeHeap` | UInt32 | Free heap, bytes |
| `RebootCount` | UInt32 | Unplanned resets (watchdog, brownout, glitch), kept across reboots |
| `RebootReason` | String | Reason for this boot: `power`, `watchdog`, `user`, `brownout`, `glitch`, `run-pin`, `debugger`, `unknown` |
| `LinkStatus` | String | `up` or `down` |
| `RawFrame` | String | Last input line |
| `UsbFrameCount` / `UsbErrorCount` | UInt32 | USB console frames accepted / rejected |
| `UartFrameCount` / `UartErrorCount` | UInt32 | UART0 frames accepted / rejected |
| `ModbusPollCount` / `ModbusTimeoutCount` | UInt32 | Reserved; 0 in this release |
| `TimeSync` | String | `Unsynchronised` or `Synchronised` |

NodeIds are `ns=2;s=Diag.<Name>`, for example `ns=2;s=Diag.FreeHeap`.

## Server limits

| Item | RP2350 | RP2040 |
|---|---|---|
| Concurrent clients (secure channels) | 4 | 4 |
| Sessions | 8 | 8 |
| Subscriptions, whole server | 16 | 16 |
| Monitored items, whole server | 400 | 200 |
| Longest KeepAlive / publishing interval | 40 s | 40 s |
| Subscription lifetime after the last Publish | 120 s | 120 s |

The item and lifetime limits are CMake cache options in
`port/open62541/CMakeLists.txt`.

## Status codes you may see

| Status code | What it means | Why | What to do |
|---|---|---|---|
| `Bad_WaitingForInitialData` | The channel has no value yet | No `$DATA:` line has arrived since the board started; the board reports this instead of a made-up 0 | Send a `$DATA:` line over USB or UART0 |
| `Bad_TcpServerTooBusy` | A secure connect was refused | It would have waited too long behind other clients' handshakes, so the server refused at once rather than let it time out | Connect again after a moment; start clients a few seconds apart |
| `Bad_TooManyMonitoredItems` | A new monitored item was refused | The server-wide limit (400 on RP2350, 200 on RP2040) is reached | Remove items you do not need, or spread them over fewer clients |
| `Bad_OutOfMemory` | A new monitored item was refused | Free heap fell below the reserve kept for the clients already connected | Use fewer items, or longer sampling intervals |
| `Bad_CertificateUriInvalid` | The client rejected the server certificate | The URI in the certificate does not match the server's application URI `urn:WIZnet:opcua-node` | Regenerate the certificate with `tools/make_opcua_certs.sh` and rebuild |

## Console commands

| Command | What it does |
|---|---|
| `$DATA:<ch1>,<ch2>,<ch3>` | Set the three channels |
| `GET` | Channel values and frame counters |
| `NET` | Network status |
| `OPCUA` | OPC UA server status |
| `UART` | UART0 input counters |
| `NTP` / `NTP a.b.c.d` | Clock sync status / set and save the NTP server |
| `CLEAR` | Reset the channel values |
| `HELP` | List the commands |

## UART0 input

| Item | Value |
|---|---|
| Pins | GP0 = TX, GP1 = RX, 3.3 V logic |
| Format | 115200 baud, 8N1, no flow control |
| Line | `$DATA:<ch1>,<ch2>,<ch3>` + CR or LF, printable ASCII |

A line that does not parse is counted in `UartErrorCount` and is not printed,
so a noisy wire cannot flood the console. Use the `UART` command to see the
last rejected line.
