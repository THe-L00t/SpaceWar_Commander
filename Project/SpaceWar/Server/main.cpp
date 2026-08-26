#include <winsock2.h>
#pragma comment(lib, "ws2_32")
#include <windows.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include "Shared/Protocol.h"

#define MAX_THREAD_CNT		4
#define SERVER_PORT			25000
#define RECV_BUFFER_SIZE	8192		//8KB

//세션이 정의되기 전에 쓰이므로 미리 선언해 둔다.
void SendToAll(const void *pData, int nLen, UINT32 nExceptId);

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
		  m_bHasPos(false), m_recvCtx(IO_TYPE::RECV)
	{
		::ZeroMemory(m_buffer, sizeof(m_buffer));
		::ZeroMemory(m_lastPos, sizeof(m_lastPos));
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

	//마지막으로 알려진 위치. 새로 들어온 사람에게 기존 플레이어를 그려주려면 필요하다.
	bool	HasPos()	const { return m_bHasPos; }
	void	LastPos(float outPos[3]) const
	{
		outPos[0] = m_lastPos[0];
		outPos[1] = m_lastPos[1];
		outPos[2] = m_lastPos[2];
	}

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
	/////////////////////////////////////////////////////////////////////
	//  header.size 만큼씩 잘라 처리한다.
	//
	//  ★ 고정 크기로 자르면 안 되는 이유
	//    이제 크기가 다른 패킷(Welcome 8바이트, PlayerMove 32바이트)이 오간다.
	//    32바이트 단위로 자르면 8바이트짜리 하나에 경계가 밀려서
	//    그 뒤 스트림이 전부 쓰레기가 된다.
	void ProcessPackets()
	{
		const int nHeaderSize = (int)sizeof(Shared::PacketHeader);
		int nOffset = 0;

		while (m_nRecvd - nOffset >= nHeaderSize)
		{
			const Shared::PacketHeader *pHead =
				(const Shared::PacketHeader *)(m_buffer + nOffset);
			const int nSize = (int)pHead->size;

			//규격에 없는 크기 = 스트림이 이미 어긋났다. 더 읽어도 복구되지 않는다.
			if (nSize < nHeaderSize || nSize > RECV_BUFFER_SIZE)
			{
				printf("\t플레이어 %u : 잘못된 패킷 크기 %d, 버퍼를 버린다\n",
					m_nPlayerId, nSize);
				m_nRecvd = 0;
				return;
			}

			//아직 다 도착하지 않았다. 다음 수신 때 이어서 처리한다.
			if (m_nRecvd - nOffset < nSize)
				break;

			HandlePacket(pHead, nSize);
			nOffset += nSize;
		}

		m_nRecvd -= nOffset;
		if (m_nRecvd > 0 && nOffset > 0)
			::memmove(m_buffer, m_buffer + nOffset, m_nRecvd);
	}

	/////////////////////////////////////////////////////////////////////
	//  완전한 패킷 한 개를 처리한다.
	void HandlePacket(const Shared::PacketHeader *pHead, int nSize)
	{
		if (pHead->type != Shared::PacketType::PlayerMove) return;
		if (nSize != (int)sizeof(Shared::PlayerMovePacket)) return;

		const Shared::PlayerMovePacket *pMove =
			(const Shared::PlayerMovePacket *)pHead;

		m_lastPos[0] = pMove->pos[0];
		m_lastPos[1] = pMove->pos[1];
		m_lastPos[2] = pMove->pos[2];
		m_bHasPos = true;

		printf("[좌표] 플레이어 %u : ( %8.2f, %8.2f, %8.2f )\n",
			m_nPlayerId,
			pMove->pos[0], pMove->pos[1], pMove->pos[2]);

		//★ 전원에게 알린다. 보낸 사람은 자기 위치를 이미 알고 있으므로 제외한다.
		//  (돌려주면 자기 큐브가 30Hz 로 과거 위치로 끌려간다)
		Shared::PlayerMovePacket bcast = *pMove;
		bcast.playerId = m_nPlayerId;		//서버가 붙인 번호로 바꿔서 보낸다
		SendToAll(&bcast, (int)sizeof(bcast), m_nPlayerId);
	}

	SOCKET		m_hSocket;
	UINT32		m_nPlayerId;

	char		m_buffer[RECV_BUFFER_SIZE];
	int			m_nRecvd;			//buffer 에 쌓인 바이트 수

	float		m_lastPos[3];		//마지막으로 받은 위치
	bool		m_bHasPos;			//한 번이라도 좌표를 보냈는가

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

/////////////////////////////////////////////////////////////////////////
//  현재 세션 목록의 복사본을 뜬다.
//
//  ★ 왜 복사하는가
//    락을 잡은 채로 WSASend 를 돌리면, 느린 클라 하나가 목록 전체를 잠근다.
//    복사본을 들고 나와서 보내면 락 구간이 짧게 끝난다.
//    복사된 shared_ptr 덕분에 보내는 도중 세션이 지워져도 안전하다.
std::vector<SessionPtr> SnapshotSessions()
{
	std::vector<SessionPtr> list;

	::EnterCriticalSection(&g_cs);
	list.reserve(g_sessions.size());
	for (std::unordered_map<UINT32, SessionPtr>::iterator it = g_sessions.begin();
		it != g_sessions.end(); ++it)
		list.push_back(it->second);
	::LeaveCriticalSection(&g_cs);

	return list;
}

/////////////////////////////////////////////////////////////////////////
//  전원에게 보낸다. nExceptId 는 제외한다 (0 이면 전원).
//
//  교재 채팅서버의 SendMessageAll 자리다. 1:1 확인 단계에서 뺐던 것을
//  멀티플레이어를 위해 되살린다.
void SendToAll(const void *pData, int nLen, UINT32 nExceptId)
{
	std::vector<SessionPtr> list = SnapshotSessions();

	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i]->PlayerId() == nExceptId) continue;
		list[i]->Send(pData, nLen);
	}
}

void RemoveSession(UINT32 nPlayerId)
{
	size_t nRemain = 0;

	::EnterCriticalSection(&g_cs);
	g_sessions.erase(nPlayerId);		//shared_ptr 이 풀리며 소멸자가 소켓을 닫는다
	nRemain = g_sessions.size();
	::LeaveCriticalSection(&g_cs);

	//★ 남은 사람들에게 알린다. 이걸 빠뜨리면 나간 플레이어의 큐브가
	//  화면에 영원히 서 있는다.
	Shared::PlayerLeavePacket leave = {};
	leave.header.size = (uint16_t)sizeof(leave);
	leave.header.type = Shared::PacketType::PlayerLeave;
	leave.playerId = nPlayerId;
	SendToAll(&leave, (int)sizeof(leave), 0);

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

		/////////////////////////////////////////////////////////////////
		//  ★ 반드시 IOCP 에 연결한 뒤에 보낸다
		//    소켓이 IOCP 에 붙기 전에 WSASend 를 걸면 완료 통보가 오지 않아
		//    IO_CONTEXT 가 그대로 샌다.

		//1) 너는 몇 번인가. 이게 있어야 클라가 브로드캐스트에서 자기 것을 걸러낸다.
		Shared::WelcomePacket hello = {};
		hello.header.size = (uint16_t)sizeof(hello);
		hello.header.type = Shared::PacketType::Welcome;
		hello.playerId = nPlayerId;
		pNewUser->Send(&hello, (int)sizeof(hello));

		//2) 이미 들어와 있는 사람들의 위치를 한 명씩 보낸다.
		//   이게 없으면 새로 들어온 사람 화면엔, 기존 플레이어가 "움직일 때까지"
		//   아무도 안 보인다. 가만히 서 있는 사람은 영영 안 보인다.
		{
			std::vector<SessionPtr> list = SnapshotSessions();
			int nSentRoster = 0;

			for (size_t i = 0; i < list.size(); ++i)
			{
				if (list[i]->PlayerId() == nPlayerId) continue;
				if (!list[i]->HasPos()) continue;

				Shared::PlayerMovePacket intro = {};
				intro.header.size = (uint16_t)sizeof(intro);
				intro.header.type = Shared::PacketType::PlayerMove;
				intro.playerId = list[i]->PlayerId();
				list[i]->LastPos(intro.pos);
				pNewUser->Send(&intro, (int)sizeof(intro));
				++nSentRoster;
			}

			printf("\t기존 플레이어 %d명의 위치를 보냈습니다.\n", nSentRoster);
		}

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
