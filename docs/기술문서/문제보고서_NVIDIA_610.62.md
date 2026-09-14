# NVIDIA 610.62 드라이버에서 D3D12 푸시버퍼가 회수되지 않는 문제

> **문제 보고서 · SpaceWar Commander 클라이언트**
> 작성 2026-09-03 · 작성자 이태형(클라이언트·통합) · 덤프 채집 2026-08-26 14:09
> **상태: 원인 미확정 · 검증 대기**
>
> HTML 판(인쇄·열람용): [`문제보고서_NVIDIA_610.62.html`](문제보고서_NVIDIA_610.62.html)

표기 규칙 — **[실측]** 덤프로 직접 확인 / **[정황]** 근거는 있으나 미검증 / **[미지]** 모름.

---

## 1. 한 장 요약

SpaceWar 클라이언트를 실행하면 **1~2분간 정상 동작하다가** 시스템 커밋 메모리가 급격히 늘기 시작하고, 프레임 시간이 100ms를 넘어가며, 끝내 화면이 검게 변하고 응답이 멈춘다. 메모리는 5분 안에 64GB에 도달한다.

덤프 실측 결과 **누수의 주체는 우리 코드가 아니라 GPU 드라이버였다.** 프로세스 커밋의 **98.1%** 가 NVIDIA D3D12 드라이버가 할당한 **32MB 크기의 write-combined 블록 1,985개**였고, 이 블록들은 한 번도 회수되지 않았다. **[실측]**

문제 PC는 **NVIDIA 610.62**(2026-06-16 릴리즈)를, 재현되지 않는 개발 PC는 **610.88**(2026-07-28 릴리즈)을 쓴다. NVIDIA는 610.88에서 *"DX12 모드에서 장시간 플레이 후 발생하는 간헐적 긴 멈춤"* 과 *"R610 드라이버에서 RTX 50 시리즈에 관측된 크래시"* 를 수정했다고 공표했다 — 근거는 10절. **[정황]**

> **확인해 둘 사실** — 지도교수가 문제를 재현한 **2026-08-26** 시점에, 수정본인 **610.88은 이미 한 달 전(7월 28일)에 배포되어 있었다.** 문제 PC는 두 달 묵은 610.62를 쓰고 있었다.

---

## 2. 드라이버 정보

버전 번호는 덤프의 모듈 목록에서 직접 읽었다. **[실측]**

| 항목 | 문제 발생 환경 | 정상 동작 환경 |
|---|---|---|
| 제조사 | NVIDIA Corporation | NVIDIA Corporation |
| 제품 | GeForce Game Ready Driver | GeForce Game Ready Driver |
| **드라이버 버전** | **610.62** | **610.88** |
| WDDM 버전 | `32.0.16.1062` | `32.0.16.1088` |
| 릴리즈 | 2026-06-16 (WHQL) | 2026-07-28 (WHQL) |
| 드라이버 파일 날짜 | — (덤프에 없음) | 2026-07-22 |
| D3D12 UMD 모듈 | `nvwgf2umx.dll` | `nvwgf2umx.dll` |
| 설치 무결성 | 정상 — `nv*` 모듈 8개 전부 동일 버전 | 정상 |

드라이버 설치 자체는 정상이다. 버전이 뒤섞인 흔적(설치 실패 시 흔한 증상)은 없었다.

---

## 3. 테스트한 PC 환경

같은 소스, 같은 저장소에서 빌드했으나 결과가 갈렸다.

| 항목 | 지도교수 PC | 개발 PC (이태형) | 팀원 PC (안병규·한현우) |
|---|---|---|---|
| GPU | **GeForce RTX 5080** (Blackwell) | GeForce RTX 4070 Laptop (Ada) | **미수집** |
| **드라이버** | **610.62** | **610.88** | **미수집** |
| CPU / RAM | Ryzen 7 7700 / 128GB | — / 31.7GB | 미수집 |
| OS | Windows 11 · build 26200 | Windows 11 · build 26200 | 미수집 |
| 빌드 구성 | **x64 Debug** (디버그 계층 ON) | Release 위주 (Debug 시험 여부 불확실) | 불확실 |
| 결과 | **재현됨** | 재현 안 됨 | 재현 안 됨 (보고) |

> ### ⚠ 이 보고서의 가장 큰 공백
> **팀원 2명의 드라이버 버전이 수집되지 않았다.** 이 보고서의 핵심 논거는 "610.62에서는 발생하고 610.88에서는 발생하지 않는다"인데, 현재 그 근거는 **PC 두 대**뿐이다.
> 팀원 PC가 610.88 이상이면 논거가 크게 강해지고, **610.62인데도 정상이라면 논거가 무너진다.**
> 수집은 팀원이 PowerShell에 한 줄만 붙여넣으면 된다 — 9절 참조.

---

## 4. 증상

- 실행 후 **1~2분간은 60fps로 정상** 동작한다.
- 그 뒤 **커밋 메모리가 먼저 늘기 시작**하고, 이어서 게임이 느려진다. 순서가 중요하다 — 누수가 먼저다.
- 프레임 시간이 **100ms 이상**으로 떨어진다. 루프가 폭주하는 것이 아니라 **기어간다.**
- 끝내 **화면이 검게 변하고** 응답이 멈춘다. 이 시점에도 렌더 루프 자체는 돌고 있다.
- 메모리는 **5분 안에 64GB**에 도달한다.
- **멀티플레이어는 조건이 아니다.** 클라이언트를 1개만 띄워도 발생한다.

---

## 5. 덤프 실측 증거

2026-08-26 14:09:39 채집 · 63.2GB 전체 메모리 덤프 · 디버거로 수동 채집(크래시 아님).

### 커밋은 63GB인데 실제로 쓴 메모리는 3.2GB뿐이다

| | 값 |
|---|---|
| 커밋(PagefileUsage) | **64,513 MB = 63.0 GB** |
| 작업집합 | **3,253 MB = 3.2 GB** |

잡아두기만 하고 **거의 건드리지 않은 메모리**다. 블록 내용을 표본 조사하니 **93~100%가 0**이었다. 일반적인 "메모리 릭"(할당한 데이터를 해제하지 않음)과는 성격이 다르다.

### 범인의 서명

```
COMMIT / PRIVATE / prot 0x404 (PAGE_READWRITE | PAGE_WRITECOMBINE) / 정확히 32.00 MB
  1,985 개  =  62.0 GB   (전체 커밋의 98.1%)
  0x01C8D2A80000 ~ 0x01DA5F5C0000   32MB 간격으로 연속, 불연속 5곳뿐
  AllocationBase 가 전부 각자 다름  =  독립된 VirtualAlloc 1,985 회
```

`PAGE_WRITECOMBINE`은 사실상 그래픽 드라이버 전용 속성이다(CPU가 쓰고 GPU가 읽는 업로드·커맨드 메모리). 블록 일부에서 GPU 푸시버퍼 패킷 형태의 24바이트 주기 반복이 발견됐다. **우리 애플리케이션의 데이터가 아니다.**

### 프로세스는 CPU를 태우지 않고 «기다리고» 있었다

| 측정 항목 | 값 | 의미 |
|---|---|---|
| 프로세스 수명 | 7분 08초 | 14:02:31 시작 → 14:09:39 채집 |
| **CPU 사용** | **54초 (12.6%)** | 느려진 원인이 연산이 아니라 **대기** |
| 스레드 | 17개 | 스레드 누수 아님 |
| 핸들 | 515개 | 핸들 누수 아님 |
| 드라이버 워커 스레드 | 7개 / 7분간 0.02초 | 회수 작업이 **돌지 않았다** |
| 덤프 시점 상태 | 17개 전원 대기 | 실행 중인 스레드가 하나도 없음 |

### 메인 스레드는 드라이버 안에서 멈춰 있었다

```
Rip    ntdll.dll+0x160A04              커널 대기 중
+0     ntdll.dll+0xCFA44
+48    KERNELBASE.dll+0x428D1          WaitForSingleObject 계열
+192~  nvwgf2umx.dll      (12 프레임)   ← 드라이버가 건 대기
+896~  D3D12SDKLayers.dll (42 프레임)   ← 디버그 계층 (스택 최다 모듈)
       D3D12Core.dll (19) / Client.exe (31) / ucrtbased.dll (11)
```

우리 코드의 GPU 대기(`WaitForSingleObject`)였다면 스택에 `nvwgf2umx` 프레임이 없어야 한다. **D3D12 호출 안에서 드라이버가 건 다른 대기**다.

---

## 6. 측정으로 배제된 원인

아래는 전부 덤프 실측으로 기각됐다. 다시 검토할 필요가 없다.

| 가설 | 기각 근거 |
|---|---|
| 애플리케이션 코드의 힙 누수 | 커밋의 98.1%가 드라이버 write-combined 블록. 우리 몫은 1GB 미만 |
| 스레드 누수 | 스레드 17개 |
| 핸들 누수 | 핸들 515개 |
| 셰이더 컴파일러 버전 차이 | 양쪽 `dxcompiler.dll`이 `1.8.2502.11`로 동일 |
| 드라이버 572.16~576.88 결함 계열 | 실제 버전은 610.62 — 해당 계열이 아님 |
| 멀티플레이어 / 다중 실행 | 클라이언트 1개만 띄워도 발생 |
| 디바이스 제거가 누수를 유발 | 강제 제거 실험에서 30초간 커밋 증가 0MB |
| 매 프레임 TLAS 재빌드 (개발 PC 조건) | 76,345회 빌드에 증가 0MB — 단 610.88·디버그 계층 없음 조건 |

---

## 7. 예상 원인

> **확정된 것과 모르는 것의 경계**
> **확정 [실측]** — 누수의 주체는 NVIDIA D3D12 드라이버의 32MB 푸시버퍼이며, 회수되지 않고 계속 새로 할당됐다. 우리 힙이 아니다.
> **모름 [미지]** — **왜 1~2분 뒤부터 회수에 실패하기 시작하는가.** 아래 세 후보는 **하나도 검증되지 않았다.**

### 1순위 — 드라이버 610.62의 D3D12 버퍼 회수 결함 [정황]

NVIDIA는 문제 PC가 쓰던 610.62의 **다음 버전인 610.88에서** 아래 두 건을 수정했다고 공표했다. 출처와 원문은 10절.

> "Fixed intermittent long pauses that could occur after extended gameplay sessions in **DX12 mode**" — Path of Exile 2

> "Resolved game crashes observed on **RTX 50 Series GPUs with R610 drivers**" — Halo: Campaign Evolved

우리 증상은 **DX12 · 시간 경과형 · RTX 50 시리즈 · R610 브랜치**라는 네 조건을 모두 만족한다. 또한 R610 브랜치에는 Linux 쪽에서도 **VRAM 관련 미해결 문제**가 보고되어 있다.

**주의 — 이것은 정황이지 증명이 아니다.** 릴리즈 노트의 수정 항목은 **특정 게임 이름으로** 적혀 있고, NVIDIA는 근본 원인을 공개하지 않았다. 우리 문제와 같은 버그인지 알 수 없다. 다만 **검증 비용이 사실상 0**이라 1순위에 둔다.

### 2순위 — D3D12 디버그 계층 [정황]

지도교수는 **Debug 빌드**로 실행했고 `D3D12SDKLayers.dll`이 로드되어 있었다. 디버그 계층은 호출 스택에서 **가장 많이 등장하는 모듈(42프레임)** 이다. 디버그 구성에서 메모리가 크래시까지 계속 오른다는 보고 자체는 외부에 실재한다(10절).

**주의:** 스택이 디버그 계층을 통과한다는 것은 **경로가 그렇다는 사실**이지 **디버그 계층이 회수를 막았다는 증명이 아니다.**

### 3순위 — 매 프레임 TLAS 전체 재빌드 [정황]

클라이언트는 매 프레임 상위 가속구조(TLAS)를 `PREFER_FAST_TRACE`로 **전체 재빌드**한다. 갱신(refit)이 아니다. 가속구조 빌드는 드라이버가 내부 커맨드 버퍼를 가장 크게 소비하는 경로다.

**주의:** 개발 PC에서 76,345회 빌드에 증가 0MB로 기각된 적이 있으나, 그 실험은 **610.88 · 디버그 계층 없음** 조건이었다. 조건이 다르므로 완전히 배제할 수 없다.

### 애플리케이션 코드 검토 결과

렌더러와 가속구조 코드를 검토했다. **D3D12 규격 위반이나 명백한 오용은 없다.** 프레임 루프에 리소스 생성이 0건이고, 가속구조는 선할당되며, 빌드 후 UAV 배리어가 정상적으로 걸려 있다. 다만 **드라이버가 흔히 마주치지 않는 패턴** 세 가지가 있다.

| 패턴 | 왜 주목하는가 |
|---|---|
| 커맨드 얼로케이터 1개를 매 프레임 `Reset()` | `CommandAllocator::Reset()`이 **드라이버가 내부 커맨드 메모리를 회수하는 바로 그 지점**이다. 회수 실패가 일어난다면 가장 유력한 자리. 정석은 프레임당 1개 |
| 매 프레임 TLAS 전체 재빌드 | 가장 비싼 빌드 옵션으로 갱신 없이 매번 처음부터 빌드 |
| 파이프라이닝 없음 | `Present` 직후 GPU 완료를 완전히 기다린다. 매 프레임 큐를 비우는 패턴은 상용 게임이 쓰지 않는다 |

---

## 8. 해결 방법

비용이 낮고 결과가 이분법으로 갈리는 순서. **1번에서 끝날 가능성이 있다.**

1. **드라이버를 610.88 이상으로 업데이트하고 1회 실행** — *비용: 수 분*
   증상이 사라지면 드라이버 세대 문제로 확정된다. 계속되면 1순위 후보가 즉시 기각되고 2·3순위로 넘어간다. 어느 쪽이든 결론이 나오는 유일한 무료 실험이다.
2. **Release 빌드로 실행** — *비용: 빌드 1회*
   디버그 계층이 제거된다. 증상이 사라지면 2순위(디버그 계층)가 원인이고, 대응은 `EnableDebugLayer()`를 환경 변수나 커맨드라인으로 제어 가능하게 만드는 것이다.
3. **개발 PC에서 Debug 구성으로 재현 시도** — *비용: 빌드 1회*
   지금까지 "개발 PC에서 재현 안 됨"이 모든 추론의 전제였는데, 그 시험을 Release로만 했다면 전제 자체가 무효다. 클라이언트 **1개 단독**으로 시험한다.
4. **코드 측 조치** — *비용: 반나절*
   커맨드 얼로케이터를 프레임당 1개로 분리, TLAS를 `ALLOW_UPDATE` + refit으로 전환, 프레임 파이프라이닝 도입. 1~3번으로 원인이 좁혀진 뒤에 하는 것이 순서다.
5. **`VirtualQuery` 순회 계측 삽입** — *비용: 하루 + 재방문*
   위가 전부 실패했을 때만. 커밋 영역을 주기적으로 순회해 32MB 블록이 늘어나는 시점과 속도를 기록하고, 지도교수 PC에서 1회 실행한다.

> **재현 시 확인할 지문**
> 커밋 메모리(`PagefileUsage`)가 **32MB 단위로 계단식으로 뛰는지**를 본다. 그것이 이 문제의 지문이다.
> **작업집합은 거의 늘지 않는다**는 점도 함께 확인한다 — 이 둘이 동시에 관측되면 같은 문제다.

---

## 9. 수집이 필요한 정보

아래가 채워지면 1순위 가설의 확신도가 결정된다.

1. **팀원 2명(안병규·한현우)의 GPU·드라이버 버전** — 아래 명령 한 줄
2. **팀원들이 Debug 구성으로 시험한 적이 있는지** — Release로만 시험했다면 "재현 안 됨"은 통제된 근거가 아니다
3. **지도교수 PC에서 Release 빌드로도 발생하는지** — 8절 2번 조치와 동일

```powershell
Get-CimInstance Win32_VideoController |
  Select-Object Name, DriverVersion, DriverDate
```

출력의 `DriverVersion`이 `32.0.16.1062` 형태로 나온다. 뒤 다섯 자리가 NVIDIA 버전이다 — `16.1062` → **610.62**.

---

## 10. 근거 자료

7절 1·2순위 가설의 출처. 인용은 원문 그대로다.

### 드라이버 610.88 릴리즈 노트 — 수정 항목 (1순위 근거)

- **[NVIDIA GeForce 610.88 WHQL Driver Available for Download — DSOGaming](https://www.dsogaming.com/news/nvidia-geforce-610-88-whql-driver-available-for-download/)**
  릴리즈 **2026-07-28**. 원문 인용:
  *"Resolved game crashes observed on RTX 50 Series GPUs with R610 drivers."*
  *"Fixed intermittent long pauses that could occur after extended gameplay sessions in DX12 mode."*
- **[NVIDIA 610.88 Driver: Real Fixes and Open Issues — iTechGuides](https://www.itechguides.com/nvidias-geforce-610-88-driver-fixes-real-problems-but-open-issues-remain/)**
  **같은 두 항목을 독립적으로 확인.** 원문 인용:
  *"Fixes intermittent long pauses after extended DX12 gameplay sessions."*
  *"Resolves crashes reported on RTX 50-series GPUs using the R610 driver branch."*
  미해결 항목으로 *"Prefer Maximum Performance power-management mode may not be applied correctly."* 및 Linux R610의 VRAM 문제·DLSS 스터터링을 기록.
- **[610.88 WHQL Game Ready Driver available — Guru3D 포럼](https://forums.guru3d.com/threads/610-88-whql-game-ready-driver-available.461197/)**
  배포 확인 및 사용자 보고 스레드. (자동 조회는 403으로 차단됨 — 브라우저로 열 것)
- **[610.88 launches without a resolution for Battlefield 6 Season 4's crashing issues — TweakTown](https://www.tweaktown.com/news/112888/nvidia-geforce-driver-610-88-launches-without-a-resolution-for-battlefield-6-season-4s-crashing-issues/index.html)**
  610.88 이후로도 R610 브랜치에 미해결 크래시가 남아 있음을 보도.

### 드라이버 610.62 — 문제 발생 버전

- **[New GeForce Game Ready Driver Released — NVIDIA 공식](https://www.nvidia.com/en-us/geforce/news/june-16-2026-geforce-game-ready-driver/)**
  610.62 WHQL, **2026-06-16 릴리즈** 확인. 공식 문서에 이 문제와 직접 관련된 알려진 이슈 기재는 없다.
- **[GeForce Game Ready Driver 610.62 — NVIDIA 드라이버 상세](https://www.nvidia.com/en-us/drivers/details/272764/)**
  버전 상세 및 다운로드 페이지.
- **[Nvidia 610.62 driver lands with big bug fixes and Empulse support — Neowin](https://www.neowin.net/news/nvidia-61062-driver-lands-with-big-bug-fixes-and-empulse-support/)**
  610.62 수정 내역 정리. **우리 증상과 겹치는 항목은 없다**(= 이 버전에서는 아직 안 고쳐졌다는 뜻).
- **[GeForce GRD 610.62 Feedback Thread — NVIDIA 공식 포럼](https://www.nvidia.com/en-us/geforce/forums/game-ready-drivers/13/587128/)**
  610.62 사용자 문제 보고 스레드. 필요하면 우리 사례를 여기에 신고할 수 있다.

### RTX 50 시리즈 관련 보도

- **[Nvidia is investigating reports of crashes plaguing RTX 5090 and 5080 GPUs with possible driver issues — TechRadar](https://www.techradar.com/computing/gpu/nvidia-is-investigating-reports-of-crashes-plaguing-rtx-5090-and-5080-gpus-with-possible-driver-issues-maybe-hitting-rtx-4000-models-too)**
  RTX 5080·5090에서 크래시·프리즈가 반복 보고되어 NVIDIA가 조사 중임을 보도.

### D3D12 디버그 계층 관련 (2순위 근거)

- **[Memory leak when using DX12 backend in debug configuration — DiligentCore #101](https://github.com/DiligentGraphics/DiligentCore/issues/101)**
  DX12 백엔드를 debug 구성으로 실행하면 *"메모리가 크래시까지 계속 오른다"* 는 보고. **2019년 · RTX 2070 사례로, 우리 건과 동일 사안이라는 근거는 없다.** 이런 부류의 문제가 실재한다는 참고 자료로만 인용한다.
- **[Enabling Debug Layer in D3D12 application causes memory… — Visual Studio Developer Community](https://developercommunity.visualstudio.com/content/problem/698264/enabling-debug-layer-in-d3d12-application-causes-m.html)**
  동일 부류의 보고. **본문 조회에 실패해 제목만 확인했다.**

> ### ⚠ 인용의 한계 — 반드시 읽을 것
> 위 릴리즈 노트 인용은 **NVIDIA가 공표한 수정 항목**이지, **우리 문제의 원인을 NVIDIA가 인정한 기록이 아니다.** 우리 증상과 조건이 겹칠 뿐이며 근본 원인이 같다는 확인은 없다. 이 보고서는 그 차이를 유지한다.
> 또한 위 인용은 **2차 출처(기술 매체)를 통한 것**이다. NVIDIA 공식 릴리즈 노트 PDF를 직접 확인하면 더 강한 근거가 된다.

---

*SpaceWar Commander · 클라이언트 렌더링 (DX12 + DXR 하이브리드) · 지도교수 정내훈*
*근거 자료: `Client.dmp` (63.2GB, 2026-08-26 14:09:39 채집) · NVIDIA GeForce 610.62 / 610.88 릴리즈 노트*
*이 보고서의 모든 수치는 덤프 파싱으로 직접 측정한 값이며, 추정과 미확인 항목은 본문에 표시했다.*
*최종 갱신 2026-09-03*
