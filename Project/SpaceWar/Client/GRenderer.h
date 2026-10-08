#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <DirectXMath.h>
#include "InstanceData.h"
#include "UISprite.h"
#include "Vertex.h"
#include "RayTracingParams.h"
#include "Resource/ModelData.h"

struct HWND__;
using HWND = HWND__*;

namespace swc {
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

		void BeginFrame();
		void Render(const RenderView&, const std::vector<InstanceData>&, const DirectX::XMFLOAT4X4*);
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
