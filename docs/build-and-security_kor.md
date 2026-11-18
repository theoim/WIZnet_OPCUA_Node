# 빌드와 보안

[English](build-and-security.md) · [README](../README_kor.md)

암호화 연결, 다른 IP 주소, 다른 보드용 이미지가 필요하면 펌웨어를 직접 빌드합니다.

| 단계 | 내용 |
|---|---|
| 1 | 도구 설치와 클론 |
| 2 | 보드와 보안 설정 고르기 |
| 3 | 인증서 생성 (보안 빌드만) |
| 4 | 빌드와 펌웨어 올리기 |
| 5 | Basic256Sha256으로 접속 |
| 6 | IP 주소 바꾸기 |

## 1단계: 도구 설치와 클론

| 도구 | 참고 |
|---|---|
| Arm GNU Toolchain (`arm-none-eabi-gcc`) | 10.3-2021.10으로 시험했습니다 |
| CMake | 3.13 이상 |
| make 도구 | MinGW Make, GNU Make, Ninja 중 하나 |
| Python 3 | open62541 빌드가 Python으로 소스를 생성합니다 |
| Git | 서브모듈이 필요합니다 |
| Bash와 OpenSSL | 보안 빌드만 필요합니다. Windows는 Git Bash에 둘 다 들어 있습니다 |

```bash
git clone --recursive https://github.com/theoim/WIZnet_OPCUA_Node.git
cd WIZnet_OPCUA_Node
```

> **`--recursive`로 클론해 주세요.** Pico SDK, Mbed TLS, open62541, ioLibrary는 서브모듈입니다.
> 서브모듈은 다른 저장소를 시험한 버전 그대로 끼워 넣은 것이라, 이 옵션 없이 클론하면 폴더가
> 비어 있어 configure 단계에서 빌드가 멈춥니다. 이미 클론했다면
> `git submodule update --init --recursive`를 실행합니다.

## 2단계: 보드와 보안 설정 고르기

| 옵션 | 값 |
|---|---|
| `BOARD_NAME` | `W6300_EVB_PICO2`(기본), `W5500_EVB_PICO2`, `W6100_EVB_PICO2`, `W55RP20_EVB_PICO`, `W5500_EVB_PICO`, `W6100_EVB_PICO`, `W6300_EVB_PICO` |
| `OPCUA_ENABLE_ENCRYPTION` | `OFF`(기본, None만) 또는 `ON`(Basic256Sha256 추가) |

> **보안 빌드는 RP2350 보드에서만 됩니다.** RP2040 보드로 보안 빌드를 하면 빌드가 일부러
> 멈춥니다. 암호화 키를 만들려면 예측할 수 없는 난수가 필요한데 RP2040에는 하드웨어 난수
> 발생기가 없고, RSA 핸드셰이크도 약 5초 걸리기 때문입니다([시험 결과](test-results_kor.md)).
> 그래서 RP2040 보드는 `OPCUA_ENABLE_ENCRYPTION=OFF`로 빌드합니다.

## 3단계: 인증서 생성 (보안 빌드만)

```bash
bash tools/make_opcua_certs.sh
```

스크립트가 `examples/opcua_node/src/opcua_certs.c`와 `examples/opcua_node/inc/opcua_certs.h`를
만듭니다. 두 파일은 git에 올라가지 않도록 설정되어 있습니다.

> **이미지 안에 개인키가 들어갑니다.** 펌웨어가 키를 플래시에 담고 부팅하기 때문에, `.uf2` 파일을
> 가진 사람은 키도 갖게 됩니다. 그래서 보안 이미지는 릴리스로 배포하지 않습니다. 설치할 때마다
> 인증서를 직접 생성해 주세요.

인증서에는 보드 주소(`192.168.11.2`)와 애플리케이션 URI `urn:WIZnet:opcua-node`가 들어갑니다.
클라이언트는 인증서의 URI와 서버가 알리는 URI를 비교해서, 다르면 `Bad_CertificateUriInvalid`로
거부합니다. 그래서 주소나 URI를 바꿀 때는 인증서도 함께 다시 만듭니다(6단계).

## 4단계: 빌드와 펌웨어 올리기

```bash
cmake -S . -B build -G "MinGW Makefiles" -DBOARD_NAME=W6300_EVB_PICO2 -DOPCUA_ENABLE_ENCRYPTION=ON
cmake --build build --target opcua_node
```

결과물은 `build/examples/opcua_node/opcua_node.uf2`입니다. README와 같이 BOOTSEL로 올립니다.
부팅 로그에 `security=`와 빌드 커밋이 나오므로, 지금 어떤 이미지가 돌고 있는지 확인할 수 있습니다.

Linux·macOS에서는 `-G "Unix Makefiles"`를 쓰거나 `-G`를 빼면 됩니다.

## 5단계: Basic256Sha256으로 접속

1. README와 같이 UaExpert에 서버를 추가합니다.
2. **Basic256Sha256 - Sign & Encrypt**를 고릅니다.
3. UaExpert가 서버 인증서를 보여 줍니다. **Trust Server Certificate**를 고릅니다.
4. 접속합니다.

서버는 `Basic256Sha256 - Sign`, `ECC_nistP256`, `None`도 제공합니다. 일반 OPC UA 클라이언트가
공통으로 지원하는 정책은 Basic256Sha256이므로 이것을 사용합니다.

> **서버는 모든 클라이언트 인증서를 받아들입니다.** 통신은 암호화되지만, 서버에 클라이언트
> 인증서를 대조할 신뢰 목록이 없습니다. 그래서 직접 관리하는 네트워크에 두고 사용합니다.

## 6단계: IP 주소 바꾸기

1. `examples/opcua_node/src/opcua_settings.c`의 기본값(`out->ip`, `out->gw`, `out->sn`)을 바꿉니다.
2. 보안 빌드라면 `tools/make_opcua_certs.sh`의 `HOST_IP`도 같은 주소로 바꾸고 스크립트를 다시 실행합니다.
3. 빌드하고 펌웨어를 올립니다.

> **저장된 설정이 기본값보다 우선합니다.** 보드는 `NTP a.b.c.d` 명령을 받거나 예기치 않게
> 리셋되면 설정을 플래시에 저장하고, 다음 부팅부터 저장된 값을 씁니다. 그래서 설정이 한 번이라도
> 저장됐다면 새 기본 주소가 쓰이지 않습니다. 이때는 Raspberry Pi의 `flash_nuke.uf2`로 플래시를
> 먼저 지우고([방법](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html#resetting-flash-memory)),
> 새 이미지를 올립니다.

## 릴리스 이미지 다시 빌드하기

Releases 페이지의 `.uf2` 파일은 바이트 단위까지 같게 다시 빌드할 수 있습니다. 릴리스
태그를 받고 `SOURCE_DATE_EPOCH`를 태그의 커밋 시각으로 정합니다.

```bash
git clone --recursive --branch v1.0.1 https://github.com/theoim/WIZnet_OPCUA_Node.git
cd WIZnet_OPCUA_Node
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
cmake -S . -B build -G "MinGW Makefiles" -DBOARD_NAME=W6300_EVB_PICO2
cmake --build build --target opcua_node
sha256sum build/examples/opcua_node/opcua_node.uf2
```

**무엇인가:** 이미지에는 보드가 부팅 때 출력하는 표식 두 개가 들어갑니다. `git describe`의
버전과 빌드 시각입니다.

**왜 이 변수가 필요한가:** 빌드 시각은 빌드할 때마다 달라서, 코드가 같아도
`SOURCE_DATE_EPOCH` 없이는 체크섬이 맞지 않습니다. 이 변수가 있으면 CMake가 현재 시각
대신 이 값을 씁니다.

**그래서:** 이 변수를 정하면 체크섬이 릴리스의 `SHA256SUMS`와 같아집니다. 1.0.1은
경로 길이가 다른 디렉터리에서 깨끗하게 빌드해 확인했습니다(W6300-EVB-Pico2 3회, W55RP20-EVB-Pico 2회)(Windows 11, 1단계의 도구).
