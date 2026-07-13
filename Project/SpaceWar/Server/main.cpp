// ============================================================
//  SpaceWar Server — IOCP 기반 대규모(50v50) 서버 진입점
//  - DirectX 없음. 서버는 그래픽을 모른다.
//  - Shared 의 프로토콜을 그대로 사용한다.
// ============================================================
#include <cstdio>
#include "Shared/Protocol.h"   // AdditionalIncludeDirectories = $(ProjectDir)..\ 덕분에 이렇게 참조

int main() {
    std::printf("SpaceWar Server 시작\n");

    // TODO: IOCP 초기화(CreateIoCompletionPort) → 리슨 소켓 → 워커 스레드 풀 → AOI 브로드캐스트
    Shared::PacketHeader hello{};
    hello.type = Shared::PacketType::PlayerMove;
    std::printf("프로토콜 링크 확인: PacketType=%u\n",
                static_cast<unsigned>(hello.type));
    return 0;
}
