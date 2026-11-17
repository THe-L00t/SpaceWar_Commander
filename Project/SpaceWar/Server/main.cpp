#include <winsock2.h>
#pragma comment(lib, "ws2_32")
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <cstdio>
#include "Shared/Protocol.h"
#include "Shared/PlanetConst.h"
#include "Shared/GameLogic/GameLogic.h"
#include "Shared/GameLogic/PlayerMovement.h"
#include "Shared/GameLogic/WeaponShowcase.h"
#include "Shared/GameLogic/WeaponPickup.h"
#include "Shared/GameLogic/ShotTrace.h"   // 한 발의 레이 — 몸 · 충돌체 · 지형(클라 겨눈 점과 같은 함수, 2026-11-15)
#include "Shared/GameObject/Weapon.h"
#include "Shared/Physics/CollisionWorld.h"
#include "Shared/Physics/PhysicsCalculator.h"
#include "Shared/Terrain/PlanetSurface.h"
#include "Shared/Terrain/TerrainSampler.h"
#include "Shared/World/Placement.h"
#include "Shared/World/WorldCatalog.h"
#include "Shared/World/WorldMap.h"
#include "AI/NpcWorld.h"

#define MAX_THREAD_CNT		4
#define SERVER_PORT			25000
#define RECV_BUFFER_SIZE	8192		//8KB

// ── 플레이어 이동 (B안 · 입력 기반, 2026-10-29) ──────────────────────
//  클라는 좌표가 아니라 고정 틱(1/60 s) 입력을 보낸다(Shared::PlayerInputPacket).
//  틱 스레드가 같은 이동 코드(Shared::PlayerMovement::Step) · 같은 충돌체(Shared::CollisionWorld)로 판정하고,
//  당사자에게 PlayerState(마지막 입력 번호 + 상태), 다른 사람에게 PlayerMove(위치)를 보낸다.
//  근거: 명세 6.13 Player ②③ · 13절 · 18절 원칙 4, 노션 「충돌 처리 ②」 B안 · 사용자 결정 5번(판정 = 틱 스레드 한 곳).
#define INPUT_RATE			60.0		//클라가 초당 보내는 입력 수 (= 1 / PlayerMovement::kTickSeconds)
#define INPUT_BUDGET_MAX	10.0		//밀린 입력을 한 틱에 최대 이만큼까지 따라잡는다
#define INPUT_QUEUE_MAX		30			//이보다 밀리면(0.5초) 오래된 것부터 버린다 — 지연이 끝없이 쌓이지 않게

// ── 총 줍기 · 떨구기 · 총마다 사격 (2026-11-15, Shared/GameLogic/WeaponPickup.h) ──────────
//  사용자: «주우면 애니메이션 시에 적절한 위치에 총을 고정시키고, 쏠 수 있도록 해줘. 줍기, 떨구기 둘 다 구현하면 되는거야.
//          이때 공중에 있는것을 줍고, 떨구면 땅바닥에 떨어지도록. 공중에 있는 총은 사라지지 않게»
//  · 든 총(세션) · 바닥 총 목록(맵마다)은 틱 스레드만 바꾼다. 워커는 요청만 남긴다(HandleWeaponRequests).
//  · 공중 진열 6개(게임 맵, Shared/GameLogic/WeaponShowcase.h)는 주워도 그대로 — 같은 종류를 하나 더 받는다.
//  · 사격은 든 총으로만 — 발 간격 · 피해 · 사거리 = Shared::SpecOf(든 총). 빈손 사격은 버린다.
//  · 연사 제한 = 연사 예산(ms, OnPlayerFire) — 흐른 시간만큼 차고 한 발에 그 총의 간격만큼 쓴다. 평균은 표의 간격 그대로이고,
//    몰려 온 발(네트워크 · 클라 프레임)은 kFireBurstShots 발까지 한꺼번에 받는다.
//    (2026-11-15 «발 간격의 75 % 이상 떨어져야» 규칙을 바꿈 — 클라가 남은 시간을 이어 쏘면 간격이 프레임마다 들쭉날쭉해
//     기관총 0.08 s 가 20 fps 에서 0.05 · 0.1 s 간격으로 섞여 와 3분의 1 이 버려졌다(시뮬레이션 분당 752 → 514발).
//     반대로 예전 규칙은 조작된 클라에게 평균 1.33 배까지 허용했다.)
constexpr double kFireBurstShots = 3.0;			//한꺼번에 받아 주는 발 수 — 클라 Engine.cpp kMaxShotsPerFrame 과 같게

// ── 맵 번호 (2026-11-01, 사용자 «맵번호 적용해줘») ────────────────────
//  세션마다 맵 번호(0 = 게임 맵, 1 ~ 5 = 예시 행성, Shared/World/WorldCatalog.h)를 들고,
//  같은 맵에 있는 사람끼리만 판정 · 송신한다(이동 · 사격 · 체력 · 위치). NPC 는 게임 맵에만 있다.
//  접속하면 게임 맵에 선다. 클라가 JoinWorld 를 보내면 틱 스레드가 옛 맵에서 빼고(PlayerLeave),
//  그 맵을 준비해(처음이면 ThreadLoadWorld 가 따로 읽는다) 스폰 자리에 세운 뒤 WorldJoined 로 알린다(HandleWorldJoin).
//  근거: 노션 「충돌 처리 ②」 0절 결정 1번 «예시행성돠 게임맵 둘 다» · 4번 B안 · 3절 서버 표 «맵 — WorldSet, 세션마다 worldId».

//세션이 정의되기 전에 쓰이므로 미리 선언해 둔다.
void SendToAll(const void *pData, int nLen, UINT32 nExceptId);

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
	uint8_t	weaponType;		//쏜 순간 든 총(2026-11-15) — 피해 · 사거리를 이 총의 WeaponSpec 에서 읽는다
};

void QueueFire(UINT32 nShooterId, const float origin[3], const float dir[3], uint8_t weaponType);

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
		  m_bHasPos(false), m_fHealth(Shared::kMaxHealth), m_nLastFireMs(0), m_fFireBudgetMs(0.0),
		  m_recvCtx(IO_TYPE::RECV)
	{
		::ZeroMemory(m_buffer, sizeof(m_buffer));
		::ZeroMemory(m_lastPos, sizeof(m_lastPos));
		::InitializeCriticalSection(&m_csInput);
	}

	~Session()
	{
		if (m_hSocket != INVALID_SOCKET)
		{
			::shutdown(m_hSocket, SD_BOTH);
			::closesocket(m_hSocket);
		}
		::DeleteCriticalSection(&m_csInput);
	}

	/////////////////////////////////////////////////////////////////////
	//  이동 (B안, 2026-10-29)
	//  ★ m_move · m_lastSeq 는 틱 스레드만 쓴다. 워커와 나누는 것은 입력 큐(m_inputs)뿐이고 m_csInput 으로 지킨다.
	//    (예외: InitMove 는 접속 스레드가 세션을 목록에 넣기 «전에» 부른다 — 아직 아무도 이 세션을 모른다.)

	//접속 직후 스폰 자리에 세운다. 클라도 같은 스폰 계산(PlayerMovement::Spawn)을 하므로 첫 상태가 같다.
	//  worldId = 처음 서는 맵(게임 맵, 2026-11-01 맵 번호).
	void InitMove(uint32_t worldId, const Shared::MoveWorld &world, const float spawn[3])
	{
		Shared::PlayerMovement::Spawn(m_move, world,
			Shared::Vec3d(spawn[0], spawn[1], spawn[2]), Shared::Vec3d(0.0, 0.0, 1.0));
		m_lastSeq = 0;
		StorePosition();
		m_world = worldId;
	}

	/////////////////////////////////////////////////////////////////////
	//  맵 번호 (2026-11-01) — 맵 바꾸기. 판정 · 위치는 틱 스레드만 쓰므로 워커는 요청만 남긴다(입력 · 사격과 같은 규칙).

	//워커 — JoinWorld 를 받았다. 이 앞에 쌓인 입력은 옛 맵의 것이라 버린다.
	void RequestWorld(uint32_t worldId)
	{
		::EnterCriticalSection(&m_csInput);
		m_inputs.clear();
		m_requestedWorld = worldId;
		m_bHasWorldRequest = true;
		::LeaveCriticalSection(&m_csInput);
	}

	//틱 스레드 — 워커가 남긴 요청을 가져간다.
	bool TakeWorldRequest(uint32_t &worldId)
	{
		bool bHas = false;
		::EnterCriticalSection(&m_csInput);
		if (m_bHasWorldRequest)
		{
			worldId = m_requestedWorld;
			m_bHasWorldRequest = false;
			bHas = true;
		}
		::LeaveCriticalSection(&m_csInput);
		return bHas;
	}

	//틱 스레드 — 지금 맵에서 빠져 worldId 를 기다린다. 그동안 판정 · 송신 · 사격 · NPC 추격 대상에서 빠진다.
	void BeginJoin(uint32_t worldId)
	{
		m_world = Shared::kNoWorld;
		m_joinWorld = worldId;
	}

	//틱 스레드 — 맵에 넣는다. 스폰 자리에 세우고(몸 방향은 이어 간다) 밀린 입력을 버린다.
	//  클라는 WorldJoined 를 받고 같은 자리 · 방향에 다시 선 뒤부터 입력을 보낸다.
	void EnterWorld(uint32_t worldId, const Shared::MoveWorld &world, const float spawn[3])
	{
		Shared::PlayerMovement::Spawn(m_move, world,
			Shared::Vec3d(spawn[0], spawn[1], spawn[2]), m_move.facing);
		::EnterCriticalSection(&m_csInput);
		m_inputs.clear();
		m_inputBudget = 0.0;
		::LeaveCriticalSection(&m_csInput);
		m_joinWorld = Shared::kNoWorld;
		StorePosition();
		m_world = worldId;
	}

	//틱 스레드 — 맵을 준비하지 못했다. 어느 맵에도 없는 채로 둔다(클라는 그 맵에서 혼자 움직인다).
	void FailJoin() { m_joinWorld = Shared::kNoWorld; }

	//지금 맵. 맵을 바꾸는 중 · 맵 없음이면 kNoWorld. 틱 스레드가 쓰고 다른 스레드(접속 명단)는 읽기만 한다.
	uint32_t World() const		{ return m_world.load(); }
	bool     InWorld() const	{ return m_world.load() < Shared::kWorldCount; }
	uint32_t JoiningWorld() const { return m_joinWorld; }		//틱 스레드만

	//몸 방향(접평면 단위 벡터) — WorldJoined 에 싣는다. 틱 스레드만.
	void Facing(float out[3]) const
	{
		out[0] = (float)m_move.facing.x;
		out[1] = (float)m_move.facing.y;
		out[2] = (float)m_move.facing.z;
	}

	/////////////////////////////////////////////////////////////////////
	//  총 (2026-11-15 줍기 · 떨구기, Shared/GameLogic/WeaponPickup.h)
	//  ★ 든 총은 틱 스레드가 정한다. 워커는 요청만 남긴다(입력 · 맵 바꾸기와 같은 규칙, 잠금 m_csInput).
	//    사격 검사(워커, OnPlayerFire)는 든 총의 사본 m_heldType(atomic)을 읽는다.

	//워커 — WeaponPickup 을 받았다. 같은 틱에 여러 번 오면 마지막 것만.
	void RequestPickup(uint8_t source, uint32_t id)
	{
		::EnterCriticalSection(&m_csInput);
		m_pickupSource = source;
		m_pickupId = id;
		m_bHasPickup = true;
		::LeaveCriticalSection(&m_csInput);
	}

	//워커 — WeaponDrop 을 받았다.
	void RequestDrop()
	{
		::EnterCriticalSection(&m_csInput);
		m_bHasDrop = true;
		::LeaveCriticalSection(&m_csInput);
	}

	//틱 스레드 — 워커가 남긴 줍기 · 떨구기 요청을 가져간다.
	void TakeWeaponRequests(bool &bPickup, uint8_t &source, uint32_t &id, bool &bDrop)
	{
		::EnterCriticalSection(&m_csInput);
		bPickup = m_bHasPickup;
		source = m_pickupSource;
		id = m_pickupId;
		bDrop = m_bHasDrop;
		m_bHasPickup = false;
		m_bHasDrop = false;
		::LeaveCriticalSection(&m_csInput);
	}

	Shared::WeaponType HeldWeapon() const				{ return (Shared::WeaponType)m_heldType.load(); }
	void SetHeldWeapon(Shared::WeaponType type)			{ m_heldType = (uint8_t)type; }		//틱 스레드만

	//판정한 이동 상태 — 떨군 총의 손 자리를 구한다. 틱 스레드만.
	const Shared::MoveState &Move() const { return m_move; }

	//그 맵의 바닥 총 · 다른 사람이 든 총을 아직 안 보냈다 — 접속 직후 · 맵을 바꾼 직후. 틱 스레드가 보내고 지운다.
	void MarkWeaponSnapshot()		{ m_bNeedsWeaponSnapshot = true; }
	bool TakeWeaponSnapshot()		{ return m_bNeedsWeaponSnapshot.exchange(false); }

	//되살아난다 — 시작 위치로 옮기고 밀린 입력을 버린다(틱 스레드, ResolveFire).
	void Respawn(const Shared::MoveWorld &world, const float spawn[3])
	{
		Shared::PlayerMovement::Spawn(m_move, world,
			Shared::Vec3d(spawn[0], spawn[1], spawn[2]), m_move.facing);
		::EnterCriticalSection(&m_csInput);
		m_inputs.clear();
		::LeaveCriticalSection(&m_csInput);
		StorePosition();
	}

	//틱 스레드 — 흐른 시간만큼 입력을 꺼내 순서대로 판정한다. 처리한 입력 수를 돌려준다.
	int Simulate(const Shared::MoveWorld &world, double elapsedSeconds)
	{
		std::vector<Shared::MoveCommand> batch;
		::EnterCriticalSection(&m_csInput);
		m_inputBudget = (std::min)(m_inputBudget + elapsedSeconds * INPUT_RATE, INPUT_BUDGET_MAX);
		while (!m_inputs.empty() && m_inputBudget >= 1.0)
		{
			batch.push_back(m_inputs.front());
			m_inputs.pop_front();
			m_inputBudget -= 1.0;
		}
		::LeaveCriticalSection(&m_csInput);

		for (size_t i = 0; i < batch.size(); ++i)
		{
			Shared::PlayerMovement::Step(m_move, batch[i], Shared::PlayerMovement::kTickSeconds, world);
			m_lastSeq = batch[i].seq;
		}
		if (!batch.empty())
			StorePosition();
		return (int)batch.size();
	}

	void FillState(Shared::PlayerStatePacket &pkt) const
	{
		pkt = {};
		pkt.header.size = (uint16_t)sizeof(pkt);
		pkt.header.type = Shared::PacketType::PlayerState;
		pkt.lastSeq = m_lastSeq;
		pkt.pos[0] = (float)m_move.position.x;
		pkt.pos[1] = (float)m_move.position.y;
		pkt.pos[2] = (float)m_move.position.z;
		pkt.velocity[0] = (float)m_move.velocity.x;
		pkt.velocity[1] = (float)m_move.velocity.y;
		pkt.velocity[2] = (float)m_move.velocity.z;
		pkt.facing[0] = (float)m_move.facing.x;
		pkt.facing[1] = (float)m_move.facing.y;
		pkt.facing[2] = (float)m_move.facing.z;
		pkt.verticalSpeed = (float)m_move.verticalSpeed;
		pkt.flags = (uint8_t)((m_move.grounded ? Shared::kMoveStateGrounded : 0) |
			(m_move.onObject ? Shared::kMoveStateOnObject : 0) |
			(m_move.sprinting ? Shared::kMoveStateSprinting : 0));
	}

	void FillMove(Shared::PlayerMovePacket &pkt) const
	{
		pkt = {};
		pkt.header.size = (uint16_t)sizeof(pkt);
		pkt.header.type = Shared::PacketType::PlayerMove;
		pkt.playerId = m_nPlayerId;
		pkt.pos[0] = (float)m_move.position.x;
		pkt.pos[1] = (float)m_move.position.y;
		pkt.pos[2] = (float)m_move.position.z;
		pkt.velocity[0] = (float)m_move.velocity.x;
		pkt.velocity[1] = (float)m_move.velocity.y;
		pkt.velocity[2] = (float)m_move.velocity.z;
	}

	UINT32 DroppedInputs() const { return m_nDroppedInputs; }

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
		case Shared::PacketType::PlayerInput:
			if (nSize == (int)sizeof(Shared::PlayerInputPacket))
				OnPlayerInput((const Shared::PlayerInputPacket *)pHead);
			break;

		case Shared::PacketType::PlayerMove:
			//★ B안(2026-10-29) — 클라 좌표는 받지 않는다. 위치는 서버가 입력으로 판정한다(명세 18절 원칙 4).
			if (!m_bWarnedLegacyMove)
			{
				m_bWarnedLegacyMove = true;
				printf("[이동] 플레이어 %u : 좌표 패킷(PlayerMove)은 무시합니다 — 입력(PlayerInput)만 받습니다. 클라를 새로 빌드하세요.\n",
					m_nPlayerId);
			}
			break;

		case Shared::PacketType::PlayerFire:
			if (nSize == (int)sizeof(Shared::PlayerFirePacket))
				OnPlayerFire((const Shared::PlayerFirePacket *)pHead);
			break;

		case Shared::PacketType::JoinWorld:
			if (nSize == (int)sizeof(Shared::JoinWorldPacket))
				OnJoinWorld((const Shared::JoinWorldPacket *)pHead);
			break;

		//총 줍기 · 떨구기(2026-11-15) — 여기서 처리하지 않는다. 요청만 남기고 틱 스레드가 거리를 재고 정한다(HandleWeaponRequests).
		case Shared::PacketType::WeaponPickup:
			if (nSize == (int)sizeof(Shared::WeaponPickupPacket))
			{
				const Shared::WeaponPickupPacket *pPick = (const Shared::WeaponPickupPacket *)pHead;
				RequestPickup(pPick->source, pPick->id);
			}
			break;

		case Shared::PacketType::WeaponDrop:
			if (nSize == (int)sizeof(Shared::WeaponDropPacket))
				RequestDrop();
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
	//  ★ 2026-11-15 든 총으로만 쏜다 — 빈손이면 버린다. 발 간격은 그 총의 WeaponSpec::fireInterval.
	//    연사 예산(ms): 지난 패킷 뒤 흐른 시간만큼 채우고(상한 kFireBurstShots × 간격), 한 발에 간격만큼 쓴다 — 모자라면 버린다.
	//    평균 발 수는 표의 간격을 넘지 못하고, 몰려 온 발은 상한까지 받는다(main.cpp 머리의 kFireBurstShots 주석).
	//    총을 바꿔도 예산은 ms 라 새 총의 상한(kFireBurstShots × 그 총 간격)을 넘지 못한다 — 바꾼 직후 몰아 쏘기도 그 발 수까지.
	void OnPlayerFire(const Shared::PlayerFirePacket *pFire)
	{
		const uint8_t nType = m_heldType.load();
		if (!Shared::IsWeaponType((Shared::WeaponType)nType))
			return;

		const ULONGLONG nNow = ::GetTickCount64();
		const double fIntervalMs = double(Shared::SpecOf((Shared::WeaponType)nType).fireInterval) * 1000.0;
		const double fCapMs = fIntervalMs * kFireBurstShots;

		if (m_nLastFireMs == 0)
			m_fFireBudgetMs = fCapMs;   // 첫 발
		else
			m_fFireBudgetMs = (std::min)(fCapMs, m_fFireBudgetMs + double(nNow - m_nLastFireMs));
		m_nLastFireMs = nNow;

		if (m_fFireBudgetMs < fIntervalMs)
			return;
		m_fFireBudgetMs -= fIntervalMs;
		QueueFire(m_nPlayerId, pFire->origin, pFire->direction, nType);
	}

	/////////////////////////////////////////////////////////////////////
	//  고정 틱 입력 하나 (B안). ★ 여기서 판정하지 않는다 — 검사하고 큐에 넣기만 한다(사격과 같은 규칙).
	//  · 번호가 앞으로만 간다(같거나 작으면 버린다 — 되보내기 · 순서 뒤집기).
	//  · 축은 −1 · 0 · 1, 버튼은 아는 비트만, 카메라 방향은 유한하고 길이가 1 근처여야 한다(정규화해서 쓴다).
	//  · 큐가 INPUT_QUEUE_MAX 를 넘으면 오래된 것부터 버린다 — 조작된 클라가 몰아 보내도 지연 · 메모리가 쌓이지 않는다.
	//  m_nLastQueuedSeq · m_nDroppedInputs 는 이 세션의 워커만 만진다(OnRecv 는 세션마다 한 번에 하나).
	void OnPlayerInput(const Shared::PlayerInputPacket *pInput)
	{
		if (pInput->seq <= m_nLastQueuedSeq) return;

		const float lx = pInput->look[0], ly = pInput->look[1], lz = pInput->look[2];
		if (!std::isfinite(lx) || !std::isfinite(ly) || !std::isfinite(lz)) return;
		const float len = std::sqrt(lx * lx + ly * ly + lz * lz);
		if (len < 0.5f || len > 1.5f) return;

		Shared::MoveCommand cmd;
		cmd.seq = pInput->seq;
		cmd.moveX = (int8_t)(std::max)(-1, (std::min)(1, (int)pInput->moveX));
		cmd.moveZ = (int8_t)(std::max)(-1, (std::min)(1, (int)pInput->moveZ));
		cmd.buttons = (uint8_t)(pInput->buttons &
			(Shared::kMoveButtonSprint | Shared::kMoveButtonJump | Shared::kMoveButtonAim));
		cmd.look[0] = lx / len;
		cmd.look[1] = ly / len;
		cmd.look[2] = lz / len;
		m_nLastQueuedSeq = cmd.seq;

		::EnterCriticalSection(&m_csInput);
		m_inputs.push_back(cmd);
		while (m_inputs.size() > (size_t)INPUT_QUEUE_MAX)
		{
			m_inputs.pop_front();
			++m_nDroppedInputs;
		}
		::LeaveCriticalSection(&m_csInput);
	}

	/////////////////////////////////////////////////////////////////////
	//  맵 번호 (2026-11-01) — 이 맵으로 옮긴다. ★ 여기서 옮기지 않는다 — 요청만 남기고 틱 스레드가 처리한다(HandleWorldJoin).
	void OnJoinWorld(const Shared::JoinWorldPacket *pJoin)
	{
		if (pJoin->worldId >= Shared::kWorldCount)
		{
			printf("[맵] 플레이어 %u : 모르는 맵 번호 %u 는 무시합니다\n", m_nPlayerId, pJoin->worldId);
			return;
		}
		RequestWorld(pJoin->worldId);
	}

	//판정한 위치를 다른 스레드가 읽는 사본(m_lastPos)에 옮긴다 — NPC 추격 · 사격 · 접속자 명단이 이것을 본다.
	void StorePosition()
	{
		m_lastPos[0] = (float)m_move.position.x;
		m_lastPos[1] = (float)m_move.position.y;
		m_lastPos[2] = (float)m_move.position.z;
		m_bHasPos = true;
	}

	SOCKET		m_hSocket;
	UINT32		m_nPlayerId;

	char		m_buffer[RECV_BUFFER_SIZE];
	int			m_nRecvd;			//buffer 에 쌓인 바이트 수

	float		m_lastPos[3];		//서버가 판정한 마지막 위치(m_move 의 사본, 2026-10-29 B안)
	bool		m_bHasPos;			//스폰했는가

	// ── 이동 (B안) ──
	Shared::MoveState					m_move;					//판정한 이동 상태 — 틱 스레드만
	uint32_t							m_lastSeq = 0;			//마지막으로 처리한 입력 번호 — 틱 스레드만
	CRITICAL_SECTION					m_csInput;				//입력 큐 잠금(워커 ↔ 틱 스레드)
	std::deque<Shared::MoveCommand>		m_inputs;
	double								m_inputBudget = 0.0;	//처리해도 되는 입력 수(흐른 시간 × 60) — 잠금 안에서만
	uint32_t							m_nLastQueuedSeq = 0;	//워커만
	UINT32								m_nDroppedInputs = 0;	//큐가 넘쳐 버린 입력 수 — 잠금 안에서만 늘린다
	bool								m_bWarnedLegacyMove = false;

	// ── 맵 번호 (2026-11-01) ──
	std::atomic<uint32_t>				m_world{ Shared::kWorldGameMap };	//지금 맵 — 틱 스레드가 쓴다(접속 스레드는 목록에 넣기 전 InitMove 한 번)
	uint32_t							m_joinWorld = Shared::kNoWorld;		//들어가려는 맵(그 맵을 읽는 중) — 틱 스레드만
	uint32_t							m_requestedWorld = Shared::kNoWorld;	//워커가 받은 JoinWorld — 잠금 안에서만
	bool								m_bHasWorldRequest = false;			//〃

	float		m_fHealth;			//남은 체력. 쓰는 쪽은 행동 스레드 하나
	ULONGLONG	m_nLastFireMs;		//마지막 발사 패킷 시각 (연사 제한). 이 세션의 워커만 만진다
	double		m_fFireBudgetMs;	//연사 예산(ms) — 흐른 시간만큼 차고(상한 kFireBurstShots × 간격) 한 발에 간격만큼 쓴다. 〃

	// ── 총 (2026-11-15 줍기 · 떨구기) ──
	std::atomic<uint8_t>	m_heldType{ (uint8_t)Shared::WeaponType::None };	//든 총 — 틱 스레드가 쓰고 워커(사격 검사)가 읽는다. 접속하면 빈손
	std::atomic<bool>		m_bNeedsWeaponSnapshot{ true };		//그 맵 바닥 총 · 남이 든 총을 보내야 한다 — 접속 직후 true
	bool					m_bHasPickup = false;				//워커가 받은 WeaponPickup — 잠금 안에서만
	uint8_t					m_pickupSource = 0;					//〃
	uint32_t				m_pickupId = 0;						//〃
	bool					m_bHasDrop = false;					//워커가 받은 WeaponDrop — 잠금 안에서만

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

/////////////////////////////////////////////////////////////////////////
//  맵 (2026-11-01 맵 번호) — Shared/World/WorldCatalog.h 의 번호마다 하나.
//
//  0 = 게임 맵(시작할 때 읽는다), 1 ~ 5 = 예시 행성(누가 처음 들어오려 할 때 ThreadLoadWorld 가 따로 읽는다).
//  ★ 다 읽은 맵은 그 뒤로 읽기만 한다 — g_worldState 가 WORLD_READY 가 된 뒤에만 보므로 락이 필요 없다.
//  ★ 맵마다 다른 것은 접지면 · 충돌체 · 스폰 방향뿐이다(모든 맵이 같은 행성 중심 · 반경 — PlanetConst.h).
//  ★ NPC 는 게임 맵에만 있다 — g_npcWorld 는 맵 0 의 지형 · 충돌체를 본다.
struct ServerWorld
{
	Shared::PlanetSurface	surface;		//접지면 — 클라와 같은 맵 glb 의 같은 노드(PlanetSurface::IsGroundObject)
	Shared::TerrainSampler	terrain;
	Shared::CollisionWorld	collision;		//정적 오브젝트 충돌체 — 클라와 같은 묶음(예시 행성은 같은 배치 JSON 을 씌운다)
	Shared::MoveWorld		move;			//이동이 보는 세계 — 클라 PlayerController::World() 와 같은 값이어야 같은 판정이 나온다
	double					spawnUp[3] = { 0.0, 1.0, 0.0 };		//스폰 방향(행성 중심 기준 단위 벡터)

	// ── 총 (2026-11-15 줍기 · 떨구기) — ★ 틱 스레드만 바꾸고 읽는다(시작할 때 진열 자리를 정한 뒤) ──
	bool							hasShowcase = false;		//공중 진열이 있다 — 게임 맵만
	Shared::WeaponShowcaseSlot		showcase[Shared::kWeaponShowcaseSlotCount];	//진열 자리(줍는 출처 — 주워도 그대로)
	struct GroundWeapon
	{
		uint32_t			id;
		Shared::WeaponType	type;
		Shared::WeaponDrop	drop;			//손 → 누운 자세(새로 들어온 사람에게는 누운 자세만)
	};
	std::vector<GroundWeapon>		ground;						//바닥 총 — 떨군 순서(앞이 가장 오래됨)
	uint32_t						nextGroundId = 1;
};

enum : LONG { WORLD_NOT_LOADED = 0, WORLD_LOADING = 1, WORLD_READY = 2, WORLD_FAILED = 3 };

ServerWorld		g_worlds[Shared::kWorldCount];
volatile LONG	g_worldState[Shared::kWorldCount] = {};		//WORLD_* — 바꾸는 쪽 · 읽는 쪽 모두 Interlocked 로

LONG WorldState(uint32_t nWorld)
{
	return nWorld < Shared::kWorldCount ? ::InterlockedCompareExchange(&g_worldState[nWorld], 0, 0) : WORLD_FAILED;
}

/////////////////////////////////////////////////////////////////////////
//  exe 옆 assets\ (끝에 \ 가 붙는다).
//
//  ★ 경로는 exe 옆 assets\ 기준이다. 빌드 후 대상이 맵 파일을 복사해 둔다(Server.vcxproj CopyPlanetSurfaceAsset).
//    작업 디렉터리 기준으로 찾으면 VS 에서 켤 때와 exe 를 직접 켤 때가 갈린다.
std::wstring AssetDir()
{
	wchar_t szExe[MAX_PATH] = { 0 };
	::GetModuleFileNameW(NULL, szExe, MAX_PATH);
	std::wstring dir(szExe);
	const size_t nSlash = dir.find_last_of(L"\\/");
	dir = (nSlash == std::wstring::npos) ? std::wstring() : dir.substr(0, nSlash + 1);
	return dir + L"assets\\";
}

//콘솔(CP949)에 찍을 수 있게 바꾼다 — 오류 메시지 · 경로용.
std::string Narrow(const std::wstring &text)
{
	if (text.empty()) return std::string();
	const int n = ::WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, NULL, 0, NULL, NULL);
	std::string out(n > 0 ? n - 1 : 0, '\0');
	if (n > 1) ::WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, &out[0], n, NULL, NULL);
	return out;
}

/////////////////////////////////////////////////////////////////////////
//  게임 맵(맵 0)의 접지 삼각형을 읽는다 — 클라이언트와 같은 맵(Shared::kPlanetModelAsset).
//
//  ★ 서버는 Shared에서 접지 삼각형만 읽으며 렌더 재질·텍스처를 불러오지 않는다.
//  ★ 스폰 방향은 게임 맵의 Landing L1 상수(kPlanetSpawnUp) — 클라 게임 맵과 같다.
bool LoadTerrain()
{
	ServerWorld &world = g_worlds[Shared::kWorldGameMap];
	const std::wstring path = AssetDir() + Shared::kPlanetModelAsset;

	std::wstring error;
	const bool bLoaded = world.surface.Load(path.c_str(), error);

	if (!bLoaded)
	{
		printf("ERROR: 지형을 읽을 수 없습니다. (%s)\n\t%s\n", Narrow(error).c_str(), Narrow(path).c_str());
		return false;
	}

	world.terrain.Configure(&world.surface, Shared::kPlanetRadius);

	const double length = std::sqrt(Shared::kPlanetSpawnUpX * Shared::kPlanetSpawnUpX +
		Shared::kPlanetSpawnUpY * Shared::kPlanetSpawnUpY + Shared::kPlanetSpawnUpZ * Shared::kPlanetSpawnUpZ);
	world.spawnUp[0] = Shared::kPlanetSpawnUpX / length;
	world.spawnUp[1] = Shared::kPlanetSpawnUpY / length;
	world.spawnUp[2] = Shared::kPlanetSpawnUpZ / length;

	if constexpr (Shared::kUsePlanarMap)
		printf("평면 맵 접지 삼각형 %zu개를 읽었습니다.\n", world.surface.TriangleCount());
	else
		printf("행성 접지 삼각형 %zu개를 읽었습니다. (반지름 %.0fm)\n",
			world.surface.TriangleCount(), Shared::kPlanetRadius);
	return true;
}

/////////////////////////////////////////////////////////////////////////
//  정적 오브젝트 충돌체 (2026-10-29) — 맵 glb 옆 묶음(Resource_Work/_tools/collision/col_build.py). 게임 맵(맵 0) 몫.
//
//  ★ 클라와 같은 파일을 읽는다 — 클라 예측과 서버 판정이 같은 충돌체여야 보정이 생기지 않는다.
//  ★ 못 읽어도 서버는 켠다. 그때는 지형만 막는다(예전처럼) — 로그로 알린다.
//  ★ ClampToGround(클라 좌표를 지면 위로 올리던 검사)는 지웠다. 위치를 서버가 직접 계산하므로 필요 없다.
bool LoadCollision()
{
	ServerWorld &world = g_worlds[Shared::kWorldGameMap];
	const std::wstring dir = AssetDir();
	const std::wstring pack = dir + Shared::kPlanetCollisionAsset;
	const std::wstring manifest = dir + Shared::kPlanetCollisionManifestAsset;
	std::wstring error;
	if (!world.collision.LoadPack(pack.c_str(), manifest.c_str(), error))
	{
		printf("경고: 충돌체를 읽지 못해 오브젝트에 막히지 않습니다 (%s)\n", Narrow(error).c_str());
		return false;
	}
	world.collision.Finalize();
	printf("충돌체 %zu개(메시 %zu종 · 삼각형 %zu)를 읽었습니다.\n",
		world.collision.InstanceCount(), world.collision.MeshCount(), world.collision.MeshTriangleCount());
	return true;
}

/////////////////////////////////////////////////////////////////////////
//  총 진열 6개의 충돌체 (2026-11-15) — 게임 맵(맵 0) 몫. LoadCollision 이 성공한 뒤 · 틱 스레드 전에 한 번.
//
//  ★ 사용자 «충돌체도 같이 적용은 해줘봐». 클라가 스폰 앞 허공에 띄운 총(Engine::BuildWeaponShowcase)과 같은 자리 · 같은 충돌체.
//    이동 충돌은 서버 권위(B안)라 클라와 같이 넣어야 총 근처에서 되감기지 않는다.
//  ★ 충돌체 = 총마다 상자 묶음(WeaponSpec::colliderBoxes — 1인칭 메시를 덮는 상자 3 ~ 6개). 파일을 읽지 않는다(코드 표).
//    2026-11-15 사용자 «총에 부딪힐때 … 프레임이 10까지 떨어져» — 처음 넣은 3인칭 glb COL_(삼각형 5만)에서 바꿨다.
//  ★ 자리 = Shared::LayoutWeaponShowcase(클라와 같은 식). 스폰 자리는 클라 Engine::GameMapSpawnPoint 와 같게
//    «스폰 방향 · kGroundOffset» 으로 넘긴다(높이는 Spawn 이 지형에서 다시 잰다).
//  ★ 2026-11-15 줍기 — 진열 자리를 world.showcase 에 남긴다(주울 때 거리를 잰다). 충돌체는 맵 충돌체가 있을 때만 넣는다.
void SetupWeaponShowcase(ServerWorld &world)
{
	const Shared::Vec3d spawnPoint = Shared::kUsePlanarMap
		? Shared::Vec3d(Shared::kMapSpawnX, 0.0, Shared::kMapSpawnZ)
		: world.move.PositionAt(Shared::Vec3d(world.spawnUp[0], world.spawnUp[1], world.spawnUp[2]), Shared::kGroundOffset);
	Shared::LayoutWeaponShowcase(world.move, spawnPoint, Shared::Vec3d(0.0, 0.0, 1.0), world.showcase);
	world.hasShowcase = true;
	if (!world.move.collision)
	{
		printf("총 진열 6개 자리를 정했습니다(충돌체 없음 — 맵 충돌체를 못 읽었다).\n");
		return;
	}
	const size_t added = Shared::AddWeaponShowcaseColliders(world.collision, world.move, world.showcase);
	world.collision.Finalize();
	printf("총 진열 충돌체 %zu개를 넣었습니다(스폰 앞 %.0f m 허공 · 1인칭 메시를 덮는 상자 묶음).\n", added, Shared::kWeaponShowcaseDistance);
}

/////////////////////////////////////////////////////////////////////////
//  예시 행성 맵 하나를 읽는다 (2026-11-01 맵 번호) — 맵 읽기 스레드(ThreadLoadWorld)에서. 다른 맵은 건드리지 않는다.
//
//  ★ 클라 디버그 행성과 같은 것을 만든다(Client/Engine/Engine.cpp BuildDebugPlanetLoad · PlanetDebugScene::Build).
//    접지면 = 맵 glb 의 IsGroundObject 노드, 스폰 = Landing_L1_Platform 방향(Shared::LoadWorldGround),
//    충돌체 = 묶음 + 배치 JSON 인스턴스(AddPlacement).
//  ★ 배치 JSON 이나 묶음을 못 읽으면 클라처럼 충돌체 없이(지형만 막는다) 둔다 — 클라도 그때는 충돌체 없이 예측한다.
//    지형을 못 읽으면 실패 — 그 맵에 들어오려는 사람에게 WorldJoined(ok = 0).
bool LoadExampleWorld(uint32_t nWorld)
{
	ServerWorld &world = g_worlds[nWorld];
	const Shared::WorldInfo &info = Shared::kWorlds[nWorld];
	const std::wstring dir = AssetDir();
	const ULONGLONG nStart = ::GetTickCount64();

	std::wstring error;
	bool bSpawnFromNode = false;
	const std::wstring mapPath = dir + info.map;
	if (!Shared::LoadWorldGround(mapPath.c_str(), Shared::kWorldSpawnNode, world.surface, world.spawnUp, bSpawnFromNode, error))
	{
		printf("[맵] 맵 %u(%s) 지형을 읽지 못했습니다 (%s)\n\t%s\n",
			nWorld, info.name, Narrow(error).c_str(), Narrow(mapPath).c_str());
		return false;
	}
	world.terrain.Configure(&world.surface, Shared::kPlanetRadius);

	bool bCollision = false;
	Shared::PlacementData placement;
	if (!info.placement || !Shared::ReadPlacement(dir + info.placement, placement))
	{
		printf("[맵] 경고: 맵 %u(%s) 배치 JSON 을 읽지 못해 충돌체 없이 둡니다 (%s)\n",
			nWorld, info.name, Narrow(placement.error).c_str());
	}
	else if (!world.collision.LoadPack((dir + info.collision).c_str(), (dir + info.collisionManifest).c_str(), error))
	{
		printf("[맵] 경고: 맵 %u(%s) 충돌체를 읽지 못해 지형만 막습니다 (%s)\n",
			nWorld, info.name, Narrow(error).c_str());
	}
	else
	{
		world.collision.AddPlacement(Shared::PlacementTypeNames(placement), placement.instances);
		world.collision.Finalize();
		bCollision = true;
	}

	world.move.terrain = &world.terrain;
	world.move.collision = bCollision ? &world.collision : NULL;

	printf("[맵] 맵 %u(%s) 을 읽었습니다 — 접지 삼각형 %zu · 충돌체 %zu개(작은 돌 제외 %zu) · 스폰 %s · %.1fs\n",
		nWorld, info.name, world.surface.TriangleCount(),
		bCollision ? world.collision.InstanceCount() : (size_t)0, bCollision ? world.collision.SkippedSmallRocks() : (size_t)0,
		bSpawnFromNode ? "L1 노드" : "게임 맵 L1 방향(노드 없음)",
		double(::GetTickCount64() - nStart) / 1000.0);
	return true;
}

//맵 읽기 스레드 — 맵 하나에 하나. 다 읽으면 상태만 바꾸고 끝난다(틱 스레드가 다음 틱에 본다).
DWORD WINAPI ThreadLoadWorld(LPVOID pParam)
{
	const uint32_t nWorld = (uint32_t)(UINT_PTR)pParam;
	const bool bOk = LoadExampleWorld(nWorld);
	::InterlockedExchange(&g_worldState[nWorld], bOk ? WORLD_READY : WORLD_FAILED);
	return 0;
}

//맵 상태를 돌려준다. 아직 안 읽었으면 읽기 스레드를 띄운다(틱 스레드가 부른다).
LONG RequestWorldLoad(uint32_t nWorld)
{
	if (nWorld >= Shared::kWorldCount) return WORLD_FAILED;

	const LONG nPrev = ::InterlockedCompareExchange(&g_worldState[nWorld], WORLD_LOADING, WORLD_NOT_LOADED);
	if (nPrev != WORLD_NOT_LOADED) return nPrev;

	printf("[맵] 맵 %u(%s) 을 읽기 시작합니다 — 처음 들어오려는 플레이어가 있습니다\n", nWorld, Shared::WorldName(nWorld));
	HANDLE hThread = ::CreateThread(NULL, 0, ThreadLoadWorld, (LPVOID)(UINT_PTR)nWorld, 0, NULL);
	if (hThread == NULL)
	{
		::InterlockedExchange(&g_worldState[nWorld], WORLD_FAILED);
		return WORLD_FAILED;
	}
	::CloseHandle(hThread);
	return WORLD_LOADING;
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

/////////////////////////////////////////////////////////////////////////
//  이 맵에 있는 사람에게만 보낸다 (2026-11-01 맵 번호). nExceptId 는 제외한다 (0 이면 그 맵 전원).
//  ★ 맵을 바꾸는 중 · 맵 없음인 세션은 어느 맵에도 들지 않으므로 받지 않는다.
void SendToWorld(uint32_t nWorld, const void *pData, int nLen, UINT32 nExceptId)
{
	std::vector<SessionPtr> list = SnapshotSessions();

	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i]->World() != nWorld) continue;
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
void QueueFire(UINT32 nShooterId, const float origin[3], const float dir[3], uint8_t weaponType)
{
	FireRequest req = {};
	req.nShooterId = nShooterId;
	req.weaponType = weaponType;
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
//  시작 위치 — 공통 Landing L1 방향의 실제 지면 위.
//
//  ★ 클라와 같은 스폰 좌표·지형 높이·kGroundOffset을 사용한다.
//    클라가 자기 판단으로 되살아나지 않고 이 좌표를 받아 옮겨간다(서버 권위).
//  ★ 맵마다 따로(2026-11-01 맵 번호) — 그 맵의 지형과 스폰 방향(게임 맵 = kPlanetSpawnUp, 예시 행성 = L1 노드).
void SpawnPoint(const ServerWorld &world, float outPos[3])
{
	if constexpr (Shared::kUsePlanarMap)
	{
		outPos[0] = float(Shared::kMapSpawnX);
		outPos[1] = float(world.terrain.Height(Shared::kMapSpawnX, 0.0, Shared::kMapSpawnZ) + Shared::kGroundOffset);
		outPos[2] = float(Shared::kMapSpawnZ);
		return;
	}
	const double ux = world.spawnUp[0];
	const double uy = world.spawnUp[1];
	const double uz = world.spawnUp[2];
	const double height = world.terrain.Height(ux, uy, uz);
	const double radius = Shared::kPlanetRadius + height + Shared::kGroundOffset;

	outPos[0] = float(Shared::kPlanetCenterX + ux * radius);
	outPos[1] = float(Shared::kPlanetCenterY + uy * radius);
	outPos[2] = float(Shared::kPlanetCenterZ + uz * radius);
}

//체력 · 쓰러짐은 그 맵 사람에게만 알린다(2026-11-01 맵 번호) — 다른 맵 사람은 그 플레이어를 그리지 않는다.
void BroadcastHealth(uint32_t nWorld, UINT32 nPlayerId, UINT32 nAttackerId, float fHealth)
{
	Shared::PlayerHealthPacket pkt = {};
	pkt.header.size = (uint16_t)sizeof(pkt);
	pkt.header.type = Shared::PacketType::PlayerHealth;
	pkt.playerId = nPlayerId;
	pkt.attackerId = nAttackerId;
	pkt.health = fHealth;

	SendToWorld(nWorld, &pkt, (int)sizeof(pkt), 0);
}

void BroadcastNeutralized(uint32_t nWorld, UINT32 nPlayerId, UINT32 nAttackerId,
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

	SendToWorld(nWorld, &pkt, (int)sizeof(pkt), 0);
}

//NPC 는 게임 맵에만 있다(2026-11-01 맵 번호).
void BroadcastNpcDespawn(UINT32 nNpcId)
{
	Shared::NpcDespawnPacket pkt = {};
	pkt.header.size = (uint16_t)sizeof(pkt);
	pkt.header.type = Shared::PacketType::NpcDespawn;
	pkt.npcId = nNpcId;

	SendToWorld(Shared::kWorldGameMap, &pkt, (int)sizeof(pkt), 0);
}

//쏜 사람에게만 — «네 발이 이 NPC 에 맞았다»(2026-11-16 NPC 명중 표시). 클라는 이것을 받아 크로스헤어 둘레에 X 를 띄운다.
void SendNpcHit(const SessionPtr &pShooter, UINT32 nNpcId)
{
	Shared::NpcHitPacket pkt = {};
	pkt.header.size = (uint16_t)sizeof(pkt);
	pkt.header.type = Shared::PacketType::NpcHit;
	pkt.npcId = nNpcId;

	pShooter->Send(&pkt, (int)sizeof(pkt));
}

/////////////////////////////////////////////////////////////////////////
//  한 발을 판정한다. ★ 행동 스레드에서만 부른다.
//
//  순서: 쏜 사람 -> 방향·원점 검사 -> 한 발의 레이(Shared::TraceShot — 몸 · 충돌체 · 지형 중 먼저 닿는 것) -> 피해.
//  클라는 «누가 맞았는지» 를 말하지 않는다 (명세 18절 원칙 4).
//  ★ 쏜 사람의 맵 안에서만 판정한다(2026-11-01 맵 번호) — 대상 = 같은 맵 플레이어(+ 게임 맵이면 NPC), 가림 = 그 맵 지형.
void ResolveFire(const FireRequest &req, const std::vector<SessionPtr> &list)
{
	//1) 쏜 사람. 그 사이 나갔거나 맵을 바꾸는 중이면 버린다.
	SessionPtr pShooter;
	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i]->PlayerId() == req.nShooterId) { pShooter = list[i]; break; }
	}
	if (!pShooter || !pShooter->HasPos() || !pShooter->InWorld()) return;
	const uint32_t nWorld = pShooter->World();
	const ServerWorld &world = g_worlds[nWorld];

	//쏜 총(2026-11-15) — 피해 · 사거리는 그 총의 표. 빈손이면 큐에 들어오지 않았다(OnPlayerFire).
	const Shared::WeaponType weaponType = (Shared::WeaponType)req.weaponType;
	if (!Shared::IsWeaponType(weaponType)) return;
	const Shared::WeaponSpec &spec = Shared::SpecOf(weaponType);

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

	//4) 한 발의 레이 — Shared::TraceShot(클라 크로스헤어 겨눈 점과 같은 함수, 2026-11-15 사용자 «벽에 막히는건 당연히 막혀야지»).
	//   대상 = 같은 맵의 다른 플레이어(+ 게임 맵이면 살아 있는 NPC) — 몸통 중심의 구(kHitRadius).
	//   가림 = 그 맵의 충돌체(건물 · 바위 · 소품 · 나무 · 총 진열 상자)와 지형. 가장 먼저 닿는 것이 몸이면 명중, 아니면 막힘.
	//   지형은 0.5 m 깊이까지 봐준다(예전 TerrainBlocks 의 여유와 같다 — 땅을 스치듯 지나는 발).
	//   ★ 예전(~11-15): 가장 가까운 몸을 고른 뒤 지형만 봤다 — 건물 · 바위를 총알이 지나갔다(노션 「사격 시스템 설명」 4.2 의 1).
	std::vector<Shared::ShotTarget> targets;
	targets.reserve(list.size() + (nWorld == Shared::kWorldGameMap ? g_npcWorld.Count() : 0));
	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i]->PlayerId() == req.nShooterId) continue;
		if (!list[i]->HasPos() || list[i]->World() != nWorld) continue;

		float target[3];
		list[i]->LastPos(target);
		targets.push_back({ Shared::Vec3d{ target[0], target[1], target[2] }, list[i]->PlayerId() });
	}
	const size_t nPlayerTargets = targets.size();   // 앞쪽 = 플레이어, 뒤쪽 = NPC

	if (nWorld == Shared::kWorldGameMap)	//NPC 는 게임 맵에만 있다 — 쓰러진 NPC 는 Snapshot 에서 빠진다
	{
		std::vector<srv::NpcView> npcs;
		g_npcWorld.Snapshot(npcs);
		for (const srv::NpcView &npc : npcs)
			targets.push_back({ Shared::Vec3d{ npc.pos.x, npc.pos.y, npc.pos.z }, npc.npcId });
	}

	Shared::ShotOptions options;
	options.terrainTolerance = Shared::kShotServerTerrainTolerance;
	const Shared::ShotHit hit = Shared::TraceShot(world.move,
		Shared::Vec3d{ origin.x, origin.y, origin.z }, Shared::Vec3d{ dir.x, dir.y, dir.z }, spec.range,
		targets.data(), targets.size(), options);

	//5) 몸보다 먼저 충돌체 · 지형에 닿으면 막힌다.
	const float fBest = (float)hit.distance;
	if (hit.kind == Shared::ShotHitKind::Collider)
	{
		printf("[사격] 플레이어 %u : 충돌체(건물 · 바위 등)에 막혔습니다 (%.1fm)\n", req.nShooterId, fBest);
		return;
	}
	if (hit.kind == Shared::ShotHitKind::Terrain)
	{
		printf("[사격] 플레이어 %u : 지형에 막혔습니다 (%.1fm)\n", req.nShooterId, fBest);
		return;
	}
	if (hit.kind != Shared::ShotHitKind::Body) return;   // 사거리 안에 아무것도 없다

	const UINT32 nHitPlayer = hit.target < nPlayerTargets ? targets[hit.target].id : 0;
	const UINT32 nHitNpc = hit.target < nPlayerTargets ? 0 : targets[hit.target].id;

	//6) 피해.
	if (nHitNpc != 0)
	{
		//★ 2026-11-16 사용자 «플레이어가 쏜 총이 NPC를 맞추면 x표시로 크로스헤어 주변에 표시해줘» — 쏜 사람에게 알린다(쓰러뜨린 발도).
		SendNpcHit(pShooter, nHitNpc);
		if (g_npcWorld.Damage(nHitNpc, spec.damage))
			printf("[명중] 플레이어 %u -> NPC %u 쓰러짐 (%.1fm)\n", req.nShooterId, nHitNpc, fBest);
		else
			printf("[명중] 플레이어 %u -> NPC %u (%.1fm) · %d틱 멈춤\n", req.nShooterId, nHitNpc, fBest, srv::kNpcHitStunTicks);
		return;
	}

	SessionPtr pVictim;
	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i]->PlayerId() == nHitPlayer) { pVictim = list[i]; break; }
	}
	if (!pVictim) return;

	const float fLeft = Shared::GameLogic::ApplyDamage(pVictim->Health(), spec.damage);
	pVictim->SetHealth(fLeft);
	BroadcastHealth(nWorld, nHitPlayer, req.nShooterId, fLeft);

	printf("[명중] 플레이어 %u -> 플레이어 %u : 남은 체력 %.0f (%.1fm)\n",
		req.nShooterId, nHitPlayer, fLeft, fBest);

	if (!Shared::GameLogic::IsDown(fLeft)) return;

	//★ 쓰러진 상태가 따로 없다 — 교수님 지시대로 시작 위치로 옮기고 체력을 채운다.
	//  그 맵의 시작 위치다(2026-11-01 맵 번호).
	float spawn[3];
	SpawnPoint(world, spawn);

	pVictim->SetHealth(Shared::kMaxHealth);
	pVictim->Respawn(world.move, spawn);
	BroadcastNeutralized(nWorld, nHitPlayer, req.nShooterId, spawn, Shared::kMaxHealth);

	printf("[무력화] 플레이어 %u 가 시작 위치로 돌아갑니다 (가해 %u)\n",
		nHitPlayer, req.nShooterId);
}

/////////////////////////////////////////////////////////////////////////
//  총 줍기 · 떨구기 (2026-11-15, Shared/GameLogic/WeaponPickup.h) — ★ 틱 스레드에서만 부른다.
//
//  든 총 = 세션(m_heldType), 바닥 총 = 맵마다 ServerWorld::ground. 바뀌면 같은 맵 전원에게
//  WeaponEquip · GroundWeaponSpawn · GroundWeaponRemove 로 알린다. 그 맵에 새로 들어온 사람에게는 SendWeaponSnapshot.

void FillPose(const Shared::WeaponPose &pose, float pos[3], float axisY[3], float axisZ[3])
{
	pos[0] = (float)pose.position.x;	pos[1] = (float)pose.position.y;	pos[2] = (float)pose.position.z;
	axisY[0] = (float)pose.axisY.x;		axisY[1] = (float)pose.axisY.y;		axisY[2] = (float)pose.axisY.z;
	axisZ[0] = (float)pose.axisZ.x;		axisZ[1] = (float)pose.axisZ.y;		axisZ[2] = (float)pose.axisZ.z;
}

Shared::WeaponEquipPacket MakeEquip(UINT32 nPlayerId, Shared::WeaponType type)
{
	Shared::WeaponEquipPacket pkt = {};
	pkt.header.size = (uint16_t)sizeof(pkt);
	pkt.header.type = Shared::PacketType::WeaponEquip;
	pkt.playerId = nPlayerId;
	pkt.weaponType = (uint8_t)type;
	return pkt;
}

//bLanded = 이미 누워 있다(새로 들어온 사람에게) — 시작 자세 = 누운 자세, 떨어지는 시간 0.
Shared::GroundWeaponSpawnPacket MakeGroundSpawn(const ServerWorld::GroundWeapon &g, bool bLanded)
{
	Shared::GroundWeaponSpawnPacket pkt = {};
	pkt.header.size = (uint16_t)sizeof(pkt);
	pkt.header.type = Shared::PacketType::GroundWeaponSpawn;
	pkt.groundId = g.id;
	pkt.weaponType = (uint8_t)g.type;
	FillPose(bLanded ? g.drop.rest : g.drop.start, pkt.startPos, pkt.startAxisY, pkt.startAxisZ);
	FillPose(g.drop.rest, pkt.restPos, pkt.restAxisY, pkt.restAxisZ);
	pkt.fallSeconds = bLanded ? 0.0f : g.drop.fallSeconds;
	return pkt;
}

void BroadcastGroundRemove(uint32_t nWorld, uint32_t nGroundId)
{
	Shared::GroundWeaponRemovePacket pkt = {};
	pkt.header.size = (uint16_t)sizeof(pkt);
	pkt.header.type = Shared::PacketType::GroundWeaponRemove;
	pkt.groundId = nGroundId;
	SendToWorld(nWorld, &pkt, (int)sizeof(pkt), 0);
}

//든 총을 손 자리에서 떨군다 — 바닥 총 하나를 만들고 같은 맵에 알린다. 상한을 넘으면 가장 오래된 바닥 총을 지운다.
void DropHeldWeapon(const SessionPtr &pSession, uint32_t nWorld)
{
	const Shared::WeaponType held = pSession->HeldWeapon();
	if (!Shared::IsWeaponType(held)) return;

	ServerWorld &world = g_worlds[nWorld];
	const Shared::MoveState &move = pSession->Move();

	ServerWorld::GroundWeapon g;
	g.id = world.nextGroundId++;
	g.type = held;
	g.drop = Shared::SettleDroppedWeapon(world.move, held, move.position, move.up, move.facing);
	world.ground.push_back(g);

	while (world.ground.size() > Shared::kMaxGroundWeapons)
	{
		BroadcastGroundRemove(nWorld, world.ground.front().id);
		world.ground.erase(world.ground.begin());
	}

	const Shared::GroundWeaponSpawnPacket spawn = MakeGroundSpawn(g, false);
	SendToWorld(nWorld, &spawn, (int)sizeof(spawn), 0);

	pSession->SetHeldWeapon(Shared::WeaponType::None);
	printf("[총] 플레이어 %u : %s 떨굼 (바닥 총 %u, 맵 %u, %.2fs 떨어짐)\n",
		pSession->PlayerId(), Shared::SpecOf(held).name, g.id, nWorld, g.drop.fallSeconds);
}

//워커가 남긴 줍기 · 떨구기 요청을 처리한다. 같은 틱에 둘 다 왔으면 떨구기 → 줍기 순서.
void HandleWeaponRequests(const SessionPtr &pSession)
{
	bool bPickup = false, bDrop = false;
	uint8_t source = 0;
	uint32_t id = 0;
	pSession->TakeWeaponRequests(bPickup, source, id, bDrop);
	if (!bPickup && !bDrop) return;
	if (!pSession->InWorld() || !pSession->HasPos()) return;

	const uint32_t nWorld = pSession->World();
	ServerWorld &world = g_worlds[nWorld];
	const UINT32 nPlayerId = pSession->PlayerId();

	if (bDrop && Shared::IsWeaponType(pSession->HeldWeapon()))
	{
		DropHeldWeapon(pSession, nWorld);
		const Shared::WeaponEquipPacket equip = MakeEquip(nPlayerId, Shared::WeaponType::None);
		SendToWorld(nWorld, &equip, (int)sizeof(equip), 0);
	}

	if (!bPickup) return;

	//줍는 총 — 진열(게임 맵) 또는 바닥 총
	Shared::WeaponType type = Shared::WeaponType::None;
	Shared::Vec3d where{};
	size_t nGroundIndex = (size_t)-1;
	if (source == Shared::kPickupFromShowcase)
	{
		if (!world.hasShowcase || id >= Shared::kWeaponShowcaseSlotCount) return;
		type = world.showcase[id].type;
		where = world.showcase[id].position;
	}
	else if (source == Shared::kPickupFromGround)
	{
		for (size_t i = 0; i < world.ground.size(); ++i)
		{
			if (world.ground[i].id != id) continue;
			nGroundIndex = i;
			type = world.ground[i].type;
			where = world.ground[i].drop.rest.position;
			break;
		}
		if (nGroundIndex == (size_t)-1) return;		//그 사이 누가 주웠다
	}
	if (!Shared::IsWeaponType(type)) return;

	//거리 — 서버가 아는 위치에서 다시 잰다(클라 예측과 조금 다를 수 있어 여유를 둔다)
	const Shared::Vec3d body = pSession->Move().position;
	const double fDist = Shared::Length(body - where);
	if (fDist > Shared::kWeaponPickupRange + Shared::kWeaponPickupSlack)
	{
		printf("[총] 플레이어 %u : %s 이(가) %.1fm 떨어져 있어 줍지 못합니다\n", nPlayerId, Shared::SpecOf(type).name, fDist);
		return;
	}

	//이미 들고 있으면 그 총을 떨군 뒤 바꿔 든다(한 번에 한 자루 — 2026-11-14 사용자).
	if (Shared::IsWeaponType(pSession->HeldWeapon()))
		DropHeldWeapon(pSession, nWorld);

	if (nGroundIndex != (size_t)-1)
	{
		//떨군 총이 상한 때문에 지워졌을 수 있다 — 번호로 다시 찾는다
		for (size_t i = 0; i < world.ground.size(); ++i)
		{
			if (world.ground[i].id != id) continue;
			world.ground.erase(world.ground.begin() + (ptrdiff_t)i);
			BroadcastGroundRemove(nWorld, id);
			break;
		}
	}

	pSession->SetHeldWeapon(type);
	const Shared::WeaponEquipPacket equip = MakeEquip(nPlayerId, type);
	SendToWorld(nWorld, &equip, (int)sizeof(equip), 0);
	printf("[총] 플레이어 %u : %s 주움 (%s, %.1fm)\n", nPlayerId, Shared::SpecOf(type).name,
		source == Shared::kPickupFromShowcase ? "공중 진열" : "바닥", fDist);
}

//그 맵에 새로 들어온 사람에게 — 바닥 총 전부(누운 자세) · 다른 사람이 든 총 · 자기 총.
void SendWeaponSnapshot(const SessionPtr &pSession, const std::vector<SessionPtr> &list)
{
	if (!pSession->InWorld()) return;
	const uint32_t nWorld = pSession->World();
	const ServerWorld &world = g_worlds[nWorld];

	for (size_t i = 0; i < world.ground.size(); ++i)
	{
		const Shared::GroundWeaponSpawnPacket spawn = MakeGroundSpawn(world.ground[i], true);
		pSession->Send(&spawn, (int)sizeof(spawn));
	}

	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i]->World() != nWorld) continue;
		const Shared::WeaponType held = list[i]->HeldWeapon();
		if (list[i]->PlayerId() != pSession->PlayerId() && !Shared::IsWeaponType(held)) continue;
		const Shared::WeaponEquipPacket equip = MakeEquip(list[i]->PlayerId(), held);
		pSession->Send(&equip, (int)sizeof(equip));
	}
}

/////////////////////////////////////////////////////////////////////////
//  맵 바꾸기 (2026-11-01 맵 번호, 사용자 «맵번호 적용해줘») — ★ 틱 스레드에서만 부른다.
//
//  ① 워커가 받은 JoinWorld 가 있으면 지금 맵에서 뺀다 — 옛 맵 사람들에게 PlayerLeave(화면에서 지우게).
//     그 뒤로는 판정 · 송신 · 사격 · NPC 추격 대상에서 빠진다.
//  ② 그 맵이 준비됐으면(처음이면 ThreadLoadWorld 가 읽는 동안 틱마다 다시 본다) 스폰 자리에 세우고
//     WorldJoined(ok, 자리 · 방향) + 체력을 보낸다.
//     ★ WorldJoined 를 이 맵의 다른 패킷보다 먼저 보낸다 — 클라는 이 패킷 뒤의 것만 새 맵 것으로 받는다
//       (TCP 순서. 맵 패킷은 모두 이 스레드에서 보낸다).
//  ③ 준비하지 못했으면 WorldJoined(ok = 0) — 어느 맵에도 없는 채로 둔다(클라는 그 맵에서 혼자 움직인다).
void HandleWorldJoin(const SessionPtr &pSession)
{
	uint32_t nRequested = Shared::kNoWorld;
	if (pSession->TakeWorldRequest(nRequested))
	{
		const uint32_t nOld = pSession->World();
		pSession->BeginJoin(nRequested);
		if (nOld < Shared::kWorldCount)
		{
			Shared::PlayerLeavePacket leave = {};
			leave.header.size = (uint16_t)sizeof(leave);
			leave.header.type = Shared::PacketType::PlayerLeave;
			leave.playerId = pSession->PlayerId();
			SendToWorld(nOld, &leave, (int)sizeof(leave), pSession->PlayerId());
		}
		printf("[맵] 플레이어 %u : 맵 %u(%s) -> %u(%s)\n", pSession->PlayerId(),
			nOld, Shared::WorldName(nOld), nRequested, Shared::WorldName(nRequested));
	}

	const uint32_t nTarget = pSession->JoiningWorld();
	if (nTarget >= Shared::kWorldCount) return;

	const LONG nState = RequestWorldLoad(nTarget);
	if (nState == WORLD_LOADING) return;		//아직 읽는 중 — 다음 틱에 다시 본다

	Shared::WorldJoinedPacket joined = {};
	joined.header.size = (uint16_t)sizeof(joined);
	joined.header.type = Shared::PacketType::WorldJoined;
	joined.worldId = nTarget;

	if (nState != WORLD_READY)
	{
		pSession->FailJoin();
		joined.ok = 0;
		pSession->Send(&joined, (int)sizeof(joined));
		printf("[맵] 플레이어 %u : 맵 %u(%s) 을 준비하지 못해 어느 맵에도 넣지 않았습니다\n",
			pSession->PlayerId(), nTarget, Shared::WorldName(nTarget));
		return;
	}

	const ServerWorld &world = g_worlds[nTarget];
	float spawn[3];
	SpawnPoint(world, spawn);
	pSession->EnterWorld(nTarget, world.move, spawn);

	joined.ok = 1;
	pSession->LastPos(joined.pos);
	pSession->Facing(joined.facing);
	pSession->Send(&joined, (int)sizeof(joined));

	//체력 — 클라는 들어가는 중에 온 옛 맵의 쓰러짐 알림을 버렸으므로 지금 값을 다시 알린다.
	Shared::PlayerHealthPacket health = {};
	health.header.size = (uint16_t)sizeof(health);
	health.header.type = Shared::PacketType::PlayerHealth;
	health.playerId = pSession->PlayerId();
	health.attackerId = 0;			//피해가 아니라 알림
	health.health = pSession->Health();
	pSession->Send(&health, (int)sizeof(health));

	//총(2026-11-15) — 든 총은 맵을 바꿔도 들고 간다. 이 맵 사람들에게 알리고, 이 사람에게는 이 맵의 바닥 총 · 남이 든 총을 보낸다(틱 루프).
	if (Shared::IsWeaponType(pSession->HeldWeapon()))
	{
		const Shared::WeaponEquipPacket equip = MakeEquip(pSession->PlayerId(), pSession->HeldWeapon());
		SendToWorld(nTarget, &equip, (int)sizeof(equip), pSession->PlayerId());
	}
	pSession->MarkWeaponSnapshot();

	printf("[맵] 플레이어 %u : 맵 %u(%s) 에 들어왔습니다\n", pSession->PlayerId(), nTarget, Shared::WorldName(nTarget));
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
	int nInputsThisSecond = 0;

	//실제로 흐른 시간 — Sleep(33) 은 타이머 해상도 때문에 더 길게 잘 수 있다. 입력 예산은 흐른 시간으로 준다.
	LARGE_INTEGER freq = {}, last = {};
	::QueryPerformanceFrequency(&freq);
	::QueryPerformanceCounter(&last);

	puts("[NPC 행동 스레드 시작]");

	while (::InterlockedCompareExchange(&g_bRunning, 1, 1) == 1)
	{
		::Sleep(dwIntervalMs);

		LARGE_INTEGER now = {};
		::QueryPerformanceCounter(&now);
		const double elapsed = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
		last = now;

		//0) 플레이어 이동 (B안, 2026-10-29) — 받은 입력을 같은 이동 코드 · 같은 충돌체로 판정한다.
		//   ★ 판정은 이 스레드 한 곳에서만 한다(사격과 같은 규칙, 사용자 결정 5번). 워커는 큐에 넣기만 했다.
		//   당사자 = PlayerState(마지막 입력 번호 + 상태, 클라가 어긋나면 되감는다), 다른 사람 = PlayerMove(위치).
		//   ★ 맵 번호 (2026-11-01) — 그 사람이 있는 맵의 지형 · 충돌체로 판정하고, 같은 맵 사람에게만 보낸다.
		//     맵을 바꾸는 중 · 맵 없음인 세션은 판정하지 않고 아무것도 받지 않는다.
		{
			std::vector<SessionPtr> list = SnapshotSessions();

			//0-1) 맵 바꾸기 — 판정보다 먼저. 워커가 받은 JoinWorld 를 여기서 처리한다(HandleWorldJoin).
			for (size_t i = 0; i < list.size(); ++i)
				HandleWorldJoin(list[i]);

			for (size_t i = 0; i < list.size(); ++i)
			{
				if (!list[i]->InWorld()) continue;
				nInputsThisSecond += list[i]->Simulate(g_worlds[list[i]->World()].move, elapsed);
			}

			//0-2) 총 (2026-11-15) — 그 맵에 새로 들어온 사람에게 바닥 총 · 든 총을 보내고, 줍기 · 떨구기 요청을 처리한다.
			//     ★ 이동 판정 뒤 — 떨군 총의 손 자리 · 줍기 거리를 이번 틱 위치로 잰다.
			for (size_t i = 0; i < list.size(); ++i)
			{
				if (!list[i]->InWorld()) continue;
				if (list[i]->TakeWeaponSnapshot())
					SendWeaponSnapshot(list[i], list);
				HandleWeaponRequests(list[i]);
			}

			for (size_t i = 0; i < list.size(); ++i)
			{
				if (!list[i]->HasPos() || !list[i]->InWorld()) continue;
				const uint32_t nWorld = list[i]->World();

				Shared::PlayerStatePacket state;
				list[i]->FillState(state);
				list[i]->Send(&state, (int)sizeof(state));

				Shared::PlayerMovePacket move;
				list[i]->FillMove(move);
				for (size_t j = 0; j < list.size(); ++j)
				{
					if (j == i || list[j]->World() != nWorld) continue;
					list[j]->Send(&move, (int)sizeof(move));
				}
			}
		}

		//1) 스폰한 플레이어를 모은다(접속하면 서버가 바로 스폰한다 — B안).
		//   NPC 는 게임 맵에만 있으므로 게임 맵에 있는 사람만 쫓는다(2026-11-01 맵 번호).
		players.clear();
		{
			std::vector<SessionPtr> list = SnapshotSessions();

			for (size_t i = 0; i < list.size(); ++i)
			{
				if (!list[i]->HasPos() || list[i]->World() != Shared::kWorldGameMap) continue;

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

		//2) 첫 틱에 한 번 — 시작 지점(SpawnPoint, Landing L1 지면)에서 떨어진 띠에 무리지어 깐다(2026-10-29).
		//   자리가 플레이어 위치에 매이지 않으므로 첫 플레이어를 기다리지 않는다. 값은 NpcWorld.cpp 상단.
		if (g_npcWorld.Empty())
		{
			float spawn[3];
			SpawnPoint(g_worlds[Shared::kWorldGameMap], spawn);
			g_npcWorld.SpawnGroups(Shared::Vec3{ spawn[0], spawn[1], spawn[2] }, srv::kNpcSpawnCount);
			printf("NPC %zu 마리를 무리 %zu개로 시작 지점 둘레에 배치했습니다. (빈자리를 못 찾고 세운 자리 %d)\n",
				g_npcWorld.Count(), g_npcWorld.GroupCount(), g_npcWorld.BlockedPlacements());
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

		//4) 게임 맵에 있는 사람에게 뿌린다(2026-11-01 맵 번호 — 다른 맵 사람에게는 보내지 않는다). PlayerMove 와 같은 방식이다.
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

			SendToWorld(Shared::kWorldGameMap, &pkt, (int)sizeof(pkt), 0);
		}

		//1초에 한 번만 상태를 찍는다. 매 틱 찍으면 좌표 로그에 묻힌다.
		if (++nTicks >= 30)
		{
			nTicks = 0;
			if (!views.empty())
			{
				printf("[NPC] %zu 마리 / 추격 %d 마리 / 맞아서 멈춤 %d 마리 (플레이어 %zu명)\n",
					views.size(), g_npcWorld.ChasingCount(), g_npcWorld.StunnedCount(), players.size());
			}
			//이동 판정 계측(2026-10-29) — 초당 처리한 입력 수(1명당 약 60이 정상) · 넘쳐 버린 입력 누계
			//  2026-11-01 맵 번호 — 맵별 인원(맵 번호:인원, 맵 없음 = 바꾸는 중 · 실패)과 그 맵 충돌체 유무를 함께 찍는다.
			{
				std::vector<SessionPtr> list = SnapshotSessions();
				if (!list.empty())
				{
					UINT32 nDropped = 0;
					size_t nInWorld[Shared::kWorldCount] = {};
					size_t nNoWorld = 0;
					for (size_t i = 0; i < list.size(); ++i)
					{
						nDropped += list[i]->DroppedInputs();
						if (list[i]->InWorld()) ++nInWorld[list[i]->World()];
						else ++nNoWorld;
					}
					std::string perWorld;
					for (uint32_t w = 0; w < Shared::kWorldCount; ++w)
					{
						if (nInWorld[w] == 0) continue;
						char szItem[64] = { 0 };
						sprintf_s(szItem, " %u:%zu명(충돌체 %s)", w, nInWorld[w],
							g_worlds[w].move.collision ? "켬" : "없음");
						perWorld += szItem;
					}
					printf("[이동] 플레이어 %zu명 · 입력 %d개/초 · 버린 입력 누계 %u · 맵%s · 맵 바꾸는 중/없음 %zu명\n",
						list.size(), nInputsThisSecond, nDropped, perWorld.c_str(), nNoWorld);
				}
			}
			nInputsThisSecond = 0;
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

		//★ 서버가 스폰한다(B안) — 목록에 넣기 전에. 클라도 같은 자리(Landing L1 지면)에 같은 계산으로 선다.
		//  접속하면 게임 맵에 선다(2026-11-01 맵 번호) — 클라도 접속 직후 게임 맵에 있다.
		{
			const ServerWorld &gameWorld = g_worlds[Shared::kWorldGameMap];
			float spawn[3];
			SpawnPoint(gameWorld, spawn);
			pNewUser->InitMove(Shared::kWorldGameMap, gameWorld.move, spawn);
		}

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
		//   게임 맵에 있는 사람만(2026-11-01 맵 번호) — 예시 행성에 간 사람은 이 맵에 없다.
		{
			std::vector<SessionPtr> list = SnapshotSessions();
			int nSentRoster = 0;

			for (size_t i = 0; i < list.size(); ++i)
			{
				if (list[i]->PlayerId() == nPlayerId) continue;
				if (!list[i]->HasPos() || list[i]->World() != Shared::kWorldGameMap) continue;

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
	ServerWorld &gameWorld = g_worlds[Shared::kWorldGameMap];
	g_npcWorld.SetTerrain(&gameWorld.terrain);

	//이동 판정 세계 (B안, 2026-10-29) — 충돌체는 없어도 켠다(지형만 막는다).
	gameWorld.move.terrain = &gameWorld.terrain;
	gameWorld.move.collision = LoadCollision() ? &gameWorld.collision : NULL;
	//총 진열 6개(2026-11-15) — 클라와 같은 자리. 줍기 출처라 자리는 늘 정하고, 충돌체는 맵 충돌체가 있을 때만 넣는다
	//(없으면 클라도 없이 예측한다).
	SetupWeaponShowcase(gameWorld);
	//NPC 스폰 자리도 같은 충돌체를 피한다(2026-10-29). 없으면 지형만 본다.
	g_npcWorld.SetCollision(gameWorld.move.collision);
	//게임 맵은 다 읽었다(2026-11-01 맵 번호). 예시 행성(1 ~ 5)은 누가 처음 들어오려 할 때 읽는다(RequestWorldLoad).
	::InterlockedExchange(&g_worldState[Shared::kWorldGameMap], WORLD_READY);

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

	//NPC 행동 스레드. 첫 틱에 시작 지점 둘레에 NPC 를 무리지어 깔고 굴린다.
	hThread = ::CreateThread(NULL, 0, ThreadNpcTick,
				(LPVOID)NULL, 0, &dwThreadID);
	::CloseHandle(hThread);

	printf("*** 좌표서버를 시작합니다! (포트 %d) ***\n", SERVER_PORT);
	printf("    좌표 패킷 크기 = %d 바이트 / NPC 패킷 크기 = %d 바이트\n",
		(int)sizeof(Shared::PlayerMovePacket),
		(int)sizeof(Shared::NpcStatePacket));
	printf("    NPC %d 마리(무리당 %d)를 30Hz 로 굴려 전원에게 뿌립니다.\n", srv::kNpcSpawnCount, srv::kNpcGroupSize);
	printf("    플레이어 이동은 입력(%d 바이트, 초당 60개)으로 받아 서버가 판정합니다 — 상태 %d 바이트를 당사자에게 30Hz.\n",
		(int)sizeof(Shared::PlayerInputPacket), (int)sizeof(Shared::PlayerStatePacket));
	while (1)
		getchar();

	return 0;
}
