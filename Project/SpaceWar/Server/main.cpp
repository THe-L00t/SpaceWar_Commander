// ============================================================
//  IOCP 좌표 서버 — 플레이어 좌표만 주고받는 최소 구현
//
//  교재의 IOCP 채팅 서버 구조를 그대로 두고, 주고받는 내용만
//  "문자열" -> "플레이어 좌표" 로 바꾼 것이다.
//
//    채팅 서버 : 받은 바이트를 그대로 전원에게 다시 보낸다
//    이 서버   : 받은 바이트를 좌표 패킷으로 해석해 콘솔에 찍고,
//                전원에게 다시 보낸다
//
//  패킷 구조는 Shared/Protocol.h 의 PlayerMovePacket 을 그대로 쓴다.
//  (클라와 서버가 같은 헤더를 쓰므로 구조가 갈릴 수 없다)
// ============================================================
#include <winsock2.h>
#pragma comment(lib, "ws2_32")
#include <windows.h>
#include <list>
#include <cstdio>
#include "Shared/Protocol.h"

/////////////////////////////////////////////////////////////////////////
typedef struct _USERSESSION
{
	SOCKET	hSocket;
	UINT32	nPlayerId;			//서버가 붙여준 번호
	char	buffer[8192];		//8KB
	int		nRecvd;				//buffer 에 쌓인 바이트 수
} USERSESSION;

/////////////////////////////////////////////////////////////////////////
//클라이언트 처리를 위한 작업자 스레드 개수.
#define MAX_THREAD_CNT		4
#define SERVER_PORT			25000

CRITICAL_SECTION	g_cs;			//스레드 동기화 객체
std::list<SOCKET>	g_listClient;	//연결된 클라이언트 소켓 리스트.
SOCKET	g_hSocket;					//서버의 리슨 소켓
HANDLE	g_hIocp;					//IOCP 핸들
LONG	g_nNextPlayerId = 0;		//플레이어 번호 발급기


/////////////////////////////////////////////////////////////////////////
//연결된 클라이언트 모두에게 좌표를 전송한다.
void SendMessageAll(const char *pszMessage, int nSize)
{
	std::list<SOCKET>::iterator it;

	::EnterCriticalSection(&g_cs);
	for (it = g_listClient.begin(); it != g_listClient.end(); ++it)
		::send(*it, pszMessage, nSize, 0);
	::LeaveCriticalSection(&g_cs);
}

/////////////////////////////////////////////////////////////////////////
//연결된 모든 클라이언트 및 리슨 소켓을 닫는다.
void CloseAll()
{
	std::list<SOCKET>::iterator it;

	::EnterCriticalSection(&g_cs);
	for (it = g_listClient.begin(); it != g_listClient.end(); ++it)
	{
		::shutdown(*it, SD_BOTH);
		::closesocket(*it);
	}
	g_listClient.clear();
	::LeaveCriticalSection(&g_cs);
}

/////////////////////////////////////////////////////////////////////////
void CloseClient(SOCKET hSock)
{
	::shutdown(hSock, SD_BOTH);
	::closesocket(hSock);

	::EnterCriticalSection(&g_cs);
	g_listClient.remove(hSock);
	::LeaveCriticalSection(&g_cs);
}

/////////////////////////////////////////////////////////////////////////
void ReleaseServer(void)
{
	//클라이언트 연결을 모두 종료한다.
	CloseAll();
	::Sleep(500);

	//Listen 소켓을 닫는다.
	::shutdown(g_hSocket, SD_BOTH);
	::closesocket(g_hSocket);
	g_hSocket = NULL;

	//IOCP 핸들을 닫는다. 이렇게 하면 GQCS() 함수가 FALSE를 반환하며
	//::GetLastError() 함수가 ERROR_ABANDONED_WAIT_0을 반환한다.
	//IOCP 스레드들이 모두 종료된다.
	::CloseHandle(g_hIocp);
	g_hIocp = NULL;

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
//받은 바이트에서 완전한 좌표 패킷을 꺼내 콘솔에 찍고 전원에게 보낸다.
//
//  ★ 왜 이런 처리가 필요한가
//    TCP 는 바이트 스트림이라 패킷 경계가 없다.
//    32바이트짜리 좌표 패킷 하나가 20바이트 + 12바이트로 쪼개져 올 수도 있고,
//    두 개가 64바이트로 붙어서 한 번에 올 수도 있다.
//    채팅은 글자가 몇 개씩 나뉘어도 그냥 이어붙으면 그만이지만,
//    좌표는 중간에서 잘리면 숫자가 통째로 깨진다.
//    그래서 "32바이트가 다 모였을 때만" 해석한다.
void ProcessPackets(USERSESSION *pSession)
{
	const int nPacketSize = (int)sizeof(Shared::PlayerMovePacket);
	int nOffset = 0;

	while (pSession->nRecvd - nOffset >= nPacketSize)
	{
		Shared::PlayerMovePacket *pMove =
			(Shared::PlayerMovePacket *)(pSession->buffer + nOffset);

		if (pMove->header.type == Shared::PacketType::PlayerMove)
		{
			//서버가 하는 일은 이게 전부다 — 좌표를 콘솔에 찍는다.
			printf("[좌표] 플레이어 %u : ( %8.2f, %8.2f, %8.2f )\n",
				pSession->nPlayerId,
				pMove->pos[0], pMove->pos[1], pMove->pos[2]);

			//받은 좌표를 그대로 전원에게 전달한다. (채팅서버의 SendMessageAll 과 같은 자리)
			SendMessageAll((const char *)pMove, nPacketSize);
		}

		nOffset += nPacketSize;
	}

	//처리하고 남은 조각을 버퍼 앞으로 당겨둔다. 다음 수신 때 이어붙는다.
	pSession->nRecvd -= nOffset;
	if (pSession->nRecvd > 0 && nOffset > 0)
		::memmove(pSession->buffer, pSession->buffer + nOffset, pSession->nRecvd);
}

/////////////////////////////////////////////////////////////////////////
//다음 수신을 IOCP 에 예약한다.
//
//  ★ 이미 받아둔 조각(nRecvd) 뒤에 이어서 받아야 한다.
//    항상 buffer 앞부터 받으면 앞서 받은 조각을 덮어써 버린다.
void PostRecv(USERSESSION *pSession, LPWSAOVERLAPPED pWol)
{
	DWORD	dwReceiveSize	= 0;
	DWORD	dwFlag			= 0;
	WSABUF	wsaBuf			= { 0 };

	wsaBuf.buf = pSession->buffer + pSession->nRecvd;
	wsaBuf.len = sizeof(pSession->buffer) - pSession->nRecvd;

	int nResult = ::WSARecv(
		pSession->hSocket,	//클라이언트 소켓 핸들
		&wsaBuf,			//WSABUF 구조체 배열의 주소
		1,					//배열 요소의 개수
		&dwReceiveSize,
		&dwFlag,
		pWol,
		NULL);

	//SOCKET_ERROR 일 때만 오류를 확인한다.
	//(성공했을 때 WSAGetLastError() 를 보면 이전 호출의 값이 남아 있어 오진한다)
	if (nResult == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
		puts("\tERROR: WSARecv()");
}

/////////////////////////////////////////////////////////////////////////
DWORD WINAPI ThreadComplete(LPVOID pParam)
{
	DWORD			dwTransferredSize = 0;
	USERSESSION		*pSession = NULL;
	LPWSAOVERLAPPED	pWol = NULL;
	BOOL			bResult;

	puts("[IOCP 작업자 스레드 시작]");
	while (1)
	{
		bResult = ::GetQueuedCompletionStatus(
			g_hIocp,				//Dequeue할 IOCP 핸들.
			&dwTransferredSize,		//수신한 데이터 크기.
			(PULONG_PTR)&pSession,	//수신된 데이터가 저장된 메모리
			&pWol,					//OVERLAPPED 구조체.
			INFINITE);				//이벤트를 무한정 대기.

		if (bResult == TRUE)
		{
			//정상적인 경우.

			/////////////////////////////////////////////////////////////
			//1. 클라이언트가 소켓을 정상적으로 닫고 연결을 끊은 경우.
			if (dwTransferredSize == 0)
			{
				printf("\t플레이어 %u 퇴장 (정상 종료)\n", pSession->nPlayerId);
				CloseClient(pSession->hSocket);
				delete pWol;
				delete pSession;
			}

			/////////////////////////////////////////////////////////////
			//2. 클라이언트가 보낸 좌표를 수신한 경우.
			else
			{
				//받은 만큼 쌓고, 완전한 패킷만 골라 처리한다.
				pSession->nRecvd += (int)dwTransferredSize;
				ProcessPackets(pSession);

				//다시 IOCP에 등록.
				PostRecv(pSession, pWol);
			}
		}
		else
		{
			//비정상적인 경우.

			/////////////////////////////////////////////////////////////
			//3. 완료 큐에서 완료 패킷을 꺼내지 못하고 반환한 경우.
			if (pWol == NULL)
			{
				//IOCP 핸들이 닫힌 경우(서버를 종료하는 경우)도 해당된다.
				puts("\tGQCS: IOCP 핸들이 닫혔습니다.");
				break;
			}

			/////////////////////////////////////////////////////////////
			//4. 클라이언트가 비정상적으로 종료됐거나
			//   서버가 먼저 연결을 종료한 경우.
			else
			{
				if (pSession != NULL)
				{
					printf("\t플레이어 %u 퇴장 (비정상 종료)\n", pSession->nPlayerId);
					CloseClient(pSession->hSocket);
					delete pWol;
					delete pSession;
				}
			}
		}
	}

	puts("[IOCP 작업자 스레드 종료]");
	return 0;
}

/////////////////////////////////////////////////////////////////////////
DWORD WINAPI ThreadAcceptLoop(LPVOID pParam)
{
	LPWSAOVERLAPPED	pWol = NULL;
	USERSESSION		*pNewUser;
	int				nAddrSize = sizeof(SOCKADDR);
	SOCKADDR		ClientAddr;
	SOCKET			hClient;

	while ((hClient = ::accept(g_hSocket,
					&ClientAddr, &nAddrSize)) != INVALID_SOCKET)
	{
		::EnterCriticalSection(&g_cs);
		g_listClient.push_back(hClient);
		::LeaveCriticalSection(&g_cs);

		//새 클라이언트에 대한 세션 객체 생성
		pNewUser = new USERSESSION;
		::ZeroMemory(pNewUser, sizeof(USERSESSION));
		pNewUser->hSocket = hClient;
		pNewUser->nPlayerId = (UINT32)::InterlockedIncrement(&g_nNextPlayerId);
		pNewUser->nRecvd = 0;

		printf("새 플레이어가 연결됐습니다. (플레이어 %u)\n", pNewUser->nPlayerId);

		//비동기 수신 처리를 위한 OVERLAPPED 구조체 생성.
		pWol = new WSAOVERLAPPED;
		::ZeroMemory(pWol, sizeof(WSAOVERLAPPED));

		//(연결된) 클라이언트 소켓 핸들을 IOCP에 연결.
		::CreateIoCompletionPort( (HANDLE)hClient, g_hIocp,
			(ULONG_PTR)pNewUser,		//KEY!!!
			0);

		//클라이언트가 보낸 좌표를 비동기 수신한다.
		PostRecv(pNewUser, pWol);
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
	printf("    좌표 패킷 크기 = %d 바이트\n", (int)sizeof(Shared::PlayerMovePacket));
	while (1)
		getchar();

	return 0;
}
