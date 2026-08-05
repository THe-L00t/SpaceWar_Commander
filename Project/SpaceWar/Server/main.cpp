// ============================================================
//  IOCP 좌표 서버 — 1:1 통신 단계
//
//  클라이언트가 렌더 루프에서 1/30초마다 좌표를 보내면
//  서버가 받아서 콘솔에 찍고, 보낸 당사자에게만 되돌려준다(에코).
//
//  ★ 브로드캐스트(SendMessageAll)를 뺀 이유
//    지금은 1:1 통신을 확인하는 단계다. 전원에게 뿌리면
//    "내 좌표가 제대로 갔는가" 와 "남의 좌표가 오는가" 가 섞여
//    무엇이 잘못됐는지 구분이 안 된다.
//
//  ★ 교재 채팅서버에서 바뀐 세 가지
//    1) 세션을 class + std::shared_ptr 로 관리     -> 수명 안전
//    2) 동기 send() 를 WSASend() 비동기로          -> 스레드가 안 막힌다
//    3) 브로드캐스트 제거, 보낸 사람에게만 응답    -> 1:1
// ============================================================
#include <winsock2.h>
#pragma comment(lib, "ws2_32")
#include <windows.h>
#include <memory>
#include <unordered_map>
#include <cstdio>
#include "Shared/Protocol.h"

/////////////////////////////////////////////////////////////////////////
//클라이언트 처리를 위한 작업자 스레드 개수.
#define MAX_THREAD_CNT		4
#define SERVER_PORT			25000
#define RECV_BUFFER_SIZE	8192		//8KB

/////////////////////////////////////////////////////////////////////////
//  IO 종류
//
//  ★ 왜 필요한가
//    GetQueuedCompletionStatus() 는 "무슨 작업이 끝났는지" 를 알려주지 않는다.
//    OVERLAPPED 주소만 돌려준다. 그래서 OVERLAPPED 를 구조체로 감싸고
//    우리가 직접 종류를 표시해 둔다.
enum class IO_TYPE { RECV, SEND };

/////////////////////////////////////////////////////////////////////////
//  IO 한 건의 문맥
//
//  ★ wol 이 반드시 첫 멤버여야 한다
//    커널은 우리가 넘긴 OVERLAPPED* 를 그대로 돌려준다.
//    첫 멤버라야 그 주소가 곧 IO_CONTEXT* 라서 캐스팅 한 번에 되찾는다.
//
//  ★ SEND 는 왜 매번 new 하는가
//    보낼 데이터는 커널이 다 보낼 때까지 살아 있어야 한다.
//    지역 변수에 담아 WSASend 를 걸고 함수를 빠져나가면
//    커널이 이미 사라진 메모리를 읽는다.
//    그래서 힙에 잡아두고, 완료 통보를 받았을 때 delete 한다.
struct IO_CONTEXT
{
	WSAOVERLAPPED	wol;		//★ 반드시 첫 멤버
	IO_TYPE			type;

	WSABUF			wsaBuf;
	char			*pData;		//SEND 전용: 보낼 데이터 사본
	int				nLen;		//보낼 총 바이트
	int				nSent;		//지금까지 보낸 바이트 (부분 송신 대응)
	SOCKET			hSocket;	//SEND 전용: 이어서 보낼 대상

	IO_CONTEXT(IO_TYPE t)
		: type(t), pData(NULL), nLen(0), nSent(0), hSocket(INVALID_SOCKET)
	{
		::ZeroMemory(&wol, sizeof(wol));
		::ZeroMemory(&wsaBuf, sizeof(wsaBuf));
	}

	~IO_CONTEXT()
	{
		delete[] pData;
	}
};

/////////////////////////////////////////////////////////////////////////
//  Session — 접속자 한 명
//
//  ★ 왜 class + shared_ptr 인가
//    교재는 USERSESSION* 를 CompletionKey 로 넘기고, 연결이 끊기면
//    작업자 스레드가 그 자리에서 delete 한다.
//    그런데 작업자 스레드는 4개다. 한 스레드가 delete 하는 사이
//    다른 스레드가 같은 포인터로 그 세션을 만지면 이미 사라진 메모리를 읽는다.
//    (= Dangling Pointer. 재현이 거의 안 되는 크래시)
//
//    shared_ptr 로 들고 있으면 "마지막으로 쓰는 사람이 나갈 때" 해제된다.
//    누가 먼저 지우든 안전하다.
class Session
{
public:
	Session(SOCKET hSocket, UINT32 nPlayerId)
		: m_hSocket(hSocket), m_nPlayerId(nPlayerId), m_nRecvd(0),
		  m_recvCtx(IO_TYPE::RECV)
	{
		::ZeroMemory(m_buffer, sizeof(m_buffer));
	}

	~Session()
	{
		//여기 도달했다 = 이 세션을 참조하는 곳이 하나도 없다.
		//그때 비로소 소켓을 닫는다.
		if (m_hSocket != INVALID_SOCKET)
		{
			::shutdown(m_hSocket, SD_BOTH);
			::closesocket(m_hSocket);
		}
	}

	SOCKET	Sock()		const { return m_hSocket; }
	UINT32	PlayerId()	const { return m_nPlayerId; }

	/////////////////////////////////////////////////////////////////////
	//다음 수신을 IOCP 에 예약한다.
	//
	//  ★ 이미 받아둔 조각(m_nRecvd) 뒤에 이어서 받아야 한다.
	//    항상 버퍼 앞부터 받으면 앞서 받은 조각을 덮어쓴다.
	//
	//  ★ 반환값을 반드시 확인해야 한다
	//    예약이 즉시 실패하면(이미 끊긴 소켓 등) 완료 통보가 오지 않는다.
	//    실패를 무시하면 그 세션은 목록에 영원히 남는다.
	//    (실제로 강제 절단 시험에서 세션 30개가 그대로 남는 것을 확인했다)
	bool PostRecv()
	{
		DWORD	dwReceiveSize	= 0;
		DWORD	dwFlag			= 0;

		::ZeroMemory(&m_recvCtx.wol, sizeof(m_recvCtx.wol));
		m_recvCtx.wsaBuf.buf = m_buffer + m_nRecvd;
		m_recvCtx.wsaBuf.len = sizeof(m_buffer) - m_nRecvd;

		//버퍼가 꽉 찼는데 해석이 안 된다 = 규격에 없는 데이터를 계속 흘려보내는 것
		if (m_recvCtx.wsaBuf.len == 0)
			return false;

		int nResult = ::WSARecv(
			m_hSocket,			//클라이언트 소켓 핸들
			&m_recvCtx.wsaBuf,	//WSABUF 구조체 배열의 주소
			1,					//배열 요소의 개수
			&dwReceiveSize,
			&dwFlag,
			&m_recvCtx.wol,
			NULL);

		//SOCKET_ERROR 일 때만 오류를 확인한다.
		//(성공했을 때 WSAGetLastError() 를 보면 이전 호출의 값이 남아 있어 오진한다)
		if (nResult == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
			return false;

		return true;
	}

	/////////////////////////////////////////////////////////////////////
	//수신 완료. 받은 만큼 쌓고 완전한 패킷만 골라 처리한다.
	//다음 수신 예약까지 성공해야 true 다.
	bool OnRecv(DWORD dwTransferredSize)
	{
		m_nRecvd += (int)dwTransferredSize;
		ProcessPackets();
		return PostRecv();
	}

	/////////////////////////////////////////////////////////////////////
	//  비동기 송신
	//
	//  ★ 왜 동기 send() 를 쓰면 안 되는가
	//    send() 는 커널 송신 버퍼가 가득 차면 자리가 날 때까지 그 자리에서 멈춘다.
	//    작업자 스레드는 4개뿐이다. 느린 클라 하나가 스레드를 붙잡으면
	//    그동안 다른 클라들의 패킷 처리가 전부 밀린다.
	//    WSASend 는 "보내달라"고 예약만 하고 즉시 돌아온다.
	void Send(const void *pData, int nLen)
	{
		if (nLen <= 0) return;

		//★ 완료 통보를 받을 때까지 살아 있어야 하므로 힙에 잡는다.
		IO_CONTEXT *pCtx = new IO_CONTEXT(IO_TYPE::SEND);
		pCtx->pData		= new char[nLen];
		pCtx->nLen		= nLen;
		pCtx->hSocket	= m_hSocket;
		::memcpy(pCtx->pData, pData, nLen);

		pCtx->wsaBuf.buf = pCtx->pData;
		pCtx->wsaBuf.len = nLen;

		DWORD dwSent = 0;
		int nResult = ::WSASend(m_hSocket, &pCtx->wsaBuf, 1, &dwSent, 0, &pCtx->wol, NULL);

		if (nResult == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
		{
			//예약 자체가 실패했다 = 완료 통보가 오지 않는다.
			//여기서 직접 해제하지 않으면 그대로 누수된다.
			delete pCtx;
		}
	}

private:
	/////////////////////////////////////////////////////////////////////
	//받은 바이트에서 완전한 좌표 패킷을 꺼내 처리한다.
	//
	//  ★ 왜 이런 처리가 필요한가
	//    TCP 는 바이트 스트림이라 패킷 경계가 없다.
	//    32바이트 패킷 하나가 20+12 로 쪼개져 올 수도 있고,
	//    두 개가 64바이트로 붙어서 한 번에 올 수도 있다.
	//    채팅은 글자가 나뉘어도 이어붙으면 그만이지만,
	//    좌표는 중간에서 잘리면 숫자가 통째로 깨진다.
	void ProcessPackets()
	{
		const int nPacketSize = (int)sizeof(Shared::PlayerMovePacket);
		int nOffset = 0;

		while (m_nRecvd - nOffset >= nPacketSize)
		{
			Shared::PlayerMovePacket *pMove =
				(Shared::PlayerMovePacket *)(m_buffer + nOffset);

			if (pMove->header.type == Shared::PacketType::PlayerMove)
			{
				//서버가 하는 일은 이게 전부다 — 좌표를 콘솔에 찍는다.
				printf("[좌표] 플레이어 %u : ( %8.2f, %8.2f, %8.2f )\n",
					m_nPlayerId,
					pMove->pos[0], pMove->pos[1], pMove->pos[2]);

				//★ 1:1 — 보낸 당사자에게만 되돌려준다.
				//  (교재의 SendMessageAll 자리. 전원 브로드캐스트는 하지 않는다)
				Shared::PlayerMovePacket echo = *pMove;
				echo.playerId = m_nPlayerId;		//서버가 붙인 번호로 바꿔서 돌려준다
				Send(&echo, nPacketSize);
			}

			nOffset += nPacketSize;
		}

		//처리하고 남은 자투리를 버퍼 앞으로 당겨둔다. 다음 수신 때 이어붙는다.
		m_nRecvd -= nOffset;
		if (m_nRecvd > 0 && nOffset > 0)
			::memmove(m_buffer, m_buffer + nOffset, m_nRecvd);
	}

	SOCKET		m_hSocket;
	UINT32		m_nPlayerId;

	char		m_buffer[RECV_BUFFER_SIZE];
	int			m_nRecvd;			//buffer 에 쌓인 바이트 수

	//RECV 는 세션당 하나만 걸어둔다.
	//(여러 개 걸면 완료 순서가 뒤바뀌어 스트림이 섞인다)
	IO_CONTEXT	m_recvCtx;
};

typedef std::shared_ptr<Session> SessionPtr;

/////////////////////////////////////////////////////////////////////////
CRITICAL_SECTION					g_cs;			//스레드 동기화 객체
std::unordered_map<UINT32, SessionPtr>	g_sessions;	//플레이어번호 -> 세션
SOCKET	g_hSocket;									//서버의 리슨 소켓
HANDLE	g_hIocp;									//IOCP 핸들
LONG	g_nNextPlayerId = 0;						//플레이어 번호 발급기

/////////////////////////////////////////////////////////////////////////
//  CompletionKey 로 세션을 찾는다.
//
//  ★ 왜 포인터를 직접 넘기지 않고 번호로 찾는가
//    CompletionKey 로 Session* 를 넘기면, 그 세션이 이미 지워진 뒤에
//    늦게 도착한 완료 통보가 사라진 메모리를 가리키게 된다.
//    번호로 찾으면 없는 세션은 그냥 못 찾고 끝난다.
//    찾은 shared_ptr 은 복사본이므로, 그 사이 다른 스레드가 목록에서
//    지워도 내가 다 쓸 때까지는 살아 있다.
SessionPtr FindSession(UINT32 nPlayerId)
{
	SessionPtr p;

	::EnterCriticalSection(&g_cs);
	std::unordered_map<UINT32, SessionPtr>::iterator it = g_sessions.find(nPlayerId);
	if (it != g_sessions.end())
		p = it->second;
	::LeaveCriticalSection(&g_cs);

	return p;
}

/////////////////////////////////////////////////////////////////////////
void RemoveSession(UINT32 nPlayerId)
{
	size_t nRemain = 0;

	::EnterCriticalSection(&g_cs);
	g_sessions.erase(nPlayerId);		//shared_ptr 이 풀리며 소멸자가 소켓을 닫는다
	nRemain = g_sessions.size();
	::LeaveCriticalSection(&g_cs);

	//남은 세션 수를 같이 찍는다. 계속 늘어나면 어딘가에서 정리를 빠뜨린 것이다.
	printf("\t(남은 접속 %zu)\n", nRemain);
}

/////////////////////////////////////////////////////////////////////////
//연결된 모든 클라이언트를 닫는다.
void CloseAll()
{
	::EnterCriticalSection(&g_cs);
	g_sessions.clear();		//shared_ptr 이 풀리면서 각 세션 소멸자가 소켓을 닫는다
	::LeaveCriticalSection(&g_cs);
}

/////////////////////////////////////////////////////////////////////////
void ReleaseServer(void)
{
	//클라이언트 연결을 모두 종료한다.
	CloseAll();
	::Sleep(500);

	//Listen 소켓을 닫는다.
	if (g_hSocket != NULL)
	{
		::shutdown(g_hSocket, SD_BOTH);
		::closesocket(g_hSocket);
		g_hSocket = NULL;
	}

	//IOCP 핸들을 닫는다. 이렇게 하면 GQCS() 함수가 FALSE를 반환하며
	//::GetLastError() 함수가 ERROR_ABANDONED_WAIT_0을 반환한다.
	//IOCP 스레드들이 모두 종료된다.
	if (g_hIocp != NULL)
	{
		::CloseHandle(g_hIocp);
		g_hIocp = NULL;
	}

	//IOCP 스레드들이 종료되기를 일정시간 동안 기다린다.
	::Sleep(500);
	::DeleteCriticalSection(&g_cs);
}

/////////////////////////////////////////////////////////////////////////
//Ctrl+C 이벤트를 감지하고 프로그램을 종료한다.
BOOL CtrlHandler(DWORD dwType)
{
	if (dwType == CTRL_C_EVENT)
	{
		ReleaseServer();

		puts("*** 좌표서버를 종료합니다! ***");
		::WSACleanup();
		exit(0);
		return TRUE;
	}

	return FALSE;
}

/////////////////////////////////////////////////////////////////////////
DWORD WINAPI ThreadComplete(LPVOID pParam)
{
	DWORD			dwTransferredSize = 0;
	ULONG_PTR		ulKey = 0;
	LPOVERLAPPED	pOv = NULL;
	BOOL			bResult;

	puts("[IOCP 작업자 스레드 시작]");
	while (1)
	{
		dwTransferredSize	= 0;
		ulKey				= 0;
		pOv					= NULL;

		bResult = ::GetQueuedCompletionStatus(
			g_hIocp,				//Dequeue할 IOCP 핸들.
			&dwTransferredSize,		//주고받은 데이터 크기.
			&ulKey,					//플레이어 번호 (KEY)
			&pOv,					//OVERLAPPED 구조체.
			INFINITE);				//이벤트를 무한정 대기.

		/////////////////////////////////////////////////////////////////
		//완료 큐에서 아무것도 꺼내지 못한 경우.
		//IOCP 핸들이 닫힌 경우(서버 종료)도 여기에 해당된다.
		if (pOv == NULL)
		{
			puts("\tGQCS: IOCP 핸들이 닫혔습니다.");
			break;
		}

		//★ OVERLAPPED* 를 IO_CONTEXT* 로 되돌린다. (wol 이 첫 멤버라 주소가 같다)
		IO_CONTEXT *pCtx = (IO_CONTEXT *)pOv;

		/////////////////////////////////////////////////////////////////
		//1. 송신 완료 — 여기서 해제한다.
		if (pCtx->type == IO_TYPE::SEND)
		{
			//부분 송신: 요청한 만큼 다 나가지 않을 수 있다.
			//남은 부분을 이어서 보내지 않으면 그 뒤 스트림이 통째로 밀린다.
			if (bResult == TRUE && (int)dwTransferredSize < pCtx->nLen - pCtx->nSent)
			{
				pCtx->nSent += (int)dwTransferredSize;
				::ZeroMemory(&pCtx->wol, sizeof(pCtx->wol));
				pCtx->wsaBuf.buf = pCtx->pData + pCtx->nSent;
				pCtx->wsaBuf.len = pCtx->nLen - pCtx->nSent;

				DWORD dwSent = 0;
				int nResult = ::WSASend(pCtx->hSocket, &pCtx->wsaBuf, 1,
									&dwSent, 0, &pCtx->wol, NULL);
				if (!(nResult == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING))
					continue;		//아직 진행 중이므로 살려둔다
			}

			delete pCtx;			//★ 스레드를 막지 않고 여기서 안전하게 해제
			continue;
		}

		/////////////////////////////////////////////////////////////////
		//2. 수신 완료
		SessionPtr pSession = FindSession((UINT32)ulKey);
		if (!pSession)
			continue;				//이미 정리된 세션. 늦게 온 통보는 버린다.

		//2-1. 클라이언트가 소켓을 정상적으로 닫고 연결을 끊은 경우.
		if (dwTransferredSize == 0)
		{
			printf("\t플레이어 %u 퇴장 (정상 종료)\n", pSession->PlayerId());
			RemoveSession((UINT32)ulKey);
			continue;
		}

		//2-2. 클라이언트가 비정상적으로 종료됐거나 서버가 먼저 끊은 경우.
		if (bResult == FALSE)
		{
			printf("\t플레이어 %u 퇴장 (비정상 종료)\n", pSession->PlayerId());
			RemoveSession((UINT32)ulKey);
			continue;
		}

		//2-3. 좌표를 수신한 경우.
		//  다음 수신 예약이 실패하면 완료 통보가 다시는 오지 않으므로
		//  여기서 정리해야 한다. (안 하면 세션이 목록에 영원히 남는다)
		if (!pSession->OnRecv(dwTransferredSize))
		{
			printf("\t플레이어 %u 퇴장 (수신 예약 실패)\n", pSession->PlayerId());
			RemoveSession((UINT32)ulKey);
		}
	}

	puts("[IOCP 작업자 스레드 종료]");
	return 0;
}

/////////////////////////////////////////////////////////////////////////
DWORD WINAPI ThreadAcceptLoop(LPVOID pParam)
{
	int			nAddrSize = sizeof(SOCKADDR);
	SOCKADDR	ClientAddr;
	SOCKET		hClient;

	while ((hClient = ::accept(g_hSocket,
					&ClientAddr, &nAddrSize)) != INVALID_SOCKET)
	{
		const UINT32 nPlayerId = (UINT32)::InterlockedIncrement(&g_nNextPlayerId);

		//새 클라이언트에 대한 세션 객체 생성
		SessionPtr pNewUser = std::make_shared<Session>(hClient, nPlayerId);

		::EnterCriticalSection(&g_cs);
		g_sessions[nPlayerId] = pNewUser;
		::LeaveCriticalSection(&g_cs);

		printf("새 플레이어가 연결됐습니다. (플레이어 %u)\n", nPlayerId);

		//(연결된) 클라이언트 소켓 핸들을 IOCP에 연결.
		//★ CompletionKey 로 플레이어 번호를 넘긴다. 포인터가 아니다.
		::CreateIoCompletionPort((HANDLE)hClient, g_hIocp,
			(ULONG_PTR)nPlayerId,		//KEY!!!
			0);

		//클라이언트가 보낸 좌표를 비동기 수신한다.
		//첫 예약부터 실패하면(붙자마자 끊긴 경우) 여기서 정리한다.
		if (!pNewUser->PostRecv())
		{
			printf("\t플레이어 %u 퇴장 (첫 수신 예약 실패)\n", nPlayerId);
			RemoveSession(nPlayerId);
		}
	}

	return 0;
}

/////////////////////////////////////////////////////////////////////////
int main()
{
	//콘솔에 바로 찍히게 한다. 로그 파일로 리다이렉트하면 4KB 씩 모았다가
	//내보내기 때문에, 강제 종료하면 찍힌 좌표가 통째로 사라진다.
	//(MSVC 는 _IOLBF(줄 버퍼링)를 지원하지 않으므로 _IONBF 를 써야 한다)
	::setvbuf(stdout, NULL, _IONBF, 0);

	//윈속 초기화
	WSADATA wsa = { 0 };
	if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		puts("ERROR: 윈속을 초기화 할 수 없습니다.");
		return 0;
	}

	//임계영역객체를 생성한다.
	::InitializeCriticalSection(&g_cs);

	//Ctrl+C 키를 눌렀을 때 이를 감지하고 처리할 함수를 등록한다.
	if (::SetConsoleCtrlHandler(
			(PHANDLER_ROUTINE)CtrlHandler, TRUE) == FALSE)
		puts("ERROR: Ctrl+C 처리기를 등록할 수 없습니다.");

	//IOCP 생성
	g_hIocp = ::CreateIoCompletionPort(
		INVALID_HANDLE_VALUE,	//연결된 파일 없음.
		NULL,			//기존 핸들 없음.
		0,				//식별자(Key) 해당되지 않음.
		0);				//스레드 개수는 OS에 맡김.
	if (g_hIocp == NULL)
	{
		puts("ERROR: IOCP를 생성할 수 없습니다.");
		return 0;
	}

	//IOCP 스레드들 생성
	HANDLE hThread;
	DWORD dwThreadID;
	for (int i = 0; i < MAX_THREAD_CNT; ++i)
	{
		dwThreadID = 0;
		//클라이언트로부터 좌표를 수신함.
		hThread = ::CreateThread(NULL,	//보안속성 상속
			0,				//스택 메모리는 기본크기(1MB)
			ThreadComplete,	//스레드로 실행할 함수이름
			(LPVOID)NULL,	//
			0,				//생성 플래그는 기본값 사용
			&dwThreadID);	//생성된 스레드ID가 저장될 변수주소

		::CloseHandle(hThread);
	}

	//서버 리슨 소켓 생성
	g_hSocket = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,
					NULL, 0, WSA_FLAG_OVERLAPPED);

	//bind()/listen()
	SOCKADDR_IN addrsvr;
	addrsvr.sin_family = AF_INET;
	addrsvr.sin_addr.S_un.S_addr = ::htonl(INADDR_ANY);
	addrsvr.sin_port = ::htons(SERVER_PORT);

	if (::bind(g_hSocket,
			(SOCKADDR*)&addrsvr, sizeof(SOCKADDR_IN)) == SOCKET_ERROR)
	{
		puts("ERROR: 포트가 이미 사용중입니다.");
		ReleaseServer();
		return 0;
	}

	if (::listen(g_hSocket, SOMAXCONN) == SOCKET_ERROR)
	{
		puts("ERROR: 리슨 상태로 전환할 수 없습니다.");
		ReleaseServer();
		return 0;
	}

	//반복해서 클라이언트의 연결을 accept() 한다.
	hThread = ::CreateThread(NULL, 0, ThreadAcceptLoop,
				(LPVOID)NULL, 0, &dwThreadID);
	::CloseHandle(hThread);

	//main() 함수가 반환하지 않도록 대기한다.
	printf("*** 좌표서버를 시작합니다! (포트 %d) ***\n", SERVER_PORT);
	printf("    좌표 패킷 크기 = %d 바이트 / 수신한 당사자에게만 에코\n",
		(int)sizeof(Shared::PlayerMovePacket));
	while (1)
		getchar();

	return 0;
}
