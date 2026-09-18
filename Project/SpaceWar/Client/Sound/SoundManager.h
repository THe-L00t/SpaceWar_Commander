#pragma once

// ============================================================
//  Client/Sound/SoundManager.h — 아키텍처 명세서 15절
//
//  사운드 리소스의 로드와 재생을 담당한다. 명세의 Sound Loader 와 Sound Player 는
//  이 클래스가 품는다. 내부적으로 FMOD 를 쓸 예정이며, FMOD 사용은 여기서 감싼다.
//
//  ★ 다른 시스템이 FMOD 를 직접 만지지 않는다 (명세 18절 원칙 1과 같은 이유).
//  ★ 클라이언트에만 있다. 서버는 소리를 내지 않는다.
//  ★ 지금은 선언만 있다. FMOD 를 아직 붙이지 않았다.
// ============================================================

namespace swc {

	class SoundManager
	{
	public:
		SoundManager();
		~SoundManager();
	};

} // namespace swc
