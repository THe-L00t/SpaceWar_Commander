#pragma once
#include "../ModelReader.h"

// ============================================================
//  Shared/Model/Readers/GltfReader.h — glTF 2.0 (.glb / .gltf)
//
//  ★ 이 프로젝트의 운영 포맷이다 (2026-10-07)
//    메시·재질·노드 계층뿐 아니라 **스킨(조인트·웨이트·역바인드)과 애니메이션 클립**이
//    규격에 들어 있다. 명세 §14 Animator 와 §4 의 Animation 리소스를 채울 수 있는 유일한 포맷이고,
//    블렌더가 그대로 내보낸다. 외부 SDK 없이 직접 파싱한다.
//
//  읽는 것
//    .glb  — 12바이트 헤더 + JSON 청크 + BIN 청크
//    .gltf — 텍스트 JSON + 외부 .bin/이미지, 또는 data: URI(base64)
//    메시(삼각형) · 재질(PBR 5슬롯) · 노드 계층 · 스킨 1개 · 애니메이션 클립
//
//  읽지 않는 것 (모르는 확장은 무시한다)
//    Draco 압축(KHR_draco_*) · 희소 접근자(sparse) · 모프 타깃 · 카메라 · 라이트 ·
//    TRIANGLES 가 아닌 모드(STRIP·FAN·LINES·POINTS) · 두 번째 UV 세트(TEXCOORD_1 이상)
//
//  ★ 좌표 변환을 하지 않는다 — 이유는 .cpp 의 «좌표계» 주석에 적었다.
// ============================================================

namespace Shared {

	class GltfReader final : public IModelReader
	{
	public:
		bool Matches(const uint8_t* head, size_t headSize, const char* ext) const override;
		ModelFormat Format() const override { return ModelFormat::Gltf; }

		bool Read(const wchar_t* path, const ReadOptions& options,
			ModelSource& out, std::wstring& error) override;
	};

} // namespace Shared
