// ============================================================
//  DebugLines.hlsl — 디버그 선 (2026-10-29, C 키 충돌체 와이어프레임)
//    정점 = 원점 기준 위치(float3) + RGBA8 색. 선 목록(LINELIST).
//    GRenderer::DrawDebugLines 가 같은 정점으로 두 번 그린다:
//      ① 가려진 부분(깊이 GREATER) — gAlpha 를 낮춰 옅게
//      ② 보이는 부분(깊이 LESS_EQUAL) — 그대로
//    톤맵이 끝난 백버퍼 위에 그린다(UI 처럼) — 노출 · 블룸에 색이 바뀌지 않는다.
//
//  ★ 루트 상수 20개 = GRenderer.cpp DebugLineConstants 와 순서가 같아야 한다.
//  ★ gViewProj 는 «카메라 기준» 행렬이다(CameraRelativeViewProj) — (월드 − eye) 를 곱한다.
//    정점 + gOffset = 월드 − eye. gOffset = 원점 − eye 를 C++ 이 double 로 구해 넘긴다.
// ============================================================

cbuffer DebugLineCB : register(b0)
{
	float4x4 gViewProj;   // 카메라 기준 → 클립 (행 벡터 mul(v, M), 다른 셰이더와 같은 규약)
	float3   gOffset;     // 원점 − eye
	float    gAlpha;      // 알파 배율
};

struct VIn
{
	float3 pos   : POSITION;
	float4 color : COLOR;
};

struct VOut
{
	float4 pos   : SV_Position;
	float4 color : COLOR;
};

VOut VSMain(VIn i)
{
	float3 p = i.pos + gOffset;   // 카메라 기준 위치
	// 충돌체가 렌더 표면과 거의 같은 자리라 깊이에서 지지 않게 카메라 쪽으로 조금 당긴다(거리의 0.2 %, 최대 3 cm).
	const float d = length(p);
	p -= p / max(d, 1.0e-3f) * min(0.03f, d * 0.002f);

	VOut o;
	o.pos = mul(float4(p, 1.0f), gViewProj);
	o.color = float4(i.color.rgb, i.color.a * gAlpha);
	return o;
}

float4 PSMain(VOut i) : SV_Target
{
	return i.color;   // 직선 알파 — 블렌드 SRC_ALPHA / INV_SRC_ALPHA
}
