#pragma once

// ★ Network.h 를 맨 앞에 둘 것
//   <winsock2.h> 가 windows.h 보다 먼저 와야 winsock 1.1 과 구조체가 충돌하지 않는다.
//   (자세한 이유는 Network.h 주석)
#include "Client/Net/Network.h"

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "Client/GRenderer.h"
#include "Client/Sound/SoundManager.h"
#include "Client/Resource/ResourceManager.h"
#include "Client/Scene/SceneManager.h"
#include "Client/Bridge/ClassBridge.h"
#include "Client/State/StateManager.h"
#include "Client/InputManager.h"
#include "Shared/Physics/PhysicsCalculator.h"
#include "Client/Animation/Animator.h"
#include "Client/GameTimer.h"
#include "Client/LegacyScene.h"
#include "Client/Camera.h"
#include "Client/PlayerController.h"
#include "Client/Planet.h"
#include "Shared/Terrain/TerrainSampler.h"

// ============================================================
//  Client/Engine/Engine.h — 아키텍처 명세서 1절 · 2절
//
//  엔진의 최상위 계층. 클라이언트 전체를 감싼다.
//  명세 1절 «전체 구조» 의 하위 시스템을 그 순서대로 소유하고,
//  초기화 · 프레임 실행 순서 · 종료(생명주기)를 관리한다.
//
//  ★ 실행 위치 — 메인 스레드 (「멀티스레딩 분류 명세서」 5장)
//    창 메시지 펌프와 프레임 루프를 여기서 돈다. 시작·종료 순서는 9.1 을 따른다.
//  ★ 명세 2절 — Engine 은 게임 로직을 직접 수행하지 않는다.
//    그 자리를 맡을 Class Bridge · State Manager · Scene 의 GameObject 가 아직 선언만 있어서,
//    main.cpp 에 있던 게임 코드를 일단 그대로 옮겨 왔다.
//    아래 «아직 자리로 옮기지 않은 것» 과 Engine.cpp 의 UpdatePlayer · UpdateNetwork 가 그것이다.
// ============================================================

namespace swc {

	class Engine
	{
	public:
		Engine();
		~Engine();

		bool Initialize(HINSTANCE, int showCommand);
		void Run();
		void Shutdown();

	private:
		bool PumpMessages();
		void HandleSystemKeys();
		void UpdatePlayer(float);
		void UpdateFire(float);
		void SpawnTracer(const Vec3d& muzzle, const Vec3d& aim);
		void UpdateNetwork(float);
		void RenderFrame();
		void UpdateTitle(float);

		HWND hwnd = nullptr;

		// ── 명세 1절 «전체 구조» ────────────────────────────────
		//  선언 순서 = 문서 순서. Renderer 가 맨 앞이라 소멸은 맨 마지막이다(GPU 자원을 마지막에 푼다).
		GRenderer                 renderer;       // Renderer (3절)
		SoundManager              soundManager;   // Sound Manager (15절) — 선언만
		ResourceManager           resources;      // Resource Manager (4절)
		SceneManager              sceneManager;   // Scene Manager (5절) — 선언만. 지금 씬은 아래 LegacyScene
		ClassBridge               classBridge;    // Game Logic └ Class Bridge (8·9절) — 선언만
		                                          //   Game Logic 본체(Shared::GameLogic)는 지금 서버만 쓴다 (2026-09-18 결정)

		Network                   network;        // Network (10절). 스레드 분리는 멀티스레딩 10장 M4
		std::wstring              netStatus;
		float                     sendAccumulator = 0.0f;
		std::vector<RemoteView>   remoteViews;
		std::vector<NpcView>      npcViews;

		StateManager              stateManager;   // State Manager (11절) — 선언만
		InputManager              input;          // Input Manager (12절)
		Shared::PhysicsCalculator physics;        // Physics / Math Calculator (13절) — 선언만
		Animator                  animator;       // Animator (14절) — 선언만
		GameTimer                 timer;          // Game Timer (16절)

		// ── 아직 자리로 옮기지 않은 것 ──────────────────────────
		Planet                 planet;            // 맵 정보 → Scene 이 가진다 (2026-09-18 결정)
		Shared::TerrainSampler terrain;
		std::wstring           terrainStatus;
		LegacyScene            scene;             // → Scene 노드 + Render World 로 나뉜다 (LegacyScene.h)
		Camera                 camera;            // 명세 1절에 자리가 없다
		PlayerController       controller;        // → Player(6.6) · Game Logic(8절). 입력은 Class Bridge 경유(6.6.3)
		NodeHandle             player = kInvalidNode;
		MeshHandle             cubeMesh = kInvalidMesh;
		MeshHandle             npcMesh = kInvalidMesh;

		// 원격 플레이어 · NPC 노드 → Scene 의 OtherPlayer · NPC (6.10)
		std::unordered_map<uint32_t, NodeHandle> remoteNodes;
		std::vector<NodeHandle>                  freeRemoteNodes;
		std::unordered_map<uint32_t, NodeHandle> npcNodes;
		std::vector<NodeHandle>                  freeNpcNodes;

		// ── 사격 효과 (임시) ────────────────────────────────
		//  얇고 긴 상자를 발사선에 잠깐 놓는 것뿐이다. 명세에는 이펙트 자리가 없다 —
		//  나중에 Scene 의 표시 요소나 파티클(설계 v1)로 옮긴다.
		//  노드는 원격 플레이어와 같은 방식으로 재사용한다(Scene 에 삭제 API 가 없다).
		struct Tracer
		{
			NodeHandle node = kInvalidNode;
			float      life = 0.0f;      // 남은 시간 (s)
		};

		MeshHandle              tracerMesh = kInvalidMesh;
		std::vector<Tracer>     tracers;
		std::vector<NodeHandle> freeTracerNodes;

		std::vector<InstanceData> items;
		float                     titleTimer = 0.0f;

		// 마우스를 다시 잡으려고 누른 클릭이 그대로 사격이 되지 않게 한 프레임 막는다.
		bool                      suppressFire = false;
	};

} // namespace swc
