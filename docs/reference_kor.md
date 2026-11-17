# 레퍼런스

[English](reference.md) · [README](../README_kor.md)

## 주소 공간

네임스페이스 URI는 `urn:wiznet:opcua-node`이고, 인덱스는 보통 2입니다. 모든 변수는 읽기 전용입니다.

```
Objects
└── WIZnet-OPCUA-Node                 장치 이름
    ├── Device                        진단 값
    └── Channels                      입력 값
```

### Channels

| 노드 | NodeId | 타입 |
|---|---|---|
| `Channel_1` … `Channel_3` | `ns=2;s=Channels/Channel_1` … | Float |

채널마다 속성이 두 개 있습니다. `EngineeringUnits`(단위가 설정된 경우)와 `EURange`(`[하한, 상한]`, 기본 `[0, 100]`)입니다.

### Device (진단 값)

| 노드 | 타입 | 뜻 |
|---|---|---|
| `DeviceName` | String | 장치 이름 |
| `IPAddress` | String | 현재 IPv4 주소 |
| `FwVersion` | String | 펌웨어 빌드 |
| `Uptime_s` | UInt32 | 부팅 후 경과 시간(초) |
| `FreeHeap` | UInt32 | 남은 힙(바이트) |
| `RebootCount` | UInt32 | 예기치 않은 리셋 횟수(워치독, 저전압, 글리치). 재부팅해도 유지 |
| `RebootReason` | String | 이번 부팅 이유: `power`, `watchdog`, `user`, `brownout`, `glitch`, `run-pin`, `debugger`, `unknown` |
| `LinkStatus` | String | `up` 또는 `down` |
| `RawFrame` | String | 마지막 입력 줄 |
| `UsbFrameCount` / `UsbErrorCount` | UInt32 | USB 콘솔 프레임 수락 / 거부 수 |
| `UartFrameCount` / `UartErrorCount` | UInt32 | UART0 프레임 수락 / 거부 수 |
| `ModbusPollCount` / `ModbusTimeoutCount` | UInt32 | 예약 항목. 이번 릴리스에서는 0 |
| `TimeSync` | String | `Unsynchronised` 또는 `Synchronised` |

NodeId는 `ns=2;s=Diag.<이름>` 형식입니다. 예: `ns=2;s=Diag.FreeHeap`.

Device 폴더에는 이 밖에도 약 25개 노드(`Retransmit*`, `Timer*`, `Socket*`, `HandshakeRefused` 등)가
있습니다. `tools/`의 시험 도구가 쓰는 내부 진단 값이며, 릴리스마다 이름이 바뀔 수 있습니다.

## 서버 한도

| 항목 | RP2350 | RP2040 |
|---|---|---|
| 동시 클라이언트 (보안 채널) | 4 | 4 |
| 세션 | 8 | 8 |
| 구독, 서버 전체 | 16 | 16 |
| 모니터 항목, 서버 전체 | 400 | 200 |
| 최대 KeepAlive / 발행 간격 | 40초 | 40초 |
| 마지막 Publish 뒤 구독 유지 시간 | 120초 | 120초 |

항목 수와 유지 시간 한도는 `port/open62541/CMakeLists.txt`의 CMake 캐시 옵션으로 바꿀 수 있습니다.

## 볼 수 있는 상태 코드

| 상태 코드 | 무엇 | 왜 | 어떻게 |
|---|---|---|---|
| `Bad_WaitingForInitialData` | 채널에 아직 값이 없습니다 | 보드가 시작한 뒤 `$DATA:` 줄을 받지 못했습니다. 꾸며낸 0 대신 이 상태를 알립니다 | USB나 UART0로 `$DATA:` 줄을 보냅니다 |
| `Bad_TcpServerTooBusy` | 보안 접속이 거절되었습니다 | 다른 클라이언트들의 핸드셰이크 뒤에서 너무 오래 기다리게 되어, 서버가 타임아웃까지 두지 않고 바로 거절했습니다 | 잠시 후 다시 연결합니다. 클라이언트를 몇 초 간격으로 시작합니다 |
| `Bad_TooManyMonitoredItems` | 새 모니터 항목이 거절되었습니다 | 서버 전체 한도(RP2350 400개, RP2040 200개)에 도달했습니다 | 필요 없는 항목을 지웁니다 |
| `Bad_OutOfMemory` | 새 모니터 항목이 거절되었습니다 | 이미 접속한 클라이언트를 위해 남겨 두는 힙 예비량 아래로 내려갔습니다 | 항목 수를 줄이거나 샘플링 간격을 늘립니다 |
| `Bad_CertificateUriInvalid` | 클라이언트가 서버 인증서를 거부했습니다 | 인증서의 URI가 서버 애플리케이션 URI `urn:WIZnet:opcua-node`와 다릅니다 | `tools/make_opcua_certs.sh`로 인증서를 다시 만들고 다시 빌드합니다 |

## 콘솔 명령

| 명령 | 동작 |
|---|---|
| `$DATA:<ch1>,<ch2>,<ch3>` | 세 채널 값 설정 |
| `GET` | 채널 값과 프레임 카운터 출력 |
| `NET` | 네트워크 상태 출력 |
| `OPCUA` | OPC UA 서버 상태 출력 |
| `UART` | UART0 입력 카운터 출력 |
| `NTP` / `NTP a.b.c.d` | 시간 동기화 상태 출력 / NTP 서버 설정·저장 |
| `CLEAR` | 채널 값 초기화 |
| `HELP` | 명령 목록 |

## UART0 입력

| 항목 | 값 |
|---|---|
| 핀 | GP0 = TX, GP1 = RX, 3.3 V 로직 |
| 형식 | 115200bps, 8N1, 흐름 제어 없음 |
| 한 줄 | `$DATA:<ch1>,<ch2>,<ch3>` + CR 또는 LF, 출력 가능한 ASCII |

해석할 수 없는 줄은 `UartErrorCount`에 세고 화면에는 출력하지 않습니다. 잡음이 많은 선에서
콘솔이 넘치지 않게 하려는 것입니다. 마지막으로 거부된 줄은 `UART` 명령으로 확인합니다.
