#pragma once
#include <DirectXMath.h>
#include "Handles.h"

// ============================================================
//  Client/UISprite.h — UI 표시 데이터 (프레임 패킷의 UI 몫)
//
//  「멀티스레딩 분류 명세서」 5장 UI: «UI 표시 데이터도 프레임 패킷에 넣는다».
//  UI 가 상태를 들고(메인), 렌더 추출 때 이 POD 배열로 복사해 Renderer 에 넘긴다.
//  Renderer 는 이것만 보고 화면 위에 사각형을 그린다. 게임 쪽을 되부르지 않는다.
//
//  ★ 좌표는 픽셀이다. (0,0) = 창 왼쪽 위, +y 는 아래.
//  ★ 텍스처는 premultiplied 알파로 올린다 (LoadingScene.cpp PrepareForUI). tint 의 a 는 전체를 곱해 흐리게 한다.
// ============================================================

namespace swc {

	struct UISprite
	{
		TextureHandle       texture = kInvalidTexture;
		DirectX::XMFLOAT2   center{ 0.0f, 0.0f };        // 사각형 중심 (px)
		DirectX::XMFLOAT2   halfSize{ 0.0f, 0.0f };      // 반폭·반높이 (px)
		float               rotation = 0.0f;             // 시계 방향 라디안
		DirectX::XMFLOAT2   uvMin{ 0.0f, 0.0f };         // 텍스처에서 잘라 쓸 영역
		DirectX::XMFLOAT2   uvMax{ 1.0f, 1.0f };
		DirectX::XMFLOAT4   tint{ 1.0f, 1.0f, 1.0f, 1.0f };
	};

} // namespace swc
