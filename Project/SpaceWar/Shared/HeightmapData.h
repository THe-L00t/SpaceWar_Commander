#pragma once
#include <cstdint>
#include <vector>

// ============================================================
//  Shared/HeightmapData.h — 하이트맵 데이터 형식
//
//  ★ 왜 Shared 인가
//    서버가 권위를 가지므로 서버도 같은 지형 높이를 알아야 위치 검증이 맞는다.
//    다만 지금 서버는 스텁이므로, 나중에 반드시 공유될 "데이터 형식"만 여기 둔다.
//
//  ★ Sample() 이 float 만 받는 이유
//    Vec3d 를 받게 하면 Vec3d 도 Shared 로 끌려오고 Planet 까지 따라온다.
//    의존성 사슬을 여기서 끊는다.
// ============================================================

namespace Shared {

	struct HeightmapData
	{
		std::vector<uint16_t> samples;
		uint32_t size = 0;        // 정사각형 한 변
		float    mean = 0.0f;     // 정규화 평균 [0,1]. 0 중심화용 (로드 시 1회 계산)

		bool Valid() const { return size > 0 && samples.size() == size_t(size) * size; }

		// u,v in [0,1]. 범위 밖 입력은 내부에서 clamp (방어).
		// 범위 판단은 호출자(TerrainSampler) 책임이다.
		float Sample(float u, float v) const
		{
			if (size == 0) return 0.0f;

			const float maxIdx = float(size - 1);
			float x = u * maxIdx;
			float y = v * maxIdx;
			x = x < 0.0f ? 0.0f : (x > maxIdx ? maxIdx : x);
			y = y < 0.0f ? 0.0f : (y > maxIdx ? maxIdx : y);

			const uint32_t x0 = uint32_t(x);
			const uint32_t y0 = uint32_t(y);
			const uint32_t x1 = (x0 + 1 < size) ? x0 + 1 : x0;
			const uint32_t y1 = (y0 + 1 < size) ? y0 + 1 : y0;
			const float fx = x - float(x0);
			const float fy = y - float(y0);

			constexpr float inv = 1.0f / 65535.0f;
			const float h00 = samples[size_t(y0) * size + x0] * inv;
			const float h10 = samples[size_t(y0) * size + x1] * inv;
			const float h01 = samples[size_t(y1) * size + x0] * inv;
			const float h11 = samples[size_t(y1) * size + x1] * inv;

			return (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fy)
				 + (h01 * (1.0f - fx) + h11 * fx) * fy;
		}
	};
}
