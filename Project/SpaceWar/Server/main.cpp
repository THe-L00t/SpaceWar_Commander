#include <winsock2.h>
#pragma comment(lib, "ws2_32")
#include <windows.h>
#include <memory>
#include <unordered_map>
#include <cstdio>
#include "Shared/Protocol.h"

#define MAX_THREAD_CNT		4
#define SERVER_PORT			25000
#define RECV_BUFFER_SIZE	8192		//8KB

enum class IO_TYPE { RECV, SEND };

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
		if (m_hSocket != INVALID_SOCKET)
		{
			::shutdown(m_hSocket, SD_BOTH);
			::closesocket(m_hSocket);
		}
	}

	SOCKET	Sock()		const { return m_hSocket; }
	UINT32	PlayerId()	const { return m_nPlayerId; }

	bool PostRecv()
	{
		DWORD	dwReceiveSize	= 0;
		DWORD	dwFlag			= 0;

		::ZeroMemory(&m_recvCtx.wol, sizeof(m_recvCtx.wol));
		m_recvCtx.wsaBuf.buf = m_buffer + m_nRecvd;
		m_recvCtx.wsaBuf.len = sizeof(m_buffer) - m_nRecvd;

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

		if (nResult == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
			return false;

		return true;
	}

	bool OnRecv(DWORD dwTransferredSize)
	{
		m_nRecvd += (int)dwTransferredSize;
		ProcessPackets();
		return PostRecv();
	}

	void Send(const void *pData, int nLen)
	{
		if (nLen <= 0) return;

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
			delete pCtx;
		}
	}

private:
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
				printf("[좌표] 플레이어 %u : ( %8.2f, %8.2f, %8.2f )\n",
					m_nPlayerId,
					pMove->pos[0], pMove->pos[1], pMove->pos[2]);

				Shared::PlayerMovePacket echo = *pMove;
				echo.playerId = m_nPlayerId;		//서버가 붙인 번호로 바꿔서 돌려준다
				Send(&echo, nPacketSize);
			}

			nOffset += nPacketSize;
		}

		m_nRecvd -= nOffset;
		if (m_nRecvd > 0 && nOffset > 0)
			::memmove(m_buffer, m_buffer + nOffset, m_nRecvd);
	}

	SOCKET		m_hSocket;
	UINT32		m_nPlayerId;

	char		m_buffer[RECV_BUFFER_SIZE];
	int			m_nRecvd;			//buffer 에 쌓인 바이트 수

	IO_CONTEXT	m_recvCtx;
};

typedef std::shared_ptr<Session> SessionPtr;

CRITICAL_SECTION					g_cs;			//스레드 동기화 객체
std::unordered_map<UINT32, SessionPtr>	g_sessions;	//플레이어번호 -> 세션
SOCKET	g_hSocket;									//서버의 리슨 소켓
HANDLE	g_hIocp;									//IOCP 핸들
LONG	g_nNextPlayerId = 0;						//플레이어 번호 발급기

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

void RemoveSession(UINT32 nPlayerId)
{
	size_t nRemain = 0;

	::EnterCriticalSection(&g_cs);
	g_sessions.erase(nPlayerId);		//shared_ptr 이 풀리며 소멸자가 소켓을 닫는다
	nRemain = g_sessions.size();
	::LeaveCriticalSection(&g_cs);

	printf("\t(남은 접속 %zu)\n", nRemain);
}

void CloseAll()
{
	::EnterCriticalSection(&g_cs);
	g_sessions.clear();		//shared_ptr 이 풀리면서 각 세션 소멸자가 소켓을 닫는다
	::LeaveCriticalSection(&g_cs);
}

void ReleaseServer(void)
{
	CloseAll();
	::Sleep(500);

	if (g_hSocket != NULL)
	{
		::shutdown(g_hSocket, SD_BOTH);
		::closesocket(g_hSocket);
		g_hSocket = NULL;
	}

	if (g_hIocp != NULL)
	{
		::CloseHandle(g_hIocp);
		g_hIocp = NULL;
	}

	::Sleep(500);
	::DeleteCriticalSection(&g_cs);
}

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

		if (pOv == NULL)
		{
			puts("\tGQCS: IOCP 핸들이 닫혔습니다.");
			break;
		}

		IO_CONTEXT *pCtx = (IO_CONTEXT *)pOv;

		if (pCtx->type == IO_TYPE::SEND)
		{
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

		SessionPtr pSession = FindSession((UINT32)ulKey);
		if (!pSession)
			continue;				//이미 정리된 세션. 늦게 온 통보는 버린다.

		if (dwTransferredSize == 0)
		{
			printf("\t플레이어 %u 퇴장 (정상 종료)\n", pSession->PlayerId());
			RemoveSession((UINT32)ulKey);
			continue;
		}

		if (bResult == FALSE)
		{
			printf("\t플레이어 %u 퇴장 (비정상 종료)\n", pSession->PlayerId());
			RemoveSession((UINT32)ulKey);
			continue;
		}

		if (!pSession->OnRecv(dwTransferredSize))
		{
			printf("\t플레이어 %u 퇴장 (수신 예약 실패)\n", pSession->PlayerId());
			RemoveSession((UINT32)ulKey);
		}
	}

	puts("[IOCP 작업자 스레드 종료]");
	return 0;
}

DWORD WINAPI ThreadAcceptLoop(LPVOID pParam)
{
	int			nAddrSize = sizeof(SOCKADDR);
	SOCKADDR	ClientAddr;
	SOCKET		hClient;

	while ((hClient = ::accept(g_hSocket,
					&ClientAddr, &nAddrSize)) != INVALID_SOCKET)
	{
		const UINT32 nPlayerId = (UINT32)::InterlockedIncrement(&g_nNextPlayerId);

		SessionPtr pNewUser = std::make_shared<Session>(hClient, nPlayerId);

		::EnterCriticalSection(&g_cs);
		g_sessions[nPlayerId] = pNewUser;
		::LeaveCriticalSection(&g_cs);

		printf("새 플레이어가 연결됐습니다. (플레이어 %u)\n", nPlayerId);

		::CreateIoCompletionPort((HANDLE)hClient, g_hIocp,
			(ULONG_PTR)nPlayerId,		//KEY!!!
			0);

		if (!pNewUser->PostRecv())
		{
			printf("\t플레이어 %u 퇴장 (첫 수신 예약 실패)\n", nPlayerId);
			RemoveSession(nPlayerId);
		}
	}

	return 0;
}

int main()
{
	::setvbuf(stdout, NULL, _IONBF, 0);

	WSADATA wsa = { 0 };
	if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		puts("ERROR: 윈속을 초기화 할 수 없습니다.");
		return 0;
	}

	::InitializeCriticalSection(&g_cs);

	if (::SetConsoleCtrlHandler(
			(PHANDLER_ROUTINE)CtrlHandler, TRUE) == FALSE)
		puts("ERROR: Ctrl+C 처리기를 등록할 수 없습니다.");

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

	HANDLE hThread;
	DWORD dwThreadID;
	for (int i = 0; i < MAX_THREAD_CNT; ++i)
	{
		dwThreadID = 0;
		hThread = ::CreateThread(NULL,	//보안속성 상속
			0,				//스택 메모리는 기본크기(1MB)
			ThreadComplete,	//스레드로 실행할 함수이름
			(LPVOID)NULL,
			0,				//생성 플래그는 기본값 사용
			&dwThreadID);	//생성된 스레드ID가 저장될 변수주소

		::CloseHandle(hThread);
	}

	g_hSocket = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,
					NULL, 0, WSA_FLAG_OVERLAPPED);

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

	hThread = ::CreateThread(NULL, 0, ThreadAcceptLoop,
				(LPVOID)NULL, 0, &dwThreadID);
	::CloseHandle(hThread);

	printf("*** 좌표서버를 시작합니다! (포트 %d) ***\n", SERVER_PORT);
	printf("    좌표 패킷 크기 = %d 바이트 / 수신한 당사자에게만 에코\n",
		(int)sizeof(Shared::PlayerMovePacket));
	while (1)
		getchar();

	return 0;
}
