// ============================================================
//  SpaceWar Client — DirectX 12 / DXR 렌더 + 게임플레이 진입점
//  - DirectX 코드는 렌더러(GRenderer) 안에만 둔다. (렌더러 경계 명세 참고)
//  - 게임플레이는 핸들·Scene 만 다루고 DX 는 모른다.
//  - Shared 의 프로토콜을 그대로 사용한다.
// ============================================================
#include <windows.h>
#include "Shared/Protocol.h"   // AdditionalIncludeDirectories = $(ProjectDir)..\ 덕분에 이렇게 참조

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE, PWSTR, int /*nCmdShow*/) {
    // TODO: 윈도우 생성 → GRenderer 초기화(DX12/DXR) → Scene 구성 → 메인 루프
    Shared::PacketHeader hello{};
    hello.type = Shared::PacketType::PlayerFire;
    OutputDebugStringW(L"SpaceWar Client 시작 (프로토콜 링크 확인 OK)\n");
    return 0;
}
