// ============================================================
//  UI.hlsl — 화면 위 사각형(UI 스프라이트)
//    정점 버퍼 없이 SV_VertexID 로 사각형 4개 정점을 만든다(삼각형 스트립).
//    좌표는 픽셀, (0,0) = 왼쪽 위. 회전은 사각형 중심 기준.
//
//  ★ 텍스처는 premultiplied 알파다 → 블렌드 ONE / INV_SRC_ALPHA (GRenderer 의 UI PSO).
//  ★ SpriteCB 는 루트 상수 16개다. GRenderer.cpp 의 SpriteConstants 와 순서가 같아야 한다.
// ============================================================

cbuffer SpriteCB : register(b0)
{
	float2 gCenter;        // px
	float2 gHalfSize;      // px
	float2 gUvMin;
	float2 gUvMax;
	float4 gTint;          // 직선 알파. 셰이더에서 미리 곱한다
	float  gRotation;      // 라디안, 화면에서 시계 방향
	float2 gInvViewport;   // 1 / 창 크기
	float  _pad0;
};

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VOut
{
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

VOut VSMain(uint id : SV_VertexID)
{
	// 0:(0,0) 1:(1,0) 2:(0,1) 3:(1,1) — 스트립 순서
	const float2 corner = float2(id & 1, id >> 1);
	const float2 local = (corner * 2.0f - 1.0f) * gHalfSize;

	// y 가 아래로 자라는 화면 좌표라 이 식이 시계 방향 회전이다.
	float s, c;
	sincos(gRotation, s, c);
	const float2 rotated = float2(local.x * c - local.y * s, local.x * s + local.y * c);
	const float2 pixel = gCenter + rotated;

	VOut o;
	o.pos = float4(pixel.x * gInvViewport.x * 2.0f - 1.0f,
	               1.0f - pixel.y * gInvViewport.y * 2.0f,
	               0.0f, 1.0f);
	o.uv = lerp(gUvMin, gUvMax, corner);
	return o;
}

float4 PSMain(VOut i) : SV_Target
{
	const float4 texel = gTexture.Sample(gSampler, i.uv);
	return texel * float4(gTint.rgb * gTint.a, gTint.a);
}
