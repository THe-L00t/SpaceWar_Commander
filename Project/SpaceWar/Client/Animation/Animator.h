#pragma once

// ============================================================
//  Client/Animation/Animator.h — 아키텍처 명세서 14절
//
//  Resource Manager 에서 애니메이션 리소스를 받아 GameObject 에 적용하고,
//  렌더링에 필요한 애니메이션 데이터를 Renderer 로 넘긴다.
//  명세의 Animation Object 와 Animation Blender 는 이 클래스가 품는다.
//
//  ★ 클라이언트에만 있다. 서버는 화면을 그리지 않는다.
//    (나중에 서버가 피격 판정을 뼈 단위로 하게 되면 그때 다시 생각한다)
//  ★ 지금은 선언만 있다. 애니메이션 리소스 형식이 아직 없다.
// ============================================================

namespace swc {

	class Animator
	{
	public:
		Animator();
		~Animator();
	};

} // namespace swc
