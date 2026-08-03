#pragma once
#include <cstdint>
#include "Shared/Math/Vec3d.h"
#include "Shared/Protocol.h"   // MoveButton 비트 정의 (전송 형식과 같은 원본을 쓴다)

// ============================================================
//  MoveInput / MotionState — 이동 계산의 입력과 상태
//
//  ★ 왜 Input / Camera 를 안 받고 이 구조체를 받나
//    Input 은 <windows.h> 와 가상키코드에, Camera 는 <DirectXMath.h> 와
//    뷰 행렬에 묶여 있다. 서버에는 키보드도 화면도 없다.
//    실제로 이동 계산이 그 둘에서 읽는 값은 아래 다섯 개뿐이라,
//    구조체 하나로 뽑아내면 나머지 로직 전체가 그대로 서버로 간다.
// ============================================================

namespace Shared {

	// 버튼 비트(Btn_Jump / Btn_Sprint / Btn_Aim)는 Protocol.h 에 있다.
	// 전송 형식과 시뮬레이션이 같은 정의를 써야 해석이 어긋나지 않는다.

	// 한 틱분 입력. 클라와 서버가 똑같이 해석한다.
	struct MoveInput
	{
		float   moveX = 0.0f;          // -1 ~ +1  (A / D)
		float   moveZ = 0.0f;          // -1 ~ +1  (S / W)
		uint8_t buttons = 0;
		Vec3d   aimDir{ 0.0, 0.0, 1.0 };  // 카메라 전방 (접평면 단위벡터)

		bool Jump()   const { return (buttons & Btn_Jump) != 0; }
		bool Sprint() const { return (buttons & Btn_Sprint) != 0; }
		bool Aiming() const { return (buttons & Btn_Aim) != 0; }
	};

	// 이동 시뮬레이션 상태. 클라의 예측 상태와 서버의 권위 상태가 같은 타입이다.
	//
	// ★ 상태는 "지형 위 고도"가 아니라 "기준구 위 고도"다.
	//   지형 위 고도로 잡으면 점프 중에도 지형을 따라가서
	//   포물선이 아니라 언덕에 들러붙는 점프가 된다.
	//
	// ★ 각도(yaw) 대신 방향 벡터를 들고 다닌다.
	//   구면에서 각도로 방향을 표현하려면 기준 방향이 필요한데
	//   어떤 기준을 잡아도 극점에서 무너진다.
	struct MotionState
	{
		Vec3d  position{ 0.0, 0.0, 0.0 };
		Vec3d  velocity{ 0.0, 0.0, 0.0 };   // 접평면 속도 (항상 up 과 수직)
		Vec3d  facing{ 0.0, 0.0, 1.0 };     // 몸이 향한 방향 (접평면 단위벡터)
		Vec3d  up{ 0.0, 1.0, 0.0 };

		double altitude = 0.0;              // 기준구 위 고도
		double verticalSpeed = 0.0;

		bool   grounded = true;
		bool   sprinting = false;
		float  speed = 0.0f;
	};
}
