#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <string>
#include <DirectXMath.h>
#include "InstanceData.h"
#include "UISprite.h"
#include "DebugLine.h"
#include "Vertex.h"
#include "RayTracingParams.h"
#include "Resource/ModelData.h"

struct HWND__;
using HWND = HWND__*;

namespace swc {
	// 래스터 제출량. GPU 실행 시간이나 실제 FPS 측정값은 아니다.
	//  culled·instances·triangles 는 GPU 컬링 결과를 EndFrame 에서 읽어 채운다(그 전에는 0).
	struct RenderStats
	{
		size_t submitted = 0;    // 패킷의 인스턴스 수
		size_t culled = 0;       // GPU 컬링으로 빠진 인스턴스 수
		size_t drawCalls = 0;    // ExecuteIndirect 수 = 그리기 묶음 수(같은 PSO·재질·메시)
		size_t instances = 0;    // 그려진 인스턴스 수(컬링 뒤)
		size_t dropped = 0;      // 버퍼·묶음 상한을 넘어 못 그린 수 — 0 이어야 정상
		uint64_t triangles = 0;
	};

	class GRenderer
	{
	public:
		GRenderer();
		~GRenderer();

		bool Initialize(HWND, uint32_t, uint32_t);
		MeshHandle CreateMesh(const Vertex*, size_t, const uint32_t*, size_t);
		// GPU 리소스 등록은 Initialize 이후, BeginFrame 밖에서 수행한다.
		// 업로드가 끝난 뒤에 돌아오므로, 받은 직후 RAM 사본을 버려도 된다.
		// 만든 텍스처는 재질에도, UI(DrawSprites)에도 쓸 수 있다.
		TextureHandle CreateTexture(const TextureData&);
		MaterialHandle CreateMaterial(const MaterialData&, const std::array<TextureHandle, kMaterialTextureCount>&);
		// ── 지형 레이어 재질 (2026-10-28) ──
		//  baseColor · normal · orm · height = 같은 종류 4장(마스크 R G B A 순)을 묶은 배열 텍스처(arraySize 4).
		//    baseColor = RGBA8 sRGB, normal · orm = RGBA8, height = R8. 넷의 크기 · 밉 수가 같아야 한다.
		//  layerMask = RGBA8 마스크 한 장(채널마다 레이어 비율).
		//  마스크 UV · 노이즈는 «월드 위치 − TerrainMaterialData::origin(행성 중심)» 으로 계산한다(2026-10-29 — 예전에는 메시 좌표).
		//  그래서 노드 변환이 있는 메시(오솔길 · 착륙 대지)도 이 재질로 그리면 지형과 같은 자리 · 같은 무늬가 된다.
		MaterialHandle CreateTerrainMaterial(const TerrainMaterialData&, TextureHandle baseColor, TextureHandle normal,
			TextureHandle orm, TextureHandle height, TextureHandle layerMask);
		// ── 물가 페이드 (2026-10-29) — 투과 면이 땅(면 위로 올라온 불투명 표면)과 만나는 물가에서 옅어진다(폭 distance m, 0 = 끔) ──
		//  얼어붙은 호수 면처럼 땅에 걸친 얇은 투과 면의 경계를 흐리게 한다. 프레임 밖에서 부른다. 실패하면 false.
		bool SetMaterialEdgeFade(MaterialHandle, float distance);

		void BeginFrame();
		// BeginFrame마다 배경(view.background=true), 메인 패스를 각 한 번 호출한다.
		//  프레임 패킷이 유일한 입력이다 — 렌더러는 Scene 을 직접 읽지 않는다(수정방향 3.2).
		void Render(const FramePacket&);
		// ── 디버그 선 (2026-10-29 — C 키 충돌체 와이어프레임, DebugLine.h) ──
		//  톤맵이 끝난 백버퍼 위에 그린다. 메인 패스 깊이로 가림을 판정해 보이는 부분은 그대로, 가려진 부분은 옅게.
		//  BeginFrame 과 EndFrame 사이, 메인 Render 뒤 · DrawSprites 앞에 부른다(UI 가 선 위에 온다).
		//  view = 메인 패스의 RenderView(패킷 그대로), origin = 정점 좌표의 원점(월드). 정점 두 개가 선 하나다.
		//  kMaxDebugLineVertices 를 넘는 몫은 그리지 않는다. 셰이더를 못 만든 장치에서는 아무것도 하지 않는다.
		static constexpr size_t kMaxDebugLineVertices = 2u * 1024u * 1024u;
		void DrawDebugLines(const RenderView& view, const DirectX::XMFLOAT3& origin, const std::vector<DebugLineVertex>& vertices);
		// UI 를 화면 맨 위에 덮어 그린다. BeginFrame 과 EndFrame 사이, Render 뒤에 부른다.
		void DrawSprites(const std::vector<UISprite>&);
		void EndFrame();

		uint32_t Width() const;
		uint32_t Height() const;

		// ── 하이브리드 제어 (DX 타입 노출 없음) ──
		bool SupportsRaytracing() const;
		void SetRayTracingParams(const RayTracingParams&);
		const RayTracingParams& GetRayTracingParams() const;

		void SetSunDirection(const DirectX::XMFLOAT3&);
		void SetDebugMode(uint32_t);
		uint32_t DebugMode() const;
		const RenderStats& Stats() const;

		// 초기화 실패 원인 / 어댑터 이름 등
		const std::wstring& StatusText() const;

		// ── 가속 구조 계측 (DX 타입 노출 없음) ──
		//  scratch 는 빌드 뒤 해제되므로 로드가 끝나면 0 이어야 정상이다.
		uint64_t BlasResultBytes() const;
		uint64_t BlasScratchBytes() const;
		uint64_t BlasCompactionSaved() const;
		size_t   BlasCount() const;

		// TLAS 는 매 프레임 재빌드한다. 상한을 넘겨 버려진 인스턴스가 있으면
		// «RT 에만 안 보이는 물체» 가 생기므로 숫자를 드러낸다.
		uint32_t TlasInstanceCount() const;
		uint32_t TlasMaxInstances() const;
		uint32_t TlasDroppedInstances() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
