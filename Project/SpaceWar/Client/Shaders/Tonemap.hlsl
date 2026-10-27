// ============================================================
//  Tonemap.hlsl — [8] HDR 컬러 → 백버퍼 (2026-10-27 파이프라인 1단계)
//
//  화면 전체 삼각형 하나로 HDR 컬러를 백버퍼(R8G8B8A8_UNORM)에 옮긴다.
//  ★ 지금은 톤 곡선 · 노출 · 감마 없이 0~1 로 자르기만 한다.
//    예전 forward 는 UNORM 백버퍼에 바로 써서 하드웨어가 0~1 로 잘랐다 — 그 화면과 같게 두는 것이다(회귀 확인).
//    톤 곡선 · 자동 노출 · 블룸은 구조도 [8] 의 미정 항목이다. 정해지면 이 PSMain 에 들어온다.
// ============================================================

Texture2D<float4> gHdr : register(t0);

struct VOut
{
	float4 pos : SV_Position;
};

// 정점 버퍼 없이 SV_VertexID 0·1·2 로 화면을 덮는 큰 삼각형 하나를 만든다.
VOut VSMain(uint id : SV_VertexID)
{
	const float2 uv = float2((id << 1) & 2, id & 2);   // (0,0) (2,0) (0,2)
	VOut o;
	o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	return o;
}

float4 PSMain(VOut i) : SV_Target
{
	// 같은 해상도라 픽셀 좌표로 바로 읽는다(샘플러 없음).
	const float3 c = gHdr.Load(int3(int2(i.pos.xy), 0)).rgb;
	return float4(saturate(c), 1.0);
}
