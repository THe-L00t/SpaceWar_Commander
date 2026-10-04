#include <winsock2.h>
#pragma comment(lib, "ws2_32")
#include <windows.h>
#include <objbase.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <cstdio>
#include "Shared/Protocol.h"
#include "Shared/PlanetConst.h"
#include "Shared/GameLogic/GameLogic.h"
#include "Shared/Physics/PhysicsCalculator.h"
#include "Shared/Terrain/HeightmapLoader.h"
#include "Shared/Terrain/TerrainSampler.h"
#include "AI/NpcWorld.h"

#define MAX_THREAD_CNT		4
#define SERVER_PORT			25000
#define RECV_BUFFER_SIZE	8192		//8KB
#define GROUND_TOLERANCE	0.05		//위치 검사 허용오차 (m). float 전송 오차는 0.1mm 수준이다

//세션이 정의되기 전에 쓰이므로 미리 선언해 둔다.
void SendToAll(const void *pData, int nLen, UINT32 nExceptId);
bool ClampToGround(float pos[3]);

/////////////////////////////////////////////////////////////////////////
//  발사 요청 — IOCP 워커가 넣고, NPC 행동 스레드가 꺼내 판정한다.
//
//  ★ 왜 워커에서 바로 판정하지 않는가
//    NpcWorld 는 «행동 스레드 하나만 건드린다» 는 약속 위에 서 있다(NpcWorld.h).
//    워커가 패킷을 받은 자리에서 NPC 를 깎으면 그 약속이 깨지고,
//    같은 NPC 를 두 스레드가 동시에 죽이는 경쟁이 생긴다.
//    워커는 «쐈다» 만 남기고, 판정·피해·재등장은 전부 틱 스레드에서 한 번에 한다.
struct FireRequest
{
	UINT32	nShooterId;
	float	origin[3];
	float	dir[3];
};

void QueueFire(UINT32 nShooterId, const float origin[3], const float dir[3]);

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
		  m_bHasPos(false), m_fHealth(Shared::kMaxHealth), m_nLastFireMs(0),
		  m_recvCtx(IO_TYPE::RECV)
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

	//체력. ★ 쓰는 쪽은 NPC 행동 스레드 하나뿐이다 (사격 판정이 거기서만 돈다).
	float	Health()	const		{ return m_fHealth; }
	void	SetHealth(float f)		{ m_fHealth = f; }

	//되살아날 자리로 옮긴다. 다음 PlayerMove 가 올 때까지 이 자리가 서버가 아는 위치다.
	void	Teleport(const float pos[3])
	{
		m_lastPos[0] = pos[0];
		m_lastPos[1] = pos[1];
		m_lastPos[2] = pos[2];
		m_bHasPos = true;
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
		switch (pHead->type)
		{
		case Shared::PacketType::PlayerMove:
			if (nSize == (int)sizeof(Shared::PlayerMovePacket))
				OnPlayerMove((const Shared::PlayerMovePacket *)pHead);
			break;

		case Shared::PacketType::PlayerFire:
			if (nSize == (int)sizeof(Shared::PlayerFirePacket))
				OnPlayerFire((const Shared::PlayerFirePacket *)pHead);
			break;

		default:
			break;		//모르는 종류는 크기만큼 건너뛴다
		}
	}

	/////////////////////////////////////////////////////////////////////
	//  좌클릭 한 발. ★ 여기서 판정하지 않는다 — 큐에 넣기만 한다.
	//
	//  연사 제한은 워커에서 건다. 조작된 클라가 한 프레임에 수백 발을 보내도
	//  큐가 부풀지 않는다. 이 값은 이 세션만 만지므로 락이 필요 없다.
	void OnPlayerFire(const Shared::PlayerFirePacket *pFire)
	{
		const ULONGLONG nNow = ::GetTickCount64();
		const ULONGLONG nInterval = (ULONGLONG)(Shared::kFireInterval * 1000.0f);

		if (m_nLastFireMs != 0 && nNow - m_nLastFireMs < nInterval)
			return;

		m_nLastFireMs = nNow;
		QueueFire(m_nPlayerId, pFire->origin, pFire->direction);
	}

	/////////////////////////////////////////////////////////////////////
	//  좌표 한 번.
	void OnPlayerMove(const Shared::PlayerMovePacket *pMove)
	{
		//★ 위치 검사 (서버 권위) — 지형 아래면 지면으로 올린 좌표를 쓴다.
		//  고친 좌표를 저장하고 뿌리므로 NPC 추격도 다른 플레이어 화면도 서버 판정을 따른다.
		//  보낸 당사자에게는 아직 알리지 않는다.
		float pos[3] = { pMove->pos[0], pMove->pos[1], pMove->pos[2] };
		if (ClampToGround(pos))
		{
			printf("[위치 보정] 플레이어 %u : 지형 아래 ( %8.2f, %8.2f, %8.2f ) -> ( %8.2f, %8.2f, %8.2f )\n",
				m_nPlayerId,
				pMove->pos[0], pMove->pos[1], pMove->pos[2],
				pos[0], pos[1], pos[2]);
		}

		m_lastPos[0] = pos[0];
		m_lastPos[1] = pos[1];
		m_lastPos[2] = pos[2];
		m_bHasPos = true;

		printf("[좌표] 플레이어 %u : ( %8.2f, %8.2f, %8.2f )\n",
			m_nPlayerId,
			pos[0], pos[1], pos[2]);

		//★ 전원에게 알린다. 보낸 사람은 자기 위치를 이미 알고 있으므로 제외한다.
		//  (돌려주면 자기 큐브가 30Hz 로 과거 위치로 끌려간다)
		Shared::PlayerMovePacket bcast = *pMove;
		bcast.playerId = m_nPlayerId;		//서버가 붙인 번호로 바꿔서 보낸다
		bcast.pos[0] = pos[0];
		bcast.pos[1] = pos[1];
		bcast.pos[2] = pos[2];
		SendToAll(&bcast, (int)sizeof(bcast), m_nPlayerId);
	}

	SOCKET		m_hSocket;
	UINT32		m_nPlayerId;

	char		m_buffer[RECV_BUFFER_SIZE];
	int			m_nRecvd;			//buffer 에 쌓인 바이트 수

	float		m_lastPos[3];		//마지막으로 받은 위치
	bool		m_bHasPos;			//한 번이라도 좌표를 보냈는가

	float		m_fHealth;			//남은 체력. 쓰는 쪽은 행동 스레드 하나
	ULONGLONG	m_nLastFireMs;		//마지막 발사 시각 (연사 제한). 이 세션의 워커만 만진다

	IO_CONTEXT	m_recvCtx;
};

typedef std::shared_ptr<Session> SessionPtr;

CRITICAL_SECTION					g_cs;			//스레드 동기화 객체
std::unordered_map<UINT32, SessionPtr>	g_sessions;	//플레이어번호 -> 세션
SOCKET	g_hSocket;									//서버의 리슨 소켓
HANDLE	g_hIocp;									//IOCP 핸들
LONG	g_nNextPlayerId = 0;						//플레이어 번호 발급기

//NPC 무리. 행동 스레드 하나만 건드린다.
srv::NpcWorld	g_npcWorld;

//발사 요청 큐. IOCP 워커가 넣고 행동 스레드가 비운다.
CRITICAL_SECTION			g_csFire;
std::vector<FireRequest>	g_fireQueue;
volatile LONG	g_bRunning = 1;						//종료 시 행동 스레드를 세운다

//지형. 시작할 때 한 번 읽고 그 뒤로는 읽기만 하므로 스레드 간에 락이 필요 없다.
Shared::HeightmapData	g_heightmap;
Shared::TerrainSampler	g_terrain;

/////////////////////////////////////////////////////////////////////////
//  지형을 읽는다. 클라와 같은 파일(Shared::kTerrainTileAsset)을 같은 설정으로.
//
//  ★ 경로는 exe 옆 assets\ 기준이다. 빌드 후 이벤트가 png 를 복사해 둔다.
//    작업 디렉터리 기준으로 찾으면 VS 에서 켤 때와 exe 를 직접 켤 때가 갈린다.
//  ★ WIC 로더가 COM 객체라 CoInitializeEx 가 먼저 있어야 한다.
//    읽고 나면 COM 은 더 쓰지 않으므로 바로 해제한다.
bool LoadTerrain()
{
	wchar_t szExe[MAX_PATH] = { 0 };
	::GetModuleFileNameW(NULL, szExe, MAX_PATH);

	std::wstring path(szExe);
	const size_t nSlash = path.find_last_of(L"\\/");
	path = (nSlash == std::wstring::npos) ? std::wstring() : path.substr(0, nSlash + 1);
	path += L"assets\\";
	path += Shared::kTerrainTileAsset;

	if (FAILED(::CoInitializeEx(NULL, COINIT_MULTITHREADED)))
	{
		puts("ERROR: COM 을 초기화할 수 없습니다.");
		return false;
	}

	std::wstring error;
	const bool bLoaded = Shared::LoadHeightmapPng(path.c_str(), g_heightmap, error);
	::CoUninitialize();

	if (!bLoaded)
	{
		char szPath[MAX_PATH * 2] = { 0 };
		char szError[256] = { 0 };
		::WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, szPath, sizeof(szPath), NULL, NULL);
		::WideCharToMultiByte(CP_ACP, 0, error.c_str(), -1, szError, sizeof(szError), NULL, NULL);
		printf("ERROR: 지형을 읽을 수 없습니다. (%s)\n\t%s\n", szError, szPath);
		return false;
	}

	g_terrain.Configure(&g_heightmap, Shared::kPlanetRadius, Shared::TerrainConfig{});
	printf("지형 %ux%u (mean %.3f) 을 읽었습니다.\n",
		g_heightmap.size, g_heightmap.size, g_heightmap.mean);
	return true;
}

/////////////////////////////////////////////////////////////////////////
//  플레이어 위치 검사 — 지형 아래면 지면으로 올린다. 올렸으면 true.
//
//  ★ 클라 착지와 같은 TerrainSampler · 같은 kGroundOffset 으로 잰다
//    정직한 클라는 착지하면 정확히 «지면 + kGroundOffset» 을 보내므로 걸리지 않는다.
//    걸리는 것은 조작됐거나 지형 설정이 서버와 다른 클라다.
//  ★ 위쪽은 검사하지 않는다 — 점프와 절벽 낙하로 정상적으로 뜬다.
bool ClampToGround(float pos[3])
{
	const double dx = (double)pos[0] - Shared::kPlanetCenterX;
	const double dy = (double)pos[1] - Shared::kPlanetCenterY;
	const double dz = (double)pos[2] - Shared::kPlanetCenterZ;
	const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (len < 1.0e-6)
		return false;			//행성 중심. 방향을 알 수 없다

	const double ux = dx / len;
	const double uy = dy / len;
	const double uz = dz / len;

	const double floorRadius = Shared::kPlanetRadius
		+ g_terrain.Height(ux, uy, uz) + Shared::kGroundOffset;

	if (len >= floorRadius - GROUND_TOLERANCE)
		return false;

	pos[0] = (float)(Shared::kPlanetCenterX + ux * floorRadius);
	pos[1] = (float)(Shared::kPlanetCenterY + uy * floorRadius);
	pos[2] = (float)(Shared::kPlanetCenterZ + uz * floorRadius);
	return true;
}

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
	//행동 스레드부터 세운다. 소켓이 닫힌 뒤에 브로드캐스트를 돌면 안 된다.
	::InterlockedExchange(&g_bRunning, 0);
	::Sleep(100);

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
	::DeleteCriticalSection(&g_csFire);
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

/////////////////////////////////////////////////////////////////////////
//  NPC 행동 스레드
//
//  ★ 행동 계산도 위치 판정도 서버가 한다 (아키텍처 명세서 6.13).
//    클라는 결과 위치를 받아 그리기만 한다. 다른 플레이어를 그리는 경로와 똑같다.
//
//  ★ 30Hz — 클라가 좌표를 보내는 주기(1/30초)와 맞춘다.
//    더 자주 보내도 클라가 100ms 보간 지연을 두고 그리므로 부드러움은 늘지 않고
//    대역폭만 는다.
//
//  ★ 스레드를 하나만 쓴다
//    NPC 를 건드리는 것은 이 스레드뿐이다. IOCP 작업자 스레드는 g_npcWorld 를
//    모른다. 그래서 NPC 쪽에는 락이 없다.
/////////////////////////////////////////////////////////////////////////
//  발사 큐 — 워커가 넣고 행동 스레드가 통째로 가져간다.
void QueueFire(UINT32 nShooterId, const float origin[3], const float dir[3])
{
	FireRequest req = {};
	req.nShooterId = nShooterId;
	req.origin[0] = origin[0];
	req.origin[1] = origin[1];
	req.origin[2] = origin[2];
	req.dir[0] = dir[0];
	req.dir[1] = dir[1];
	req.dir[2] = dir[2];

	::EnterCriticalSection(&g_csFire);
	//상한을 둔다. 조작된 클라가 큐를 부풀려도 메모리가 늘지 않는다.
	if (g_fireQueue.size() < 256)
		g_fireQueue.push_back(req);
	::LeaveCriticalSection(&g_csFire);
}

void TakeFires(std::vector<FireRequest> &out)
{
	out.clear();

	::EnterCriticalSection(&g_csFire);
	out.swap(g_fireQueue);
	::LeaveCriticalSection(&g_csFire);
}

/////////////////////////////////////////////////////////////////////////
//  시작 위치 — 월드 원점 방향(0,1,0) 의 지면 위.
//
//  ★ 클라 스폰과 같은 규약이다: 기준구 반지름 + 지형 높이 + kGroundOffset.
//    클라가 자기 판단으로 되살아나지 않고 이 좌표를 받아 옮겨간다(서버 권위).
void SpawnPoint(float outPos[3])
{
	const double height = g_terrain.Height(0.0, 1.0, 0.0);
	const double radius = Shared::kPlanetRadius + height + Shared::kGroundOffset;

	outPos[0] = (float)Shared::kPlanetCenterX;
	outPos[1] = (float)(Shared::kPlanetCenterY + radius);
	outPos[2] = (float)Shared::kPlanetCenterZ;
}

void BroadcastHealth(UINT32 nPlayerId, UINT32 nAttackerId, float fHealth)
{
	Shared::PlayerHealthPacket pkt = {};
	pkt.header.size = (uint16_t)sizeof(pkt);
	pkt.header.type = Shared::PacketType::PlayerHealth;
	pkt.playerId = nPlayerId;
	pkt.attackerId = nAttackerId;
	pkt.health = fHealth;

	SendToAll(&pkt, (int)sizeof(pkt), 0);
}

void BroadcastNeutralized(UINT32 nPlayerId, UINT32 nAttackerId,
	const float pos[3], float fHealth)
{
	Shared::PlayerNeutralizedPacket pkt = {};
	pkt.header.size = (uint16_t)sizeof(pkt);
	pkt.header.type = Shared::PacketType::PlayerNeutralized;
	pkt.playerId = nPlayerId;
	pkt.attackerId = nAttackerId;
	pkt.pos[0] = pos[0];
	pkt.pos[1] = pos[1];
	pkt.pos[2] = pos[2];
	pkt.health = fHealth;

	SendToAll(&pkt, (int)sizeof(pkt), 0);
}

void BroadcastNpcDespawn(UINT32 nNpcId)
{
	Shared::NpcDespawnPacket pkt = {};
	pkt.header.size = (uint16_t)sizeof(pkt);
	pkt.header.type = Shared::PacketType::NpcDespawn;
	pkt.npcId = nNpcId;

	SendToAll(&pkt, (int)sizeof(pkt), 0);
}

/////////////////////////////////////////////////////////////////////////
//  한 발을 판정한다. ★ 행동 스레드에서만 부른다.
//
//  순서: 쏜 사람 -> 방향·원점 검사 -> 가장 가까운 대상 -> 지형 가림 -> 피해.
//  클라는 «누가 맞았는지» 를 말하지 않는다 (명세 18절 원칙 4).
void ResolveFire(const FireRequest &req, const std::vector<SessionPtr> &list)
{
	//1) 쏜 사람. 그 사이 나갔으면 버린다.
	SessionPtr pShooter;
	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i]->PlayerId() == req.nShooterId) { pShooter = list[i]; break; }
	}
	if (!pShooter || !pShooter->HasPos()) return;

	//2) 방향. 길이가 0 이면 버린다.
	const double dx = req.dir[0], dy = req.dir[1], dz = req.dir[2];
	const double dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (dlen < 1.0e-6) return;

	const Shared::Vec3 dir = { (float)(dx / dlen), (float)(dy / dlen), (float)(dz / dlen) };

	//3) 원점 검사. 서버가 아는 위치에서 너무 벌어지면 서버 위치에서 쏜 것으로 한다.
	float serverPos[3];
	pShooter->LastPos(serverPos);

	Shared::Vec3 origin = { req.origin[0], req.origin[1], req.origin[2] };

	const double ox = origin.x - serverPos[0];
	const double oy = origin.y - serverPos[1];
	const double oz = origin.z - serverPos[2];

	if (std::sqrt(ox * ox + oy * oy + oz * oz) > Shared::kMuzzleTolerance)
	{
		printf("[사격 보정] 플레이어 %u : 총구가 %.1fm 벗어나 서버 위치로 쏩니다\n",
			req.nShooterId, std::sqrt(ox * ox + oy * oy + oz * oz));
		origin.x = serverPos[0];
		origin.y = serverPos[1];
		origin.z = serverPos[2];
	}

	//4) 대상 — 다른 플레이어와 NPC 중 가장 가까운 것 하나.
	bool	bHit		= false;
	float	fBest		= Shared::kFireRange;
	UINT32	nHitPlayer	= 0;
	UINT32	nHitNpc		= 0;

	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i]->PlayerId() == req.nShooterId) continue;
		if (!list[i]->HasPos()) continue;

		float target[3];
		list[i]->LastPos(target);

		const Shared::Vec3 center = { target[0], target[1], target[2] };

		float fDist = 0.0f;
		if (!Shared::PhysicsCalculator::RaySphere(origin, dir, center,
			Shared::kHitRadius, fBest, fDist))
			continue;

		bHit = true;
		fBest = fDist;
		nHitPlayer = list[i]->PlayerId();
		nHitNpc = 0;
	}

	{
		uint32_t nNpcId = 0;
		float    fNpcDist = 0.0f;
		if (g_npcWorld.Raycast(origin, dir, fBest, nNpcId, fNpcDist))
		{
			bHit = true;
			fBest = fNpcDist;
			nHitNpc = nNpcId;
			nHitPlayer = 0;
		}
	}

	if (!bHit) return;

	//5) 지형이 막고 있으면 맞지 않는다.
	if (Shared::PhysicsCalculator::TerrainBlocks(&g_terrain, origin, dir, fBest))
	{
		printf("[사격] 플레이어 %u : 지형에 막혔습니다 (%.1fm)\n", req.nShooterId, fBest);
		return;
	}

	//6) 피해.
	if (nHitNpc != 0)
	{
		if (g_npcWorld.Damage(nHitNpc, Shared::kShotDamage))
			printf("[명중] 플레이어 %u -> NPC %u 쓰러짐 (%.1fm)\n", req.nShooterId, nHitNpc, fBest);
		else
			printf("[명중] 플레이어 %u -> NPC %u (%.1fm)\n", req.nShooterId, nHitNpc, fBest);
		return;
	}

	SessionPtr pVictim;
	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i]->PlayerId() == nHitPlayer) { pVictim = list[i]; break; }
	}
	if (!pVictim) return;

	const float fLeft = Shared::GameLogic::ApplyDamage(pVictim->Health(), Shared::kShotDamage);
	pVictim->SetHealth(fLeft);
	BroadcastHealth(nHitPlayer, req.nShooterId, fLeft);

	printf("[명중] 플레이어 %u -> 플레이어 %u : 남은 체력 %.0f (%.1fm)\n",
		req.nShooterId, nHitPlayer, fLeft, fBest);

	if (!Shared::GameLogic::IsDown(fLeft)) return;

	//★ 쓰러진 상태가 따로 없다 — 교수님 지시대로 시작 위치로 옮기고 체력을 채운다.
	float spawn[3];
	SpawnPoint(spawn);

	pVictim->SetHealth(Shared::kMaxHealth);
	pVictim->Teleport(spawn);
	BroadcastNeutralized(nHitPlayer, req.nShooterId, spawn, Shared::kMaxHealth);

	printf("[무력화] 플레이어 %u 가 시작 위치로 돌아갑니다 (가해 %u)\n",
		nHitPlayer, req.nShooterId);
}

DWORD WINAPI ThreadNpcTick(LPVOID pParam)
{
	const DWORD dwIntervalMs = 33;
	const float fDt = (float)dwIntervalMs / 1000.0f;

	std::vector<srv::PlayerView> players;
	std::vector<srv::NpcView>    views;
	std::vector<FireRequest>     fires;
	std::vector<uint32_t>        despawned;

	int nTicks = 0;

	puts("[NPC 행동 스레드 시작]");

	while (::InterlockedCompareExchange(&g_bRunning, 1, 1) == 1)
	{
		::Sleep(dwIntervalMs);

		//1) 위치를 한 번이라도 보낸 플레이어를 모은다.
		players.clear();
		{
			std::vector<SessionPtr> list = SnapshotSessions();

			for (size_t i = 0; i < list.size(); ++i)
			{
				if (!list[i]->HasPos()) continue;

				float pos[3];
				list[i]->LastPos(pos);

				srv::PlayerView pv;
				pv.playerId = list[i]->PlayerId();
				pv.pos.x = pos[0];
				pv.pos.y = pos[1];
				pv.pos.z = pos[2];
				players.push_back(pv);
			}
		}

		//2) 첫 플레이어가 자리를 잡으면 그 주변에 한 번 깔아둔다.
		if (g_npcWorld.Empty())
		{
			if (players.empty()) continue;

			g_npcWorld.SpawnAround(players[0].pos, srv::kNpcSpawnCount);
			printf("NPC %zu 마리를 플레이어 %u 주변에 배치했습니다.\n",
				g_npcWorld.Count(), players[0].playerId);
		}

		//3) 행동 트리를 돌리고 결정대로 위치를 옮긴다. 쓰러진 NPC 의 재등장 시계도 여기서 돈다.
		g_npcWorld.Tick(players, fDt);

		//3-1) 이번 틱에 들어온 사격을 판정한다.
		//     ★ 판정은 이 스레드 한 곳에서만 한다. 워커는 큐에 넣기만 했다.
		TakeFires(fires);
		if (!fires.empty())
		{
			std::vector<SessionPtr> list = SnapshotSessions();

			for (size_t i = 0; i < fires.size(); ++i)
				ResolveFire(fires[i], list);
		}

		//3-2) 쓰러진 NPC 를 알린다. 이게 없으면 클라 화면에 그대로 서 있는다.
		g_npcWorld.TakeDespawned(despawned);
		for (size_t i = 0; i < despawned.size(); ++i)
			BroadcastNpcDespawn((UINT32)despawned[i]);

		//4) 전원에게 뿌린다. PlayerMove 브로드캐스트와 같은 방식이다.
		g_npcWorld.Snapshot(views);

		for (size_t i = 0; i < views.size(); ++i)
		{
			Shared::NpcStatePacket pkt = {};
			pkt.header.size = (uint16_t)sizeof(pkt);
			pkt.header.type = Shared::PacketType::NpcState;
			pkt.npcId  = views[i].npcId;
			pkt.pos[0] = views[i].pos.x;
			pkt.pos[1] = views[i].pos.y;
			pkt.pos[2] = views[i].pos.z;

			SendToAll(&pkt, (int)sizeof(pkt), 0);
		}

		//1초에 한 번만 상태를 찍는다. 매 틱 찍으면 좌표 로그에 묻힌다.
		if (++nTicks >= 30)
		{
			nTicks = 0;
			if (!views.empty())
			{
				printf("[NPC] %zu 마리 / 추격 %d 마리 (플레이어 %zu명)\n",
					views.size(), g_npcWorld.ChasingCount(), players.size());
			}
		}
	}

	puts("[NPC 행동 스레드 종료]");
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

		//1-1) 시작 체력. 클라는 체력 규칙을 모르고 서버가 보낸 값만 표시한다.
		Shared::PlayerHealthPacket health = {};
		health.header.size = (uint16_t)sizeof(health);
		health.header.type = Shared::PacketType::PlayerHealth;
		health.playerId = nPlayerId;
		health.attackerId = 0;			//피해가 아니라 알림
		health.health = pNewUser->Health();
		pNewUser->Send(&health, (int)sizeof(health));

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

	//★ 지형이 없으면 켜지 않는다. 평평한 구로 돌면 골짜기에 선 플레이어를 전부
	//  «지형 아래» 로 오판하고, NPC 고도도 클라 화면과 어긋난다.
	if (!LoadTerrain())
		return 0;
	g_npcWorld.SetTerrain(&g_terrain);

	WSADATA wsa = { 0 };
	if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		puts("ERROR: 윈속을 초기화 할 수 없습니다.");
		return 0;
	}

	::InitializeCriticalSection(&g_cs);
	::InitializeCriticalSection(&g_csFire);

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

	//NPC 행동 스레드. 첫 플레이어가 좌표를 보내면 그 주변에 NPC 를 깔고 굴린다.
	hThread = ::CreateThread(NULL, 0, ThreadNpcTick,
				(LPVOID)NULL, 0, &dwThreadID);
	::CloseHandle(hThread);

	printf("*** 좌표서버를 시작합니다! (포트 %d) ***\n", SERVER_PORT);
	printf("    좌표 패킷 크기 = %d 바이트 / NPC 패킷 크기 = %d 바이트\n",
		(int)sizeof(Shared::PlayerMovePacket),
		(int)sizeof(Shared::NpcStatePacket));
	printf("    NPC %d 마리를 30Hz 로 굴려 전원에게 뿌립니다.\n", srv::kNpcSpawnCount);
	printf("    플레이어가 지형 아래(허용오차 %.2fm)면 지면으로 올립니다.\n", GROUND_TOLERANCE);
	while (1)
		getchar();

	return 0;
}
