# iCUE5 OpenRGB Plugin — 운영 가이드

> **대상 독자**: 이 플러그인을 유지보수하거나 새 기기를 추가할 때 참고하는 문서.  
> AI(Claude 등)와 함께 작업할 때도 이 문서를 먼저 공유하면 빠르게 진행할 수 있음.

---

## 목차

1. [iCUE 버전 업데이트 시 점검 목록](#1-icue-버전-업데이트-시-점검-목록)
2. [최초 설치 절차 (처음부터 세팅하는 경우)](#2-최초-설치-절차)
3. [새 기기 추가 방법](#3-새-기기-추가-방법)

---

## 1. iCUE 버전 업데이트 시 점검 목록

iCUE가 업데이트된 뒤 플러그인이 작동하지 않으면 아래 순서대로 확인한다.

### 1-1. API 버전 번호 (가장 먼저 확인)

**파일**: `dllmain.cpp`  
**함수**: `CorsairPluginGetAPIVersion()`

```cpp
// 현재 iCUE 5.47 기준 — 버전이 올라가면 이 값이 달라질 수 있음
return 0x69;
```

iCUE가 기대하는 버전 번호와 일치하지 않으면 플러그인이 완전히 무시된다.  
새 버전 번호를 찾는 방법: iCUE 실행 파일(`iCUEDevicePluginHost.exe`)을 HxD 등 헥스 에디터로 열어서 `0x68`, `0x69`, `0x6A` 등 인접 값을 검색하거나, 로그에서 "API version" 관련 메시지를 확인한다.

### 1-2. 플러그인 구조체 크기 (API 버전이 바뀌었다면 함께 확인)

**파일**: `CUESDKDevice.h`  
**구조체**: `CorsairGetInstance_v67`

현재 함수 포인터 19개 × 8바이트 = 0x98(152)바이트.  
iCUE가 다른 크기를 기대하면 즉시 크래시가 발생한다.

**파일**: `dllmain.cpp`  
**매크로**: `#define C_PROPERTIES` (파일 상단)

이 매크로가 v67(19개) 구조체를 활성화한다. 주석 처리하면 v66(11개)으로 다운그레이드된다.

### 1-3. 플러그인 등록 정보 (플러그인 자체가 로드조차 안 될 때)

**파일**: `C:\Program Files\Corsair\Corsair iCUE5 Software\cuepkg.registry` (SQLite DB)

iCUE를 재설치하거나 캐시를 초기화하면 이 DB가 초기화될 수 있다.  
등록 확인:

```
sqlite3 "C:\Program Files\Corsair\Corsair iCUE5 Software\cuepkg.registry" "SELECT name, type FROM packages WHERE name='devplugin-openrgb';"
```

결과가 없으면 → [등록 스크립트 재실행](#2-4-플러그인-db-등록)

### 1-4. DLL 서명 우회 (`version.dll`)

**파일**: `C:\Program Files\Corsair\Corsair iCUE5 Software\version.dll`

iCUE는 플러그인 DLL에 서명 검사를 실행한다. `version_wrapper` 프로젝트가 빌드한 `version.dll`을 iCUE 루트에 놓으면 이 검사가 우회된다.  
iCUE 업데이트 후 이 파일이 교체됐는지 확인한다 (파일 크기가 원본 Windows version.dll보다 훨씬 작아야 함).

### 요약 체크리스트

| 증상 | 확인 위치 |
|------|-----------|
| 플러그인 로드 자체가 안 됨 | `version.dll` 존재 여부, `cuepkg.registry` 등록 |
| 로드는 되나 기기가 아예 안 보임 | `dllmain.cpp` `CorsairPluginGetAPIVersion()` 반환값 |
| 기기는 보이나 즉시 크래시 | `CUESDKDevice.h` 구조체 크기, `C_PROPERTIES` 매크로 |
| 기기는 보이나 LED 조절 불가 | `devices.json` zone 설정, `settings.json` 기기 타입 기본값 |

---

## 2. 최초 설치 절차

### 환경 전제 조건

| 도구 | 설치 위치 / 비고 |
|------|-----------------|
| Visual Studio 2022 Build Tools | MSBuild 포함 옵션 선택 필수 |
| OpenRGB | [openrgb.org](https://openrgb.org) — SDK 서버 기능 사용 |
| sqlite3.exe | `C:\workspace\sqlite3.exe` 에 위치 |
| Git | 소스 클론용 |

### 2-1. 소스코드 준비

```powershell
git clone https://github.com/chaejunno689/iCUE5-OpenRGB-Plugin.git C:\workspace\CUEORGBPlugin-master
```

### 2-2. OpenRGB 설정

1. OpenRGB 설치 및 실행
2. **Settings → General** : "Start at login", "Start minimized", "Start server" 모두 활성화
3. **SDK Server 탭** → Start Server (포트 기본값 `6742` 유지, IP주소를 127.0.0.1로 설정)
4. **Devices 탭** → Corsair 기기가 있다면 비활성화 (iCUE와 충돌 방지)

> OpenRGB는 iCUE보다 **먼저** 실행되어야 한다.

### 2-3. 플러그인 빌드

```bat
C:\workspace\build_plugin.bat
```

내부 명령:
```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" ^
  "C:\workspace\CUEORGBPlugin-master\CUEORGBPlugin.sln" ^
  /p:Configuration=Release /p:Platform=x64 ^
  /p:WindowsTargetPlatformVersion=10.0.26100.0 ^
  /t:CUEORGBPlugin /m /v:minimal
```

빌드 결과물: `C:\workspace\CUEORGBPlugin-master\x64\Release\CUEORGBPlugin.dll`

### 2-4. 플러그인 DB 등록

iCUE가 플러그인을 인식하려면 `cuepkg.registry` SQLite DB에 등록이 필요하다.  
**한 번만 실행하면 됨 (iCUE 재설치 시 재실행 필요)**

```bat
C:\workspace\register_openrgb2.bat
```

이 스크립트는 `C:\workspace\register_openrgb_full.sql`을 실행한다.  
등록 확인:
```
sqlite3 "C:\Program Files\Corsair\Corsair iCUE5 Software\cuepkg.registry" "SELECT name FROM packages WHERE name='devplugin-openrgb';"
```
→ `devplugin-openrgb` 출력되면 정상.

### 2-5. 플러그인 파일 설치

iCUE를 완전히 종료한 뒤 아래 파일들을 복사한다.  
**관리자 권한 필요** — 탐색기에서 직접 붙여넣거나 관리자 CMD에서 실행:

```bat
C:\workspace\install_plugin3.bat
```

복사 대상:

| 원본 | 대상 |
|------|------|
| `x64\Release\CUEORGBPlugin.dll` | `C:\Program Files\Corsair\Corsair iCUE5 Software\plugins\OpenRGB\` |
| `dist\plugins\OpenRGB\devices.json` | 위와 동일 폴더 |
| `dist\plugins\OpenRGB\settings.json` | 위와 동일 폴더 |
| `dist\plugins\OpenRGB\images\` | 위와 동일 폴더 |

`version.dll`은 이미 iCUE 루트에 있는 경우 교체 불필요.  
없다면 `version_wrapper` 프로젝트를 별도 빌드해서 `C:\Program Files\Corsair\Corsair iCUE5 Software\version.dll`에 복사.

### 2-6. iCUE 실행 순서

```
1. OpenRGB 실행 (SDK 서버 시작됨)
2. iCUE 실행
3. iCUE → 장치 목록에 OpenRGB 기기 표시 확인
```

---

## 3. 새 기기 추가 방법

### 개요

새 기기를 iCUE에서 제어하려면 **코드 수정 없이** JSON 파일 두 개만 편집하면 된다.

- `dist/plugins/OpenRGB/devices.json` — 개별 기기 설정
- `dist/plugins/OpenRGB/settings.json` — 기기 타입별 기본값

편집 후 두 파일을 설치 경로(`plugins/OpenRGB/`)에 복사하고 iCUE를 재시작하면 반영된다.

### 3-1. OpenRGB에서 기기 정보 확인

OpenRGB를 열면 왼쪽 목록에 기기가 표시된다.  
**반드시 확인해야 할 두 가지:**

1. **기기 이름** (정확히 복사) — `devices.json`의 `"Name"` 필드에 사용
2. **기기 타입** — 헤더 또는 기기 정보 탭에서 확인 (예: GPU, Mousemat, LED Strip, Motherboard 등)

### 3-2. `devices.json`에 항목 추가

```json
{
    "Name": "OpenRGB에 표시된 정확한 기기 이름",
    "Thumbnail": "images/gpu/default/thumbnail.png",
    "Image": "images/gpu/default/promo.png",
    "InheritDefault": false,
    "Zones": [
        {"Zone": 0}
    ],
    "Views": [
        {
            "Image": "images/gpu/default/device_view.png",
            "PolyGenerator": {
                "Zone": 0,
                "Rect": [5, 5, 95, 95],
                "Spacing": 5
            }
        }
    ]
}
```

**Zone 지정 방식:**

| 방식 | 예시 | 사용 시기 |
|------|------|-----------|
| 인덱스(숫자) | `{"Zone": 0}` | **권장** — 이름 불일치 위험 없음 |
| 이름(문자열) | `{"Zone": "Underglow"}` | OpenRGB 이름과 정확히 일치할 때만 |

zone이 여러 개라면:
```json
"Zones": [
    {"Zone": 0},
    {"Zone": 1},
    {"Zone": 2}
]
```

**`InheritDefault` 옵션:**

| 값 | 동작 |
|----|------|
| `false` | settings.json 기본값 무시, 이 항목만 사용 (권장) |
| `true` | settings.json 기본값에 이 항목을 추가로 덮어씀 |

### 3-3. `settings.json` 기기 타입 기본값 확인

`GetDeviceViewFromJson()` 함수는 기기 타입이 `settings.json`의 `Defaults`에 없으면 **장치 뷰를 아예 적용하지 않는다.**  
현재 등록된 타입:

| 타입 문자열 | 해당 기기 종류 |
|------------|--------------|
| `"Motherboard"` | 메인보드 |
| `"GPU"` | 그래픽카드 |
| `"Keyboard"` | 키보드 |
| `"Mousemat"` | 마우스패드 (Razer Firefly 등) |
| `"LED Strip"` | LED 스트립 (Lian Li Strimer 등) |

기기 타입이 위 목록에 없다면 `settings.json`에 추가:

```json
"Cooler": {
    "Thumbnail": "",
    "Image": "",
    "Zones": [{"Zone": 0}],
    "Views": [
        {"Image": "images/gpu/default/device_view.png",
         "PolyGenerator": {"Zone": 0, "Rect": [5, 5, 95, 95], "Spacing": 5}}
    ]
}
```

타입 문자열 전체 목록 (`RGBController.cpp` `device_type_to_str()` 참조):

```
Motherboard / DRAM / GPU / Cooler / LED Strip /
Keyboard / Mouse / Mousemat / Headset / Headset Stand /
Gamepad / Light / Speaker / Virtual
```

### 3-4. 이미지 추가 (선택)

```
plugins/OpenRGB/images/
└── 기기브랜드_모델/
    └── default/
        ├── thumbnail.png    (기기 목록 아이콘, 약 200×200)
        ├── promo.png        (iCUE 홈 화면 배너)
        └── device_view.png  (LED 에디터 배경 이미지)
```

이미지가 없으면 기존 GPU/LED Strip 기본 이미지를 재사용해도 동작에 문제 없음.

### 3-5. LED가 0개로 표시될 때 (troubleshooting)

iCUE에 기기가 보이지만 LED 조절이 없을 때:

1. **Zone 0이 실제로 존재하는지 확인** — OpenRGB에서 해당 기기의 Zones 탭 확인
2. **LED 수가 0으로 보고되는 경우** — OpenRGB SDK 서버를 재시작하거나 기기를 Custom 모드로 설정
3. **기기 타입이 settings.json에 없는 경우** → 3-3 참고
4. **Zone 이름 불일치** — 문자열 대신 숫자 인덱스(`{"Zone": 0}`) 사용

### AI에게 새 기기 추가를 요청할 때 제공할 정보

```
1. OpenRGB에 표시된 기기 이름 (정확히):
2. OpenRGB의 기기 타입 (GPU/Mousemat/LED Strip 등):
3. OpenRGB Zones 탭에서 보이는 zone 목록과 LED 수:
4. 원하는 이미지 (없으면 기본 이미지 사용):
```

---

## 주요 파일 경로 참조

| 역할 | 경로 |
|------|------|
| 소스코드 | `C:\workspace\CUEORGBPlugin-master\` |
| 빌드 스크립트 | `C:\workspace\build_plugin.bat` |
| 설치 스크립트 | `C:\workspace\install_plugin3.bat` |
| DB 등록 스크립트 | `C:\workspace\register_openrgb2.bat` |
| DB 등록 SQL | `C:\workspace\register_openrgb_full.sql` |
| 설치된 플러그인 폴더 | `C:\Program Files\Corsair\Corsair iCUE5 Software\plugins\OpenRGB\` |
| iCUE 플러그인 레지스트리 | `C:\Program Files\Corsair\Corsair iCUE5 Software\cuepkg.registry` |
| API 버전 코드 | `dllmain.cpp` → `CorsairPluginGetAPIVersion()` |
| 구조체 정의 | `CUESDKDevice.h` → `CorsairGetInstance_v67` |
| 기기별 설정 | `dist/plugins/OpenRGB/devices.json` |
| 타입별 기본값 | `dist/plugins/OpenRGB/settings.json` |
