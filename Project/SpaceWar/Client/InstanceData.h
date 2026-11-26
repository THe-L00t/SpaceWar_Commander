#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <DirectXMath.h>
#include "Handles.h"

// ============================================================
//  Client/InstanceData.h — 「렌더러 수정방향」 3.2 · 3.5
//
//  ★ 이름을 노션 기준으로 맞췄다 (RenderItem → InstanceData, 2026-09-18)
//  ★ 3.2 의 목표 형태로 바꿨다 (2026-10-11)
//    InstanceData = world · prevWorld · mesh · material · flags.
//    Scene 의 Extract 가 이 배열을 채워 프레임 패킷(FramePacket)으로 넘기고,
//    렌더러는 패킷만 받는다 — Scene 을 직접 읽지 않는다(멀티스레딩 명세 M2).
//
//  ★ RenderView 는 3.5 에서 확장된다
//    view · proj · viewProj · prevViewProj · invProj · 지터 · 렌더/출력 크기 ·
//    시간 · 프레임 번호 · 카메라 원점(Vec3d) · 태양 방향까지.
//    그때 C++ 와 HLSL 이 함께 include 하는 공유 상수 헤더로 옮긴다.
// ============================================================

namespace swc {
	// InstanceData::flags — Extract 가 노드의 월드 행렬에서 정한다(반전 · 퇴화).
	//  kInstanceNoRayTracing 만은 노드 설정이다(LegacyScene::SetRayTracingExcluded).
	enum InstanceFlags : uint32_t
	{
		kInstanceMirrored     = 1u << 0,   // 행렬식 < 0 — 와인딩이 뒤집혀 반전 PSO 로 그린다
		kInstanceDegenerate   = 1u << 1,   // 행렬식 ≈ 0 — 그리지 않는다
		// 2026-11-16 — TLAS 에 넣지 않는다(래스터에는 그린다 · 그림자 · 반사 · GI · AO 에는 안 나온다).
		//  예광탄 — 사용자 «예광탄 빼. 총알이 날아가는 경로에 그림자 생겨서 이상해».
		kInstanceNoRayTracing = 1u << 2,
	};

	struct InstanceData {
		DirectX::XMFLOAT4X4 world{ };
		DirectX::XMFLOAT4X4 prevWorld{ };   // 이전 프레임 world — 모션 벡터용(아직 읽는 셰이더 없음)
		MeshHandle mesh = kInvalidMesh;
		MaterialHandle material = kInvalidMaterial;
		uint32_t flags = 0;
	};

	struct RenderView {
		DirectX::XMFLOAT4X4 viewProj{ };
		DirectX::XMFLOAT3   eyePosition{ };   // 프레넬의 시선 벡터(V) 계산용
		// 카메라 자리의 «위» (행성 중심 → 카메라, 단위 벡터) — 2026-10-27 렌더 6단계 [6] 하늘.
		//  하늘과 해의 높이를 이 방향 기준으로 계산한다. 배경 · 메인 패스에 같은 값을 넣는다.
		DirectX::XMFLOAT3   up{ 0.0f, 1.0f, 0.0f };
		// 2026-11-17 지형 조각 컬링(ClusterCull.hlsl) — 행성 중심(월드). w = 1 이면 «지평선 · 행성 반대편» 판정을 쓴다.
		//  0(평면 맵 · 배경 패스 · 모름)이면 프러스텀 · 뒷면 판정만 한다.
		DirectX::XMFLOAT4   planet{ 0.0f, 0.0f, 0.0f, 0.0f };
		bool background = false;             // 배경 패스 뒤 깊이만 초기화한다.
	};

	// ── 파티클 사각형 하나 (2026-11-17 사용자 «파티클 전용 파이프라인», 명세 7절 Render World Particle[]) ──
	//  ParticleSystem(Client/Effects)이 매 프레임 채우고, 렌더러 [7b] 파티클 패스가 카메라를 보는 사각형으로 펴 그린다.
	//  ★ ParticleCommon.hlsli 의 ParticleSprite 와 같은 배치(96 B — 2026-11-25 고정 사각형 축 · 기준점 · 레이 몫이 붙어 48 → 96).
	//    기존 인스턴스 경로(InstanceData · 그리기 묶음 · 본 TLAS)는 거치지 않는다.
	enum ParticleFlags : uint32_t
	{
		kParticleAdditive  = 1u << 0,   // 가산 블렌드(섬광 · 불꽃 — 스스로 빛남). 없으면 알파 블렌드(먼지 · 연기 · 피)
		kParticleLit       = 1u << 1,   // 해 · 하늘 빛을 받는다(알파 묶음). 없으면 색 그대로(자체 발광)
		// 2026-11-25 사용자 «맞은 지점 기준으로 정면 측면 둘다 출력» — 카메라를 보지 않고 axisX · axisY · axisZ 로 고정된 사각형.
		//  없으면 예전처럼 카메라를 보는 사각형(축 = 카메라 오른쪽 · 위를 rotation 만큼 돌린 것).
		kParticleFixedAxes = 1u << 2,
	};
	// ── 2026-11-26 [0b] 파티클 빛 고정 열쇠 — flags 위 비트(ParticleSystem::Extract 가 채운다 · ParticleCommon.hlsli 와 같은 값) ──
	//  사용자 «사라지기 전까지는 주변광 값 고정 하면 안돼?» · «파티클 가림도 없애줘 … 임시니까 진행해줘»(나중에 갓레이를 정하며 다시 고친다).
	//  패킷은 프레임마다 거리로 정렬돼 번호가 바뀌므로, 같은 파티클을 알아볼 풀 칸 번호와 «그 칸의 몇 번째 파티클인지» 를 싣는다.
	//  ParticleLight.hlsl 은 칸에 적어 둔 열쇠가 다르면(새로 태어남) 한 번 재서 적고, 같으면 적어 둔 값을 그대로 쓴다.
	inline constexpr uint32_t kParticleLightKeyShift = 4;       // 4 ~ 15 비트 — 그 칸에 태어난 차례(12비트, 4096 에서 돌아감)
	inline constexpr uint32_t kParticleSlotShift     = 16;      // 16 ~ 27 비트 — 풀 칸 번호(ParticleSystem::kSpritePool 4096 까지)
	inline constexpr uint32_t kParticleKeyMask       = 0xFFFu;  // 두 칸 모두 12비트
	// 레이(그림자 · AO · GI · 반사)가 파티클 구 하나에서 받는 평균 불투명도 몫의 기본값 — 둥근 모양 (1 − r²)² 의 원판 평균 1/3.
	//  플립북은 FlipbookSettings::rayCoverage 로 바꾼다(2026-11-25 — 피 시트는 칸 평균 0.02 ~ 0.05).
	inline constexpr float kParticleRayCoverageRound = 0.33f;
	struct ParticleSprite {
		DirectX::XMFLOAT3 position{ };   // 월드 위치(m) — 사각형의 기준점(2026-11-25 플립북 pivot 이 놓이는 곳, 기본 = 사각형 가운데)
		float             size = 0.0f;   // 사각형 한 변(m)
		DirectX::XMFLOAT4 color{ };      // 선형 RGB(1 넘으면 블룸으로 번진다) · a = 불투명도. 플립북이면 텍스처 색에 곱한다
		float             rotation = 0.0f;   // 화면 안 회전(라디안) — 카메라를 보는 사각형만
		uint32_t          flags = 0;     // ParticleFlags
		// 2026-11-23 플립북 — 예전 pad 두 칸. flipbook = 0 이면 텍스처 없음(둥근 모양 계산),
		//  n 이면 FramePacket::particleFlipbooks[n − 1]. frame = 프레임 위치(0 ~ 프레임 수 − 1, 소수 = 다음 프레임과 섞는 몫).
		uint32_t          flipbook = 0;
		float             frame = 0.0f;
		// 2026-11-25 — 고정 사각형 축(kParticleFixedAxes 일 때만, 월드 단위 벡터): X = 사각형 오른쪽 · Y = 위 · Z = 보는 쪽(노멀 텍스처의 Z).
		//  offsetX · offsetY = 기준점 → 사각형 가운데(한 변 단위, 오른쪽 · 위가 +). 카메라를 보는 사각형이면 카메라 오른쪽 · 위 쪽으로.
		DirectX::XMFLOAT3 axisX{ };
		float             offsetX = 0.0f;
		DirectX::XMFLOAT3 axisY{ };
		float             offsetY = 0.0f;
		DirectX::XMFLOAT3 axisZ{ };
		float             rayCoverage = kParticleRayCoverageRound;   // 레이가 받는 불투명도 = color.a × 이 값
	};
	static_assert(sizeof(ParticleSprite) == 96, "ParticleSprite 는 96바이트 (ParticleCommon.hlsli 와 같아야 한다)");

	// ── 파티클 플립북 한 줄 (2026-11-23 사용자 «플립북 클래스도 하나 구성하고 리소스 매니저에서 읽어 넘겨주면 플립북으로 다룰 수 있도록») ──
	//  Flipbook(Client/Effects)이 만든 GPU 텍스처 · 칸 배치. ParticleSystem::Extract 가 패킷에 채우고,
	//  렌더러가 텍스처 번호를 바인드리스 번호로 바꿔 [7b] 파티클 상수에 싣는다(Particles.hlsl gFlipbooks).
	//  칸 = 열 × 행, 프레임은 왼쪽 위부터 오른쪽 → 다음 줄 순서.
	//  2026-11-24 — 낱장 텍스처(ParticleTexture, 사각형이 도는 것)도 1 × 1 칸 · 1 프레임 한 줄로 같은 표에 들어간다.
	struct ParticleFlipbook {
		TextureHandle color = kInvalidTexture;    // 바탕색(sRGB) + 알파 — 빛은 굽지 않는다(게임이 비춘다)
		TextureHandle normal = kInvalidTexture;   // 카메라 공간 노멀(선형, X 오른쪽 · Y 위 · Z 카메라 쪽) + 알파. 없으면 노멀 없이 비춘다
		uint32_t      columns = 1;
		uint32_t      rows = 1;
		uint32_t      frameCount = 1;
	};
	//  2026-11-24 — 층마다 여러 장(사용자 «먼지 전부, 연기 전부» — 먼지 16 · 연기 53 + 피)이라 8 → 128.
	inline constexpr uint32_t kMaxParticleFlipbooks = 128;   // 한 프레임에 쓸 수 있는 표 줄 수(Particles.hlsl PARTICLE_MAX_FLIPBOOKS 와 같다)

	// ── 추가 래스터 리스트 (2026-11-17 render/tlas-select, 사용자 «재구성 범위 단위별로 리스트 만들어 두고 해당 리스트들만
	//    불러오는건 어때» · «리스트 하나에 최대 8192로») — 예시 행성 배치 오브젝트를 칸(링) · 큰 것 · 임시 리스트로 미리 만든다.
	//  items 는 바뀔 때만 새로 만들어지는 읽기 전용 배열(공유 — 매 프레임 복사하지 않는다), version 이 바뀌면 렌더러가 다시 묶고 올린다.
	//  한 리스트는 렌더러 패스 자원 하나(인스턴스 · 컬링 버퍼, 최대 MaxDrawInstances = 8,192)를 쓴다.
	struct InstanceList {
		std::shared_ptr<const std::vector<InstanceData>> items;
		uint64_t version = 0;
	};

	// 렌더 추출 ④ 의 결과 — 렌더러가 받는 유일한 입력 (수정방향 3.2, 멀티스레딩 명세 §6 ④)
	//  광원·이미터·데칼 배열은 해당 항목(3.12 등)을 할 때 붙는다.
	struct FramePacket {
		RenderView view;
		std::vector<InstanceData> instances;
		// 2026-11-17 — 파티클 사각형. [0, particleAlphaCount) = 알파 묶음(먼 것부터 정렬됨), 그 뒤 = 가산 묶음.
		//  메인 패스에만 채운다(배경 패스는 비운다). 메시 파편 배열은 4단계(P4)에서 붙는다.
		std::vector<ParticleSprite> particles;
		uint32_t particleAlphaCount = 0;
		// 2026-11-23 — 파티클 플립북 표(ParticleSprite::flipbook − 1 이 이 번호). kMaxParticleFlipbooks 를 넘는 몫은 렌더러가 안 쓴다.
		std::vector<ParticleFlipbook> particleFlipbooks;
		// 2026-11-17 TLAS 목록 분리 (사용자 «TLAS 따로 고르는 단계 만들어» · «아예 두 묶음으로 다르게») —
		//  rtSeparate 면 렌더러가 TLAS 를 rtInstances 로 만든다(래스터 묶음 · 반투명은 instances 그대로, 서로 독립).
		//  false 면 예전처럼 instances 로 만든다. 메인 패스에만 채운다.
		std::vector<InstanceData> rtInstances;
		bool rtSeparate = false;
		// 2026-11-17 추가 래스터 리스트 — 메인 패스에만. instances(기본 리스트)와 따로 묶고 · 컬링하고 · 그린다(TLAS 에는 안 쓴다).
		std::vector<InstanceList> instanceLists;
		// 인스턴스의 «구성» 버전(수정방향 3.2 «버전 번호»). 노드 추가·메시 변경·가시성·반전 여부가
		// 바뀔 때만 오른다. 렌더러는 이 값이 같으면 그리기 묶음을 다시 만들지 않는다.
		//  상위 32비트 = 장면 번호, 하위 32비트 = 장면 안의 버전.
		uint64_t layoutVersion = 0;
	};
}
