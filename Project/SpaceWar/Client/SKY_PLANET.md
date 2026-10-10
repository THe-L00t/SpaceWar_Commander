# 원거리 하늘 행성과 LOD

제공한 FPS_LOD_Optimized의 원시 glTF(GLB) 메타데이터와 좌표·정점·인덱스를 분석하여 하늘에 표시할 행성으로 추가했습니다. 게임 실행이나 FPS 측정은 수행하지 않았습니다.

## 배치와 클래스 연결

- 자산: Project/assets/model/sky_planet/LOD2/future_ruins_fps_lod2.glb 및 LOD3/future_ruins_fps_lod3.glb.
- 중심: 월드 (0, 1800, 4200)m. 현재 스폰에서 약 4.63km 거리, 약 23도 위쪽입니다.
- 공통 외곽 반경: 원본 124.11954m → 표시 반경 350m, 배율 약 2.819855. Blender의 기존 배율은 다시 적용하지 않습니다.
- Y축으로 45도 회전합니다. 현재 카메라의 상향 31도 범위에서 하늘을 보면 행성이 보이는 배치입니다.
- SkyPlanet이 ResourceManager → Model → 별도 LegacyScene → GRenderer 경로를 사용합니다.
- 이 행성은 배경 표시용입니다. 플레이어가 걷는 맵은 PLANET_MODEL.md의 Separated Planet 구면 GLB이며 별도로 접지·중력·서버 판정을 사용합니다.
- glTF 2.0의 바이너리 형식인 GLB를 기존 직접 로더로 읽습니다. GLB BIN에 내장된 색상·발광 PNG를 WIC로 디코딩하므로 외부 MTL·PNG는 필요하지 않습니다. 추가 Assimp·FBX SDK 설치는 필요하지 않습니다.
- GLB의 활성 scene만 가져옵니다. LOD2의 scene 1과 LOD3의 scene 2를 사용하므로 같은 파일에 남아 있는 검토용 scene이나 Blender 컨트롤러 배율을 다시 적용하지 않습니다.
- KHR_materials_emissive_strength를 읽어 기존 발광 계수에 반영합니다. 텍스처는 sRGB로 읽고 UV는 glTF의 위쪽 원점 규약을 유지합니다.

위 위치·반경은 SkyPlanet의 center와 SkyPlanet.cpp의 kWorldRadius에서 조정합니다. 다른 모델로 교체할 때는 kAssetRadius도 해당 모델의 외곽 반경으로 갱신합니다.

## LOD 계산

중심 거리 d, 표시 반경 r, 카메라의 현재 수직 FOV f, 실제 렌더 높이 h를 사용합니다.

화면 지름(px) ≈ h × r / (sqrt(d² - r²) × tan(f / 2))

조준·질주로 바뀌는 FOV를 매 프레임 반영합니다. 기준은 제공한 lod_config의 80px이며 전환에 12% 여유를 적용합니다.

| 조건 | 선택 |
|---|---|
| 처음 선택할 때 80px 이상 | LOD2 |
| 처음 선택할 때 80px 미만 | LOD3 |
| 현재 LOD2, 지름이 70.4px 미만 | LOD3로 변경 |
| 현재 LOD3, 지름이 89.6px 초과 | LOD2로 변경 |

두 단계 모두 시작할 때 GPU에 올리고 CPU 정점·픽셀은 단계마다 즉시 해제합니다. 프레임 중 로드·GPU 업로드를 하지 않습니다. 선택하지 않은 LOD는 부모 표시 플래그를 내려 렌더 추출에서 제외합니다. 두 모델을 동시에 그리지 않습니다.

현재 1280×720에서 정상 FOV 60도이면 약 95px, 조준 35도이면 약 173px로 LOD2에 해당합니다. 하늘의 작은 행성 용도에 맞춰 LOD0·LOD1을 복사하거나 로드하지 않습니다. 현재 배치에서는 LOD2가 가장 자세한 단계이며 행성 표면에 접근하는 기능은 포함하지 않습니다.

| 제공한 모델 | 삼각형 |
|---|---:|
| LOD0 (분석만) | 531,844 |
| LOD1 (분석만) | 80,332 |
| LOD2 (현재 기본) | 24,760 |
| LOD3 (작게 보일 때) | 4,698 |

LOD2는 LOD0 대비 제출 삼각형 약 95.34% 감소, LOD3는 LOD2 대비 약 81.03% 감소입니다. 모델 전체의 수치이며 FPS 개선율이나 뒷면 제거 후 래스터 삼각형 실측이 아닙니다. GLB accessor 기준 GPU 정점은 LOD2 51,143개, LOD3 10,284개이며 정점·인덱스 합계는 두 단계 약 3.85MiB입니다. 내장 PNG의 디코딩된 RGBA8은 두 단계 합계 8MiB입니다. GPU 힙 정렬·다른 모델·드라이버 메모리는 별도입니다.

## 렌더 최적화

GRenderer는 업로드할 때 메시별 로컬 AABB를 계산합니다. 프레임의 viewProj에서 D3D의 6개 클립 평면을 추출하고 회전·비균일 배율·반전 등을 반영한 월드 AABB가 시야 밖이면 draw 전에 제외합니다. 현재 맵·캐릭터·행성에 함께 적용하며 프레임마다 임시 배열을 만들지 않습니다. 시야 안에 있는 메시가 다른 물체에 완전히 가려졌는지 판정하는 occlusion query는 추가하지 않았습니다.

행성 GLB의 원본 재질은 양면으로 내보내져 있습니다. SkyPlanet은 Model 초기화의 forceBackfaceCulling 옵션으로 행성 GPU 재질만 단면으로 만들어 뒤쪽 삼각형을 제거합니다. 기존 GLB 맵의 나무 등 양면 재질은 양면으로 유지하여 필요한 표면이 사라지지 않게 합니다. 반전 변환이면 앞면 방향을 바꾼 PSO를 사용합니다. 기존 깊이 검사도 유지하며 연속한 draw의 같은 PSO·재질·메시 바인딩은 생략합니다.

배경 행성은 같은 카메라/FOV와 near 10m, far 20km의 별도 투영으로 먼저 그립니다. 그 뒤 깊이만 초기화하고 기존 맵을 near 0.1m의 메인 투영으로 그립니다. 이 방식은 먼 행성 표면의 깊이 정밀도를 확보하고 맵·캐릭터가 배경 위에 표시되게 합니다. 두 패스의 프레임 상수는 다른 256바이트 CBV에 보관하여 GPU가 같은 메모리의 마지막 값을 읽는 문제를 방지합니다. 배경 행성의 레이 트레이싱은 사용하지 않습니다.

## 현재 GLB 지원 범위

Shared::GlbDocument가 이미지 bufferView와 텍스처·재질 인덱스를 검증하고 BIN 내부 범위를 반환합니다. 클라이언트의 GlbModelLoader는 화면에 사용하는 재질의 이미지만 디코딩하고 같은 이미지·색 공간은 모델 안에서 공유합니다. WIC 메모리 스트림은 동기 디코딩 중에만 BIN을 참조하며, 반환된 ModelData에는 독립적인 RGBA8 픽셀만 남습니다.

현재 내장 PNG/JPEG의 BaseColor·Emissive, TEXCOORD_0, opaque 재질, linear/repeat sampler를 지원합니다. 텍스처는 기존 렌더러와 동일하게 한 mip을 사용하며 mip 체인을 생성하지 않습니다. 외부 이미지 URI·텍스처 변환·다른 UV 세트·노멀/metallic-roughness/occlusion 텍스처·Draco/meshopt 압축·스킨·애니메이션은 지원 범위를 벗어나면 오류로 보고합니다. 현재 행성 GLB는 이 범위에 해당합니다.

## 실행 시 확인할 표시

창 제목은 기존 FPS·메모리 로그와 함께 다음을 표시합니다.

- 행성 LOD2/LOD3 및 계산한 지름(px).
- Draw: 실제 draw 호출 수 / 선택된 LOD와 메인 장면에서 추출한 메시 수.
- Cull: CPU 프러스텀 판정으로 제외한 메시 수.
- Tri: 시야 판정 이후 GPU에 제출한 삼각형 수(M). 뒷면·깊이 탈락 전의 수입니다.

카메라를 지면·하늘·다른 방향으로 돌리면 Cull과 Draw가 달라집니다. 실제 FPS 비교는 같은 위치·시야·해상도와 같은 빌드 구성에서 진행합니다. 시작 메모리는 [Memory] sky planet CPU released 로그에서도 볼 수 있습니다. 빌드·게임 실행·FPS 측정은 사용자가 진행합니다.

기술 참고: [Microsoft D3D12 래스터 상태](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc).

