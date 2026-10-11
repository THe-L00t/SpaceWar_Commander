# Ruins Enhanced 구면 맵

현재 맵: Project/assets/model/map/future_ruins_planet_separated.glb

2026-10-11 사용자가 지정한 Expedition_Ruins_Enhanced/future_ruins_planet_separated.glb를 그대로 복사했습니다(client2 `2f15728`). Client와 Server의 에셋 복사 대상과 Shared의 맵 경로가 이 파일을 사용합니다. 기존 ResourceManager → Model → LegacyScene → GRenderer 구조와 공통 PlanetSurface → TerrainSampler 접지 경로를 유지합니다. Assimp나 FBX SDK가 필요하지 않습니다.

## 파서 — Shared/Model 하나 (integrate-client2 병합, 2026-10-11)

client2 는 이 맵을 위해 GLB 전용 리더(`Shared::GlbDocument`·`GlbModelLoader`)를 새로 만들었습니다. integrate-client2 에는 이미 glTF 2.0 리더(`Shared/Model/Readers/GltfReader`)와 디스크 캐시·로딩 스레드 경로가 있어서, 병합하면서 GLB 리더를 따로 두지 않고 그 기능을 기존 리더로 옮겼습니다. 저장소의 모델 파서는 계속 하나이며, 서버도 같은 코드로 접지면을 읽습니다.

| client2 의 GLB 리더 기능 | 병합 후 위치 |
|---|---|
| 접지 노드만 골라 월드 좌표로 펼치기(`ReadGeometry`) | `ReadOptions::collectObject` 를 GltfReader 가 노드 이름으로 지원 → `ModelSource::collected` |
| 충돌 프록시·제작 안내·라벨 노드 숨김(`HiddenNode`) | `ReadOptions::hideAuthoringHelpers`(기본 켬). 노드는 남기고 메시만 떼며, 쓰지 않게 된 메시는 버린다 |
| 반복 나무·건물이 같은 메시를 공유 | 원래 리더도 노드 → 메시 참조로 공유한다 |
| `doubleSided`, `KHR_materials_emissive_strength` | `SourceMaterial::doubleSided`, 발광 계수에 곱한다 |
| 지표면만 뒷면 제거(`Terrain_Expedition_Planet_Closed`) | Engine 의 맵 요청 `onParsed` 훅(맵 규칙이라 Engine 에 둔다) |
| 화면이 쓰는 재질의 내장 이미지만 디코딩 | `ModelBuilder` 가 메시가 쓰지 않는 재질의 텍스처를 건너뛴다 |

모델 캐시 버전을 2로 올렸습니다(`ModelCache.h`). 첫 실행에서 맵을 파싱해 캐시를 쓰고, 다음 실행부터 캐시를 읽습니다.

## 좌표와 시작 위치

원본은 미터 단위, 중심 (0,0,0), 기준 반경 650m의 닫힌 구면입니다. 실제 지형 정점 반경은 약 631.59~700.97m입니다. 원본 노드의 회전·배율·이동을 한 번 적용하고 추가 전체 배율은 1입니다.

**좌표 변환은 하지 않습니다.** 우리 파서는 glTF 좌표를 그대로 둡니다(이유는 `Readers/GltfReader.cpp` 머리의 「좌표계」 주석). client2 의 GlbDocument 는 Z와 삼각형 순서를 반전했으므로, 그쪽에서 잰 좌표를 옮길 때 **Z의 부호를 뒤집었습니다**(시작 방향·평면 시작 좌표). 렌더 메시와 접지 삼각형은 같은 공간에 있습니다.

Shared::kUsePlanarMap=false, kPlanetRadius와 kPlanetModelReferenceRadius는 모두 650입니다. 월드 중심은 (0,-650,0)이며 렌더·플레이어·카메라·NPC·서버 위치 보정과 사격 지형 판정이 같은 상수를 사용합니다. 위쪽은 중심에서 현재 위치로 향하는 방향, 고도는 기준구 위의 거리입니다.

시작 위치는 Landing L1의 단위 방향 (-0.3261576291, 0.9179231599, 0.2259165190)입니다(client2 값은 Z 가 음수). 클라이언트와 서버는 해당 방향의 실제 플랫폼 높이에 몸통-발 오프셋 1m를 더합니다. client2 의 원시 GLB 계산에서 지형 반경은 약 653m, 플랫폼은 653.0201m이며 몸통 중심 반경은 654.0201m입니다. 이 수치는 게임 실행 측정이 아닙니다.

원격 플레이어와 NPC도 받은 좌표는 그대로 사용하고 행성의 위쪽 방향에 캐릭터 자세를 정렬합니다. 통신 패킷에 방향이 없으므로 정면은 해당 지점의 접평면 기준으로 구성합니다.

## 지면 선택

`PlanetSurface::IsGroundObject`가 다음 노드의 면만 고릅니다. 렌더용 파싱과 같은 한 번의 파싱에서 모읍니다(클라: 로딩 스레드의 onParsed → `Build`, 서버: `Load`).

- COL_Expedition_Planet_Terrain(`Shared::kPlanetSurfaceNode`): 표시 지형 Terrain_Expedition_Planet_Closed와 같은 메시. 화면에서는 숨기지만 접지에는 쓴다
- Landing_L*_Platform: 표시용 착륙 플랫폼 4개
- Walkable_Path_*: 있으면 읽는다. 새 자산은 도로를 지형의 색상 텍스처에 통합해 이 메시가 없다

각 노드의 월드 변환을 적용한 좌표를 저장하고 방사 방향에서 가장 바깥 접지면을 찾습니다. 구면의 옆면도 지면이므로 퇴화 판정은 3D 삼각형 면적으로 계산합니다. 규칙을 바꾸면 `PlanetSurface::kGroundRuleTag`의 버전을 올립니다(지금 v2).

일부 COL_Landing_* 프록시는 원본의 부모·자식 변환이 상쇄되어 중심 근처에 배치됩니다. 이 프록시 대신 실제 플랫폼을 사용합니다. 건물 벽·지붕·엄폐물은 높이 조회에 포함하지 않으며 체적 충돌은 기존 시스템의 별도 과제입니다.

## 로딩과 렌더 최적화

- 맵·하늘 행성·캐릭터는 로딩 씬 동안 로딩 스레드가 읽고, 메인이 한 프레임에 하나씩 GPU 에 올린 뒤 CPU 사본을 해제합니다(ReleaseModel).
- 반복 나무·건물은 고유 메시의 CPU·GPU 데이터를 공유하고 각 노드에는 변환만 둡니다.
- LegacyScene은 기존 SoA/API를 유지하며 변경된 로컬 변환·가시성과 부모 변경만 자식에게 전달합니다. 초기화 뒤 정적 맵의 행렬을 매 프레임 다시 곱하지 않습니다.
- 충돌 프록시·크기 안내·제작 가이드와 라벨 메시는 표시에서 제외합니다. 해당 노드는 변환 계층을 유지합니다.
- 모든 면이 바깥 방향인 닫힌 지표면만 뒷면 제거를 적용합니다. 건물과 나무의 원본 양면 설정, 지형의 정점 색상은 유지합니다.
- GRenderer 는 메시별 로컬 AABB로 프러스텀 밖의 메시를 그리지 않고, 연속한 draw 의 같은 PSO·재질·메시 바인딩을 생략합니다(SKY_PLANET.md).

현재 파일에는 지형·도로 색상(4096×2048), 콘크리트와 부식 금속 색상(각 1024×1024)의 내장 PNG 3장이 있습니다. WIC 메모리 디코딩으로 각 이미지를 한 번 읽고 해당 재질들이 같은 GPU 텍스처를 공유합니다. 스킨·애니메이션은 없으며 기본 PBR 재질과 정점 색상을 유지합니다. clearcoat/transmission/ior/sheen은 기존 셰이더의 기본 PBR 표현으로 표시합니다.

## 메모리와 성능 확인

다음은 client2 가 원본 데이터와 자료구조 크기로 계산한 정적 수치입니다. 프로세스 메모리나 FPS 실측이 아니며, 병합 후 우리 파서로 다시 잰 값도 아닙니다.

| 데이터 | 크기 |
|---|---:|
| 새 GLB 파일 | 약 57.56 MiB |
| 제작 가이드 제외 후 공유 렌더 정점·인덱스 | 약 86.64 MiB |
| 내장 텍스처 RGBA8 픽셀 합계 | 40.00 MiB |
| 지면·플랫폼 위치/삼각형/BVH | 약 21.45 MiB |
| 교체 직전 프로젝트 맵의 공유 렌더 정점·인덱스 | 약 118.37 MiB |

렌더 노드는 509개, 고유 메시 147개, 고유 primitive 410개, 컬링 전 draw 인스턴스는 1,580개입니다. 공유 렌더 데이터는 1,243,648정점/1,352,448삼각형이며 정점당 60바이트인 swc::Vertex의 필드 크기로 계산했습니다. 접지 입력은 335,687정점/664,592삼각형입니다.

콘솔 로그 창(Client/Log.h)과 VS 출력창의 [Memory] 줄은 before map load / map parsed / map GPU uploaded / map CPU released / sky planet CPU released / character CPU released와 30초마다 runtime의 private·workingSet·peakWorkingSet·cpuModel·cachedModels를 출력합니다. peakWorkingSet은 이전 최대값이라 해제해도 내려가지 않습니다. 모델 캐시·파싱 시간은 같은 콘솔의 [모델] 줄에 나옵니다.

창 제목의 FPS, BLAS/TLAS, 행성 LOD, Draw, Cull, Tri는 실제 실행 시 확인할 수 있습니다. Tri는 GPU에 제출한 삼각형 수이며 뒷면·깊이 판정 뒤 화면에 남은 수가 아닙니다.

## 이전 맵

이전 future_ruins_realistic.obj(687MB, 반경 1,600m)는 저장소에 남아 있지만 게임은 읽지 않습니다. 그 맵의 접지 규칙(Planet_Core·Asphalt 등)은 v1 이며 git 기록에 있습니다.

## 하늘 행성

SkyPlanet의 FPS_LOD_Optimized LOD2·LOD3 GLB는 별도 원거리 배경입니다. 이번 Ruins Enhanced의 Separated Planet은 플레이 가능한 맵입니다. 배경 배치와 화면 크기별 LOD는 SKY_PLANET.md에 설명합니다.
