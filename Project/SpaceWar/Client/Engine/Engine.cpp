#include "Engine.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>

#include <shellapi.h>
#include <objbase.h>
#include <cstdio>
#include <psapi.h>
#include <DirectXMath.h>
#include "Client/DummyMesh.h"
#include "Client/RayTracingParams.h"
#include "Client/Log.h"
#include "Shared/Model/ModelSource.h"   // 행성 접지면을 파싱 결과(collected)로 만든다
#include "Shared/GameLogic/WeaponShowcase.h"   // 총 진열 6개의 자리 · 충돌체 — 서버와 같은 식(2026-11-15)
#include "Shared/GameLogic/ShotTrace.h"        // 한 발의 레이 — 크로스헤어 겨눈 점(서버 판정과 같은 함수, 2026-11-15)

using namespace DirectX;

namespace
{
	swc::InputManager* g_input = nullptr;

	constexpr uint32_t kWidth = 1280;
	constexpr uint32_t kHeight = 720;

	constexpr float kMouseSensitivity = 0.0022f;   // Raw 카운트 -> 라디안

	// 콘솔 로그 창 (Client/Log.h). 로드 단계·모델 캐시·파싱 시간을 찍는다.
	constexpr bool kShowLogConsole = true;

	// 캐릭터 모델. exe 옆 assets\ 기준이다.
	// ★ 파일명이 Client.vcxproj 의 CopyModelAssets 검사에도 적혀 있다 — 바꿀 때 두 곳.
	constexpr const wchar_t* kCharacterModelAsset =
		L"model\\character\\Meshy_AI_01_Arc_Sentinel_1005152429_texture.obj";

	// 맵 GLB 에서 뒷면을 제거할 닫힌 지표면 노드 (client2 2f15728). 표시 지형이며,
	// 같은 메시를 쓰는 충돌 노드(Shared::kPlanetSurfaceNode)는 화면에 그리지 않는다.
	constexpr std::string_view kClosedTerrainNode = "Terrain_Expedition_Planet_Closed";

	// ── 로딩 ────────────────────────────────────────────────
	//  로딩 화면 이미지. exe 옆 assets\ 기준 (Client.vcxproj 빌드 후 복사).
	constexpr const wchar_t* kLoadingBackgroundAsset = L"ui\\loading_background.png";
	constexpr const wchar_t* kLoadingSpinnerAsset = L"ui\\loading_spinner.png";

	// 프레임당 메인에서 마무리할 로딩 완료분 수 (「멀티스레딩 분류 명세서」 ⑥ 업로드 예산).
	// 행성 모델 GPU 업로드처럼 무거운 것이 한 프레임에 겹치지 않게 1 로 둔다.
	constexpr size_t kLoadFinishesPerFrame = 1;

	// ── 사격 효과 (임시) ───────────────────────────────────
	//  판정과 무관한 «보이는 것» 이다. 어디에 맞았는지는 서버만 아므로 길이는 고정이다.
	//  맞은 표시·총구 화염·소리는 없다.
	constexpr float  kTracerLife = 0.06f;     // 보이는 시간 (s)
	constexpr float  kTracerStart = 0.05f;    // 총구 앞에서 시작 (m) — 2026-11-15 원점이 몸통 중심 → 든 총의 총구로 바뀌어 1.5 → 0.05
	constexpr float  kTracerLength = 120.0f;  // 가장 긴 길이 (m) — 지평선이 125m 다. 2026-11-16 부터 그 전에 닿으면 거기까지만
	constexpr float  kTracerMinLength = 0.05f; // 이보다 짧으면 그리지 않는다(총구가 벽 · 땅에 붙어 있을 때)
	constexpr size_t kTracerPool = 4;         // 동시에 보일 수 있는 수
	// 한 프레임에 쏠 수 있는 최대 발 수 — 프레임이 발 간격보다 길 때(기관총 1/11 s 면 11 fps 밑) 밀린 발을 이만큼까지 같이 쏜다.
	//  서버가 한꺼번에 받아 주는 발 수(Server main.cpp kFireBurstShots)와 같게 둔다(2026-11-15).
	constexpr uint32_t kMaxShotsPerFrame = 3;
	// 게이트가 «열렸다» 고 보는 여유(s) — 간격(0.2 s)을 프레임 dt 로 깎다 보면 부동소수 오차로 아주 조금 남아 한 프레임 늦게 열린다
	//  (60 fps 에서 0.2 s 마다 누른 클릭이 버려져 초당 2.5발이 된다 — 시뮬레이션). 0.1 ms 는 사람 손으로는 차이가 없다(2026-11-15).
	constexpr float kFireGateTolerance = 1.0e-4f;

	// ── 크로스헤어 맞추기 (2026-11-15 사용자 «이제 크로스헤어에 맞도록 하는거 조절하자») ──
	//  노션 「사격 시스템 설명」 4.1 의 ① — 눈 → 겨눈 점, 총구 → 겨눈 점(3인칭 슈터의 흔한 방식). Engine::AimDirection.
	//  겨눈 점 찾기는 Shared::TraceShot(서버 판정과 같은 함수) — 지형 간격 · 좁히기 횟수도 거기(kShotTerrainStep · kShotTerrainRefine).
	constexpr double kAimMinDistance   = 0.5;   // m — 겨눈 점이 총구에서 이보다 가까우면 조준선 방향 그대로

	// ── 총 모델 진열 (2026-11-14 사용자 «씬 처음 허공에 1인칭 모델과 3인칭 모델 총 6개 띄우는거까지만») ──
	//  게임 맵 스폰 앞에 총 종류를 오른쪽으로 늘어놓고, 윗줄 1인칭 · 아랫줄 3인칭. 총은 옆면이 스폰 쪽을 보고 총구가 오른쪽.
	//  높이는 그 자리 지면(접지면 — 착륙장 · 길 포함) 위로 잰다.
	//  ★ 2026-11-15 사용자 «충돌체도 같이 적용은 해줘봐» — 6개 모두 그 총의 상자 묶음(WeaponSpec::colliderBoxes)으로 막힌다(서버도 같은 자리 · 같은 충돌체).
	//    처음엔 3인칭 glb COL_(삼각형 5만)이었으나 부딪히면 프레임이 10 까지 떨어져(사용자) 1인칭 메시를 덮는 상자 3 ~ 6개로 바꿨다.
	//    자리 상수 · 계산은 Shared/GameLogic/WeaponShowcase.h(kWeaponShowcase*, LayoutWeaponShowcase)로 옮겼다 — 서버가 같은 식을 쓴다.

	// ── 임시 크로스헤어 (2026-11-14 사용자 «임시 크로스헤어 배치하되, 어떤 방식으로 배치할지 설명할것») ──
	//  화면 한가운데에 고정한다 — 명세 20절 «화면 한가운데가 향하는 방향(조준선)을 사격에 준다» 의 그 점(Camera::LookDirection).
	//  ★ 지금 총알은 몸통 중심에서 조준선과 평행하게 나가서 이 점에서 1 ~ 2 m 비껴 지나간다(노션 「사격 시스템 설명」 4.1).
	//    «눈 → 겨눈 점, 총구 → 겨눈 점» 으로 발사선을 고치면 크로스헤어에 맞게 된다(사격 계획 1단계).
	//  그림은 코드로 만든다(이미지 파일이 없다) — 흰 팔 4개 + 가운데 점, 1 px 검은 테두리(밝은 하늘 · 눈밭에서도 보이게).
	//  UI 텍스처는 premultiplied 알파다(UISprite.h). 텍스처 1 텍셀 = 화면 1 px.
	constexpr uint32_t kCrosshairTexels = 32;        // 텍스처 한 변 = 화면에 그리는 크기 (px)
	constexpr float    kCrosshairGap = 4.0f;         // 가운데에서 팔이 시작하는 거리 (px)
	constexpr float    kCrosshairArm = 8.0f;         // 팔 길이 (px)
	constexpr float    kCrosshairHalfThick = 1.0f;   // 팔 · 점 반두께 (px) — 두께 2 px
	constexpr uint8_t  kCrosshairFill = 235;         // 흰색 불투명도
	constexpr uint8_t  kCrosshairOutline = 150;      // 검은 테두리 불투명도

	// ── 서버 전송 주기 ──────────────────────────────────────
	//  2026-10-29 B안 — 좌표(1/30초) 대신 고정 틱 입력을 보낸다. 주기는 이동 틱(Shared::PlayerMovement::kTickSeconds = 1/60초)이고
	//  렌더 프레임률과는 이미 분리돼 있다(PlayerController::Update 가 틱을 센다). 그래서 여기 따로 주기를 두지 않는다.

	// ── 실행 인자 ───────────────────────────────────────────
	//   Client.exe                     127.0.0.1:25000 에 접속 (기본값)
	//   Client.exe 192.168.0.5         그 주소의 25000 포트로 접속
	//   Client.exe 192.168.0.5 27000   주소와 포트 지정
	//   Client.exe --offline           접속하지 않고 단독 실행
	//
	//  ★ 기본을 "접속" 으로 둔다
	//    비주얼 스튜디오에서 F5 를 누르면 인자가 안 붙는다.
	//    기본이 오프라인이면 서버를 켜놓고 F5 를 눌러도 아무 일이 안 일어나서
	//    "왜 좌표가 안 뜨지" 로 헤매게 된다. (실제로 그랬다)
	struct NetOptions
	{
		bool           online = true;              // 기본 = 접속
		char           host[64] = "127.0.0.1";
		unsigned short port = 25000;
	};

	NetOptions ParseCommandLine()
	{
		NetOptions o{};
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv) return o;

		int nPositional = 0;
		for (int i = 1; i < argc; ++i)
		{
			if (_wcsicmp(argv[i], L"--offline") == 0) { o.online = false; continue; }

			if (nPositional == 0)
				WideCharToMultiByte(CP_ACP, 0, argv[i], -1, o.host, sizeof(o.host), nullptr, nullptr);
			else if (nPositional == 1)
				o.port = static_cast<unsigned short>(_wtoi(argv[i]));

			++nPositional;
		}
		LocalFree(argv);
		return o;
	}

	// 쓰지 않는 노드를 화면 밖으로 치워 두는 변환.
	// 현재 행성 크기보다 충분히 먼 1만 km 아래로 이동한다.
	DirectX::XMMATRIX ParkedTransform()
	{
		return DirectX::XMMatrixTranslation(0.0f, -1.0e7f, 0.0f);
	}

	// exe 가 있는 폴더(끝에 \ 포함).
	// ★ 반드시 와이드로 다룬다. GetModuleFileNameA 는 ANSI(CP949)를 주므로
	//   경로에 한글이 있으면 UTF-8 로 오인해 깨진다.
	std::wstring ExeDirectory()
	{
		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);

		std::wstring p(exePath);
		const size_t slash = p.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash + 1);
	}

	bool AssetFileExists(const std::wstring& path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	// 원본 assets\ 를 찾을 때 exe 폴더에서 몇 단계까지 올라갈지.
	// exe 는 Project\SpaceWar\x64\<구성>\ 에 있어 원본 Project\assets\ 는 세 단계 위다.
	constexpr int kAssetSourceSearchDepth = 6;

	// 에셋 경로. 작업 디렉터리와 무관하게 exe 위치로 찾는다. 두 곳을 차례로 본다
	// (2026-10-29 사용자 «디버그 폴더 안에 리소스가 없다면, 지금 기존의 리소스를 저장해두는 폴더에서 읽어오도록»).
	//  1) exe 옆 assets\ — 빌드 후 대상(Client.vcxproj CopyModelAssets · xcopy)이 복사해 둔 판.
	//  2) 원본 assets\ — exe 폴더에서 위로 올라가며 그 파일이 처음 있는 «…\assets\<relative>».
	//     빌드 후 복사는 원본과 같은 하위 경로(model\map\… · model\prop\… · ui\…)를 쓴다.
	//  파일마다 따로 본다. 두 곳 다 없으면 1) 경로를 돌려준다 → 못 읽었다는 로그에 exe 쪽 경로가 찍힌다.
	// ★ 1) 의 문자열은 예전과 똑같이 만든다 — 모델 디스크 캐시 키에 절대 경로가 들어간다.
	std::wstring AssetPath(const wchar_t* relative)
	{
		const std::wstring exeDirectory = ExeDirectory();
		const std::wstring copied = exeDirectory + L"assets\\" + relative;
		if (AssetFileExists(copied))
			return copied;

		std::wstring directory = exeDirectory;
		for (int depth = 0; depth < kAssetSourceSearchDepth && directory.size() >= 2; ++depth)
		{
			// 끝의 \ 를 빼고 그 앞의 \ 까지 잘라 한 단계 위로 간다. 드라이브 루트에서 멈춘다.
			const size_t slash = directory.find_last_of(L"\\/", directory.size() - 2);
			if (slash == std::wstring::npos)
				break;
			directory.resize(slash + 1);

			const std::wstring source = directory + L"assets\\" + relative;
			if (AssetFileExists(source))
			{
				static bool logged = false;   // 메인 스레드에서만 부른다
				if (!logged)
				{
					logged = true;
					swc::Log(L"[에셋] exe 옆 assets\\ 에 없는 파일은 원본 폴더에서 읽는다 — %sassets\\ (처음: %s)",
						directory.c_str(), relative);
				}
				return source;
			}
		}
		return copied;
	}

	// 좌표만 받는 원격 캐릭터도 현재 지면의 위쪽에 몸통을 정렬한다.
	XMMATRIX CharacterTransform(const swc::Planet& planet, const float position[3])
	{
		const swc::Vec3d p{ position[0], position[1], position[2] };
		const swc::Vec3d up = planet.Up(p);
		const swc::Vec3d forward = swc::ProjectOntoPlane({ 0.0, 0.0, 1.0 }, up);
		const swc::Vec3d right = swc::Cross(up, forward);
		XMMATRIX m;
		m.r[0] = XMVectorSet(float(right.x), float(right.y), float(right.z), 0.0f);
		m.r[1] = XMVectorSet(float(up.x), float(up.y), float(up.z), 0.0f);
		m.r[2] = XMVectorSet(float(forward.x), float(forward.y), float(forward.z), 0.0f);
		m.r[3] = XMVectorSet(position[0], position[1], position[2], 1.0f);
		return m;
	}

	// 해 높이 (2026-10-29 사용자 «태양 위치 플레이어 정면의 45도 상공에 위치하게»).
	//  게임 맵 첫 스폰 · 예시 행성 입장 때, 플레이어가 바라보는 쪽 지평선에서 이 각도만큼 올린 곳에 해를 둔다.
	//  (그 전: render/lab 803ca01 «지평선에 걸치게» = 0°.)
	constexpr double kSunElevationDegrees = 45.0;

	// SetSunDirection 에 넣을 값. sunDir 는 빛이 나아가는 방향 = −해 쪽이다.
	//  해 쪽 = 바라보는 방향을 지면(위쪽에 수직인 면)에 투영한 앞쪽 × cos(높이) + 위쪽 × sin(높이).
	XMFLOAT3 SunDirectionInFront(const swc::Vec3d& facing, const swc::Vec3d& up)
	{
		constexpr double kRadians = kSunElevationDegrees * 3.14159265358979323846 / 180.0;
		const swc::Vec3d front = swc::ProjectOntoPlane(facing, up);
		const swc::Vec3d toSun = swc::Normalize(front * std::cos(kRadians) + up * std::sin(kRadians));
		return XMFLOAT3(float(-toSun.x), float(-toSun.y), float(-toSun.z));
	}

	// 게임 맵의 창 제목 몫 — 처음 로드 · 0 키로 돌아올 때(2026-11-14) 같은 글을 쓴다.
	std::wstring GameMapTerrainStatus(const Shared::PlanetSurface& surface)
	{
		return (Shared::kUsePlanarMap ? L"GLB 평면 지형 " : L"GLB 구면 지형 ") +
			std::to_wstring(surface.TriangleCount()) + L" 삼각형";
	}

	// 진열한 총 하나의 변환 (2026-11-14) — 행 벡터 규약(PlayerController::WorldMatrix 와 같다): 행 = 총의 X · Y · Z 축 · 위치.
	//  2026-11-15 — 축 · 위치는 Shared::LayoutWeaponShowcase 가 구한다(옆면 +X → 스폰 쪽, 총구 +Z → 오른쪽). 충돌체 변환과 같은 값이다.
	XMMATRIX ShowcaseTransform(const Shared::WeaponShowcaseSlot& slot)
	{
		XMMATRIX m;
		m.r[0] = XMVectorSet(float(slot.axisX.x), float(slot.axisX.y), float(slot.axisX.z), 0.0f);
		m.r[1] = XMVectorSet(float(slot.axisY.x), float(slot.axisY.y), float(slot.axisY.z), 0.0f);
		m.r[2] = XMVectorSet(float(slot.axisZ.x), float(slot.axisZ.y), float(slot.axisZ.z), 0.0f);
		m.r[3] = XMVectorSet(float(slot.position.x), float(slot.position.y), float(slot.position.z), 1.0f);
		return m;
	}

	// 임시 크로스헤어 그림 (2026-11-14) — 위 상수 참고. premultiplied RGBA8.
	swc::TextureData MakeCrosshairTexture()
	{
		swc::TextureData image;
		image.path = L"(crosshair)";
		image.width = kCrosshairTexels;
		image.height = kCrosshairTexels;
		image.srgb = false;
		image.pixels.assign(size_t(kCrosshairTexels) * kCrosshairTexels * 4, 0);

		const float center = float(kCrosshairTexels) * 0.5f;
		auto filled = [center](int x, int y)
		{
			if (x < 0 || y < 0 || x >= int(kCrosshairTexels) || y >= int(kCrosshairTexels)) return false;
			const float dx = std::fabs(float(x) + 0.5f - center);
			const float dy = std::fabs(float(y) + 0.5f - center);
			const bool dot = dx < kCrosshairHalfThick && dy < kCrosshairHalfThick;
			const bool horizontal = dy < kCrosshairHalfThick && dx >= kCrosshairGap && dx < kCrosshairGap + kCrosshairArm;
			const bool vertical = dx < kCrosshairHalfThick && dy >= kCrosshairGap && dy < kCrosshairGap + kCrosshairArm;
			return dot || horizontal || vertical;
		};

		for (int y = 0; y < int(kCrosshairTexels); ++y)
		{
			for (int x = 0; x < int(kCrosshairTexels); ++x)
			{
				uint8_t* p = &image.pixels[(size_t(y) * kCrosshairTexels + size_t(x)) * 4];
				if (filled(x, y))
				{
					p[0] = p[1] = p[2] = p[3] = kCrosshairFill;   // 흰색 (premultiplied — 색 = 알파)
					continue;
				}
				bool edge = false;
				for (int oy = -1; oy <= 1 && !edge; ++oy)
					for (int ox = -1; ox <= 1 && !edge; ++ox)
						edge = filled(x + ox, y + oy);
				if (edge)
					p[3] = kCrosshairOutline;   // 검은 테두리 (색 0)
			}
		}
		return image;
	}

	// 예시 행성 디버그 씬의 창 제목 몫 (2026-10-28)
	std::wstring DebugPlanetStatus(size_t index, const swc::PlanetDebugScene& planet)
	{
		return L"디버그 행성 " + std::to_wstring(index + 1) + L" " + swc::PlanetDebugScene::Name(index) +
			L" — " + planet.Status();
	}

	// 로딩 중 임시 메모리와 실행 후 상주량을 구분한다 (client2 2f15728).
	// 우리 로그(Client/Log.h)로 찍는다 — 콘솔 창과 VS 출력창에 함께 나온다.
	void LogMemory(const wchar_t* stage, size_t cachedModels, const swc::ModelData* model = nullptr)
	{
		PROCESS_MEMORY_COUNTERS_EX memory{};
		memory.cb = sizeof(memory);
		if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) return;
		size_t modelBytes = 0;
		if (model)
		{
			for (const auto& mesh : model->meshes)
				modelBytes += mesh.mesh.vertices.capacity() * sizeof(swc::Vertex) +
					mesh.mesh.indices.capacity() * sizeof(uint32_t);
			for (const auto& texture : model->textures) modelBytes += texture.pixels.capacity();
		}
		constexpr double kMiB = 1024.0 * 1024.0;
		swc::Log(L"[Memory] %s: private=%.1f MiB, workingSet=%.1f MiB, peakWorkingSet=%.1f MiB, cpuModel=%.1f MiB, cachedModels=%zu",
			stage, double(memory.PrivateUsage) / kMiB, double(memory.WorkingSetSize) / kMiB,
			double(memory.PeakWorkingSetSize) / kMiB, double(modelBytes) / kMiB, cachedModels);
	}

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (g_input && g_input->HandleMessage(msg, wParam, lParam))
			return 0;

		if (msg == WM_DESTROY)
		{
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	// ── 충돌체 로드 결과 슬롯 (2026-10-29) ──
	//  로딩 스레드가 묶음을 읽어 world 를 채우고, 메인(finish)이 넘겨받는다(멀티스레딩 명세 §9.3 — 잡은 자기 슬롯에만 쓴다).
	//  디버그 행성은 배치 인스턴스를 복사해 넘긴다(SetPlacement 가 배치를 옮겨 가므로). 게임 맵은 둘 다 비어 있다.
	struct CollisionLoadSlot
	{
		std::vector<std::string>            typeNames;
		std::vector<Shared::PlacedInstance> instances;
		Shared::CollisionWorld              world;
		std::wstring                        error;
	};

	// 맵 번호(2026-11-01, Shared/World/WorldCatalog.h)의 이름 — 로그 · 창 제목용
	const wchar_t* WorldLabel(uint32_t world)
	{
		if (world == Shared::kWorldGameMap) return L"게임 맵";
		if (world < Shared::kWorldCount) return swc::PlanetDebugScene::Name(world - 1);
		return L"없음";
	}
}

namespace swc {

	Engine::Engine() = default;

	// ★ 로딩 스레드를 멤버 소멸보다 먼저 멈춘다
	//   로딩 중에 창을 닫으면 로딩 스레드가 planet·terrain 을 읽고 있을 수 있다.
	//   그 멤버들은 resources 보다 뒤에 선언돼 먼저 소멸하므로, 여기서 join 해 둔다.
	Engine::~Engine()
	{
		resources.StopLoader();
	}

	// 시작 순서 — 「멀티스레딩 분류 명세서」 9.1:
	//   파일 I/O 스레드 → Renderer 초기화 → 로딩 씬 → (로딩 스레드에서) 게임 리소스 → 서버 접속 → 게임 루프
	bool Engine::Initialize(HINSTANCE hInstance, int showCommand)
	{
		// WIC(하이트맵 로더)가 COM 객체다. 이게 없으면 CO_E_NOTINITIALIZED 로 조용히 실패한다.
		if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
			return false;

		// 로드·캐시 로그를 볼 콘솔 창. 끄려면 kShowLogConsole 을 false 로.
		if (kShowLogConsole)
			OpenLogConsole();
		Log(L"SpaceWar 시작");

		WNDCLASSEX wc = {};
		wc.cbSize = sizeof(wc);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = WndProc;
		wc.hInstance = hInstance;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.lpszClassName = L"SpaceWarWindow";
		RegisterClassEx(&wc);

		RECT rc = { 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };
		AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

		hwnd = CreateWindow(wc.lpszClassName, L"SpaceWar", WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
			nullptr, nullptr, hInstance, nullptr);

		// 창은 맨 먼저 띄운다 — 렌더러·로딩 화면 초기화 동안에도 실행됐다는 것이 보이게.
		ShowWindow(hwnd, showCommand);
		UpdateWindow(hwnd);

		// 9.1 시작 순서 3 — 파일 I/O 스레드를 Renderer 보다 먼저 띄운다.
		if (!resources.StartLoader())
		{
			MessageBox(hwnd, L"로딩 스레드를 만들지 못했습니다.", L"초기화 실패", MB_OK | MB_ICONERROR);
			return false;
		}

		if (!renderer.Initialize(hwnd, kWidth, kHeight))
		{
			MessageBox(hwnd, renderer.StatusText().c_str(), L"렌더러 초기화 실패", MB_OK | MB_ICONERROR);
			return false;
		}

		// 로딩 씬 — 이 이미지가 없으면 띄우지 않는다. 검은 화면으로 조용히 넘어가면
		// «로딩이 멈췄다» 와 «로딩 화면이 안 나온다» 를 구분할 수 없다.
		std::wstring loadingError;
		if (!sceneManager.Initialize(renderer, resources,
			AssetPath(kLoadingBackgroundAsset), AssetPath(kLoadingSpinnerAsset), loadingError))
		{
			MessageBox(hwnd, loadingError.c_str(), L"로딩 화면 초기화 실패", MB_OK | MB_ICONERROR);
			return false;
		}

		g_input = &input;
		input.Initialize(hwnd);

		camera.SetAspect(float(kWidth) / float(kHeight));

		// 게임 씬 리소스는 로딩 스레드로 넘긴다. 끝날 때까지 로딩 씬이 돈다.
		std::unique_ptr<LoadBatch> gameLoad = std::make_unique<LoadBatch>(L"게임");
		BuildGameLoad(*gameLoad);
		loadStart = std::chrono::steady_clock::now();
		sceneManager.ChangeScene(SceneId::Game, std::move(gameLoad), resources);
		return true;
	}

	// ── 게임 씬 로드 묶음 ───────────────────────────────────────
	//  단계 0 (로딩 스레드) 맵 GLB 파싱 «한 번» → 같은 결과로 접지면(PlanetSurface)과 렌더 메시를 함께 만든다
	//                       → (메인) 지형 연결 · GPU 업로드 · 씬 배치 · RAM 사본 해제
	//         (로딩 스레드) 하늘 행성 LOD2·LOD3   → (메인) LOD 마다 GPU 업로드 · RAM 사본 해제
	//         (로딩 스레드) 캐릭터 모델 파싱       → (메인) GPU 업로드 · 애니메이션 · RAM 사본 해제
	//  단계 1 (메인)        예광탄 · 플레이어 노드 · 스폰(지형을 쓰므로 단계 0 뒤) · 서버 접속
	//
	//  ★ GPU 업로드는 메인에서 한다 — 렌더 몫(명세 ⑥). 한 프레임에 하나씩(kLoadFinishesPerFrame).
	//  ★ 접지면은 로딩 스레드가 «자기 결과 슬롯» 에 만든다 (onParsed 훅, 2026-10-11)
	//    멀티스레딩 명세 §9.2·§9.3: 로딩 콜백은 게임 쪽 데이터를 건드리지 않고, 잡은 자기 결과 슬롯에만 쓴다.
	//    슬롯은 이 요청이 소유하고(shared_ptr), finish(메인)가 완료 큐 ⑥ 을 지난 뒤 planetSurface 로 옮겨 온다.
	//  ★ 모델이 없으면 게임을 띄우지 않는다 — 큐브로 조용히 돌아가면
	//    «모델이 안 나온다» 를 빌드 문제로 착각한다. 실패하면 묶음이 실패로 끝나고 오류 창이 뜬다.
	void Engine::BuildGameLoad(LoadBatch& batch)
	{
		// ── Separated Planet 구면 맵 (GLB) ─────────────────────
		// 지표면 판정은 Shared가 맡고 렌더는 기존 ResourceManager → Model 경로를 사용한다.
		// GLB의 미터 단위와 노드별 변환을 유지한다. 반지름 650m, 중심 (0,-R,0) (Shared/PlanetConst.h)
		LogMemory(L"before map load", resources.ModelCount());
		const std::wstring planetAsset = AssetPath(Shared::kPlanetModelAsset);

		// ★ 맵은 한 번만 파싱한다 (2026-10-09)
		//   렌더 메시를 읽는 같은 파싱에서 접지 노드(지형 충돌 메시·보행로·착륙 플랫폼)의 면만
		//   collected 로 따로 모은다(collectObject). 그걸로 로딩 스레드에서 접지면 BVH 를 바로 만든다.
		ModelRequestOptions planetOptions;
		planetOptions.collectObject = &Shared::PlanetSurface::IsGroundObject;
		auto parsedSurface = std::make_shared<Shared::PlanetSurface>();   // 로딩 스레드의 결과 슬롯
		planetOptions.onParsed = [parsedSurface](Shared::ModelSource& source, std::wstring& error)
		{
			// ★ 닫힌 지표면만 뒷면을 제거한다 (client2 2f15728) — 원본 GLB 는 모든 재질이 양면이다.
			//   건물·나무의 양면 재질은 그대로 둔다. 캐시를 읽은 뒤에도 매번 여기를 지나므로
			//   이 규칙은 캐시 태그와 무관하다.
			for (const Shared::SourceNode& node : source.nodes)
			{
				if (node.name != kClosedTerrainNode) continue;
				for (const uint32_t mesh : node.meshes)
				{
					const uint32_t material = source.meshes[mesh].material;
					if (material < source.materials.size())
						source.materials[material].doubleSided = false;
				}
			}
			return parsedSurface->Build(source.collected, error);
		};
		// 디스크 캐시 태그 — 접지면 규칙이 바뀌면 PlanetSurface::kGroundRuleTag 버전이 올라가 캐시를 다시 만든다.
		planetOptions.cacheTag = Shared::PlanetSurface::kGroundRuleTag;

		resources.RequestModelFile(batch, planetAsset,
			[this, &batch, parsedSurface](const ModelFileResources& loaded)
			{
				// 접지면 — 로딩 스레드가 파싱 직후 결과 슬롯에 만들어 두었다. 여기(메인)서 넘겨받는다.
				planetSurface = std::move(*parsedSurface);
				if (!planetSurface.Valid())
				{
					batch.SetFailureDetail(L"행성 지표면 로드 실패: " + resources.LastError());
					return false;
				}
				terrain.Configure(&planetSurface, planet.radius);
				planet.terrain = &terrain;
				terrainStatus = GameMapTerrainStatus(planetSurface);
				// 캐시 여부는 게임에 들어갈 때 콘솔에 찍는다 — 두 번째 실행부터 «캐시» 가 보여야 정상이다.
				planetFromCache = loaded.fromCache;

				const ModelData* planetData = resources.Get(loaded.model);
				if (!planetData)
				{
					batch.SetFailureDetail(L"맵 모델 로드 실패: " + resources.LastError());
					return false;
				}
				LogMemory(L"map parsed", resources.ModelCount(), planetData);
				const float planetScale = Shared::kUsePlanarMap
					? 1.0f : float(planet.radius / Shared::kPlanetModelReferenceRadius);
				XMFLOAT4X4 planetVisual;
				XMStoreFloat4x4(&planetVisual, XMMatrixScaling(planetScale, planetScale, planetScale));
				if (!planetModel.Initialize(*planetData, renderer, planetVisual))
				{
					batch.SetFailureDetail(L"맵 모델 초기화 실패: " + planetModel.LastError());
					return false;
				}
				// Model이 노드/핸들을 보관하므로 GPU 업로드 후 CPU 메시/픽셀은 필요 없다.
				LogMemory(L"map GPU uploaded", resources.ModelCount(), planetData);
				resources.ReleaseModel(loaded.model);
				LogMemory(L"map CPU released", resources.ModelCount());
				// 루트를 기억해 둔다 — 디버그 행성에 들어가면 숨긴다(EnterDebugPlanet, 2026-10-28).
				gameMapNode = planetModel.Instantiate(scene);
				if (gameMapNode == kInvalidNode)
				{
					batch.SetFailureDetail(L"행성 모델을 장면에 등록하지 못했습니다.");
					return false;
				}
				scene.SetLocalTransform(gameMapNode,
					XMMatrixTranslation(float(planet.center.x), float(planet.center.y), float(planet.center.z)));
				return true;
			},
			std::move(planetOptions));

		// ── 정적 오브젝트 충돌체 (2026-10-29, Shared/Physics/CollisionWorld.h) ──
		//  맵 glb 옆 충돌체 묶음(Resource_Work/_tools/collision/col_build.py)을 로딩 스레드에서 읽는다.
		//  이동 예측(controller.SetCollision)과 C 키 와이어프레임이 쓴다. 못 읽어도 게임은 띄운다 — 지형만 막는다(로그만 남긴다).
		{
			auto collision = std::make_shared<CollisionLoadSlot>();
			const std::wstring collisionPack = AssetPath(Shared::kPlanetCollisionAsset);
			const std::wstring collisionManifest = AssetPath(Shared::kPlanetCollisionManifestAsset);
			batch.Add(L"충돌체",
				[collision, collisionPack, collisionManifest]()
				{
					if (!collision->world.LoadPack(collisionPack.c_str(), collisionManifest.c_str(), collision->error))
						return false;
					collision->world.Finalize();
					return true;
				},
				[this, collision](bool ok)
				{
					if (!ok)
					{
						Log(L"[충돌체] 게임 맵 충돌체를 못 읽어 없이 띄운다 — %s", collision->error.c_str());
						return true;
					}
					gameMapCollision = std::move(collision->world);
					colliderView.Clear();
					Log(L"[충돌체] 게임 맵 — 메시 %zu · 인스턴스 %zu · 삼각형 %zu (C 키 = 와이어프레임)",
						gameMapCollision.MeshCount(), gameMapCollision.InstanceCount(), gameMapCollision.MeshTriangleCount());
					return true;
				});
		}

		// ── 하늘의 원거리 행성 (GLB LOD2/LOD3, client2 2f15728) ──
		//  LOD 마다 따로 요청한다 — 파싱은 로딩 스레드, GPU 업로드는 메인에서 한 프레임에 하나씩.
		//  한 단계씩 올리고 바로 CPU 사본을 버려 두 LOD 의 RAM 피크가 겹치지 않는다.
		for (size_t lod = 0; lod < SkyPlanet::kLodCount; ++lod)
		{
			resources.RequestModelFile(batch, AssetPath(SkyPlanet::LodAsset(lod)),
				[this, &batch, lod](const ModelFileResources& loaded)
				{
					const ModelData* data = resources.Get(loaded.model);
					if (!data)
					{
						batch.SetFailureDetail(L"하늘 행성 로드 실패: " + resources.LastError());
						return false;
					}
					const bool added = skyPlanet.AddLod(lod, *data, renderer);
					resources.ReleaseModel(loaded.model);
					if (!added)
					{
						batch.SetFailureDetail(L"하늘 행성 초기화 실패: " + skyPlanet.LastError());
						return false;
					}
					LogMemory(L"sky planet CPU released", resources.ModelCount());
					return true;
				});
		}

		// ── 캐릭터 모델 (OBJ) ──────────────────────────────────
		//  ★ 한 번만 읽고 GPU 자원을 만든다. 플레이어·원격 플레이어·NPC 가 같은 메시·재질을 공유한다
		//    (그래서 BLAS 도 하나다).
		//  ★ 파일 하나에서 메시·스켈레톤·클립이 함께 나온다 (명세 §4 — 종류별로 다른 핸들).
		//    지금 에셋은 OBJ 라 메시만 나오고, .glb 로 바꾸면 스켈레톤·클립도 같이 채워진다.
		resources.RequestModelFile(batch, AssetPath(kCharacterModelAsset),
			[this, &batch](const ModelFileResources& loaded)
			{
				const ModelData* modelData = resources.Get(loaded.model);
				if (!modelData)
				{
					batch.SetFailureDetail(L"캐릭터 모델 로드 실패: " + resources.LastError());
					return false;
				}
				if (!characterModel.Initialize(*modelData, renderer))
				{
					batch.SetFailureDetail(L"모델 초기화 실패: " + characterModel.LastError());
					return false;
				}
				// GPU 업로드가 끝났으므로 CPU 메시·픽셀을 버린다. 스켈레톤·클립은 남는다(Animator 가 쓴다).
				resources.ReleaseModel(loaded.model);
				LogMemory(L"character CPU released", resources.ModelCount());

				// 스킨이 있는 모델이면 재생 인스턴스를 만들고 첫 클립을 돌린다 (명세 §14).
				// OBJ 캐릭터는 뼈가 없어 여기를 지나가지 않는다 — .glb 로 내보내면 살아난다.
				if (loaded.skeleton.Valid())
				{
					playerAnimation = animator.Create(loaded.skeleton);
					if (!loaded.animations.empty())
						animator.Play(playerAnimation, loaded.animations.front());
				}
				return true;
			});

		// ── 총 모델 (2026-11-14, Shared/GameObject/Weapon.h · Client/Weapon/WeaponModels.h) ──
		//  총 종류마다 1인칭 · 3인칭 glb 를 하나씩 요청한다(파싱은 로딩 스레드, GPU 업로드는 메인에서 한 프레임에 하나).
		//  올린 뒤 바로 CPU 사본을 버린다. 한 파일을 못 읽어도 게임은 띄운다 — 그 모델만 빼고 로그를 남긴다.
		//  1인칭 파일은 Meshy 원본 메시 그대로라 크다(권총 133만 삼각형 · 69 MB). 첫 실행은 파싱이 길고, 다음부터는 모델 디스크 캐시를 읽는다.
		for (size_t t = 0; t < Shared::kWeaponTypeCount; ++t)
		{
			for (size_t v = 0; v < kWeaponViewCount; ++v)
			{
				const Shared::WeaponType type = Shared::WeaponType(t);
				const WeaponView view = WeaponView(v);
				const std::wstring asset = WeaponModels::Asset(type, view);
				resources.RequestModelFile(batch, AssetPath(asset.c_str()),
					[this, type, view, asset](const ModelFileResources& loaded)
					{
						const ModelData* data = resources.Get(loaded.model);
						if (!data)
						{
							Log(L"[총] 모델 건너뜀 — %s: %s", asset.c_str(), resources.LastError().c_str());
							return true;
						}
						if (!weaponModels.Initialize(type, view, *data, renderer))
							Log(L"[총] 모델 건너뜀 — %s: %s", asset.c_str(), weaponModels.LastError().c_str());
						// GPU 업로드가 끝났으므로 CPU 메시 · 픽셀을 버린다(실패해도 버린다).
						resources.ReleaseModel(loaded.model);
						return true;
					});
			}
		}

		// ── 단계 1: 씬 구성 · 스폰 · 서버 접속 (메인) ──
		batch.NextStage();
		batch.Add(L"씬 구성", nullptr,
			[this](bool)
			{
				// 예광탄 — +Z 로 1m 길이. 쏠 때 Z 만 늘려 발사선에 놓는다.
				MeshData tracerData = MakeBox(0.10f, 0.10f, 1.0f, { 1.00f, 0.85f, 0.30f });
				tracerMesh = renderer.CreateMesh(
					tracerData.vertices.data(), tracerData.vertices.size(),
					tracerData.indices.data(), tracerData.indices.size());

				// 모델은 루트(이동) + 표시 보정 + OBJ 오브젝트 노드로 펼쳐진다. 반환값이 이동 루트다.
				// 화면 밖으로 치울 때도 이 루트만 옮기면 자식이 따라온다.
				player = characterModel.Instantiate(scene);

				// 예광탄 노드는 미리 만들어 화면 밖에 치워 둔다. 쏠 때 하나 꺼내 쓰고 되돌린다.
				freeTracerNodes.reserve(kTracerPool);
				for (size_t i = 0; i < kTracerPool; ++i)
				{
					const NodeHandle tracer = scene.AddNode(kInvalidNode, tracerMesh, 0);
					scene.SetLocalTransform(tracer, ParkedTransform());
					freeTracerNodes.push_back(tracer);
				}

				// 공통 시작 방향(Landing L1)을 사용하고 실제 지면·플랫폼 높이에 맞춘다 (client2 2f15728).
				// (모델도 중심 기준으로 1m 내려 배치된다 — Model.cpp kGroundOffset).
				controller.SetPlanet(&planet);
				// 게임 맵 충돌체(단계 0 «충돌체» 에서 읽었다 — 못 읽었으면 비어 있어 지형만 막는다, 2026-10-29 B안)
				controller.SetCollision(gameMapCollision.Loaded() ? &gameMapCollision : nullptr);
				controller.Spawn(GameMapSpawnPoint(), { 0.0, 0.0, 1.0 });
				spawnPosition = controller.Position();
				camera.SnapTo(controller.Position(), controller.Up(), controller.Facing());

				// 해를 처음 바라보는 쪽 45° 상공에 둔다 (2026-10-29 사용자 «태양 위치 플레이어 정면의 45도 상공에 위치하게») — 스폰 자리 기준.
				renderer.SetSunDirection(SunDirectionInFront(controller.Facing(), controller.Up()));

				// 총 모델 6개를 스폰 앞 허공에 띄운다(2026-11-14) — 단계 0 에서 올린 것만.
				BuildWeaponShowcase();

				// 임시 크로스헤어 텍스처(2026-11-14) — 그리기는 RenderFrame(화면 한가운데).
				crosshairTexture = renderer.CreateTexture(MakeCrosshairTexture());
				if (crosshairTexture == kInvalidTexture)
					Log(L"[UI] 크로스헤어 텍스처를 만들지 못했다 — %s", renderer.StatusText().c_str());
				return true;
			});

		// ★ 접속은 메인에서 한다 — 네트워크 스레드(M4)가 생기기 전까지 Network 는 메인 것이다.
		//   서버가 없으면 대개 바로 거절되지만, 응답 없는 주소면 그동안 스피너가 멈춘다.
		batch.Add(L"서버 접속", nullptr,
			[this](bool)
			{
				const NetOptions netOpt = ParseCommandLine();
				netStatus = L"오프라인";
				if (netOpt.online)
				{
					std::wstring err;
					if (network.Connect(netOpt.host, netOpt.port, err))
						netStatus = L"접속됨";
					else
						netStatus = L"접속 실패: " + err;
				}
				return true;   // 접속 실패는 오프라인으로 계속한다 — 지금까지와 같은 동작
			});
	}

	void Engine::EnterGame()
	{
		// 마우스는 게임에 들어갈 때 잡는다. 로딩 중에는 창을 옮기거나 다른 창으로 갈 수 있게 둔다.
		input.SetCaptured(true);
		titleTimer = 0.0f;

		// 게임 리소스 로드에 걸린 시간 — 로딩 씬을 띄운 순간부터 게임에 들어온 순간까지.
		const double seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - loadStart).count();
		Log(L"[게임] 입장 — 로드 %.2fs (행성 %s) · %s", seconds,
			planetFromCache ? L"캐시" : L"파싱", terrainStatus.c_str());
	}

	void Engine::Run()
	{
		timer.Reset();

		for (;;)
		{
			input.BeginFrame();
			if (!PumpMessages())
				break;

			timer.Tick();
			const float dt = timer.DeltaTime();

			// ⑥ 로딩 완료분 — 업로드 예산만큼만 메인에서 마무리한다(게임 중에도 매 프레임).
			resources.PumpLoaded(kLoadFinishesPerFrame);
			sceneManager.Update(dt, resources);

			if (sceneManager.LoadFailed())
			{
				MessageBox(hwnd, sceneManager.LoadError().c_str(), L"로딩 실패", MB_OK | MB_ICONERROR);
				break;
			}
			const SceneId entered = sceneManager.TakeEntered();
			size_t debugPlanet = 0;
			if (entered == SceneId::Game)
			{
				// 예시 행성에서 0 키로 돌아왔으면(2026-11-14) 처음 입장이 아니다 — 보이는 맵 · 접지면 · 스폰만 되돌린다.
				if (InDebugPlanet())
					ReturnToGameMap();
				else
					EnterGame();
			}
			else if (IsDebugPlanetScene(entered, debugPlanet))
				EnterDebugPlanet(debugPlanet);

			if (sceneManager.IsLoading())
			{
				// 로딩 중에도 받기는 한다(2026-11-01 맵 번호) — 행성을 처음 읽는 동안 서버가 보낸 것이 소켓 · 서버에 쌓이지 않게.
				//  받은 것을 쓰는 일(보정 · 원격 노드 · 맵 답)은 들어간 뒤 UpdateNetwork 가 한다.
				network.Poll();
				RenderLoading();
				UpdateLoadingTitle(dt);
				continue;
			}

			HandleSystemKeys();
			UpdatePlayer(dt);
			UpdateDebugPlanetObjects();
			UpdateWeapons(dt);   // 2026-11-15 — 든 총 · 바닥 총 · E 줍기 · G 떨구기. 이번 프레임 든 총으로 UpdateFire 가 쏜다
			UpdateFire(dt);
			UpdateNetwork(dt);

			// 애니메이션 포즈 — 게임 단계의 «출력» 이다 (멀티스레딩 명세 3.2 ③).
			//  총구 위치·히트박스가 포즈를 쓰므로 렌더보다 먼저, 상태가 다 정해진 뒤에 돌린다.
			//  나중에 캐릭터 범위 단위 잡으로 쪼갠다(M5).
			animator.Update(resources, dt);

			RenderFrame();
			UpdateTitle(dt);
		}
	}

	// 종료 순서 — 9.1: 네트워크를 먼저 끊고, 파일 I/O 스레드를 멈춘다(대기 요청 취소 + join).
	//   GPU 자원은 멤버 소멸 때 Renderer 가 맨 마지막에 푼다.
	void Engine::Shutdown()
	{
		network.Disconnect();
		resources.StopLoader();
		g_input = nullptr;
		input.SetCaptured(false);
		CoUninitialize();
	}

	bool Engine::PumpMessages()
	{
		MSG msg = {};
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
				return false;

			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		return true;
	}

	void Engine::HandleSystemKeys()
	{
		// ESC 로 마우스 놓기 / 다시 클릭하면 잡기
		suppressFire = false;
		if (input.WasPressed(VK_ESCAPE)) input.SetCaptured(false);
		else if (!input.Captured() && input.MouseDown(0))
		{
			input.SetCaptured(true);
			suppressFire = true;   // 이 클릭은 «잡기» 였다. 총알이 나가면 안 된다
		}

		// V = 디버그 뷰 순환, R = RT 토글
		//  (2026-10-27 렌더 2단계 — 룰렛 무릎점 [ ] 키는 RayTracingParams 에서 룰렛이 빠져 없앴다)
		// 11번째(10) 는 재질 확인용 — 거칠기·금속성
		if (input.WasPressed('V'))
			renderer.SetDebugMode((renderer.DebugMode() + 1) % 14);   // Lighting.hlsl · Forward.hlsl 디버그 뷰 0~13 (6단계: 11 주변광, 7단계: 12 GI 프로브 사용, 8단계: 13 AO)
		if (input.WasPressed('R'))
		{
			RayTracingParams p = renderer.GetRayTracingParams();
			p.enabled = !p.enabled;
			renderer.SetRayTracingParams(p);
		}

		// C = 충돌체 와이어프레임 (2026-10-29 사용자 «c키를 눌러 저폴리 충돌체를 와이어 프레임으로 볼 수있게 해줘»)
		if (input.WasPressed('C'))
		{
			showColliders = !showColliders;
			colliderView.Clear();
			const Shared::CollisionWorld* world = ActiveCollision();
			Log(L"[충돌체] 와이어프레임 %s — %s", showColliders ? L"켬" : L"끔",
				world && world->Loaded() ? L"이 맵의 충돌체를 그린다" : L"이 맵은 충돌체를 못 읽었다(로드 로그 참고)");
		}

		HandleDebugPlanetKeys();
	}

	// 지금 맵의 충돌체 — 예시 행성이면 그 씬의 것, 아니면 게임 맵 것(2026-10-29)
	const Shared::CollisionWorld* Engine::ActiveCollision() const
	{
		return InDebugPlanet() ? &sceneManager.DebugPlanet(activeDebugPlanet).Collision() : &gameMapCollision;
	}

	// 지금 보이는 맵의 번호(2026-11-01) — 서버의 맵 번호와 같은 규칙(Shared/World/WorldCatalog.h).
	uint32_t Engine::CurrentWorld() const
	{
		return InDebugPlanet() ? Shared::WorldOfDebugPlanet(activeDebugPlanet) : Shared::kWorldGameMap;
	}

	// ── 예시 행성 디버그 씬 (2026-10-28) ─────────────────────────
	//  숫자키 1~5 = Verdant · Neon · Ruin · Volcanic · Glacier (PlanetDebugScene.h).
	//  처음 고르면 로딩 씬을 띄우고 맵을 읽는다. 한 번 읽은 행성은 묶음 없이 바로 넘어간다.
	//  0 = 처음 씬(게임 맵)으로 돌아간다 (2026-11-14 사용자 «처음 씬을 0번 키에 할당해주고»).
	//  ★ 맵 번호 (2026-11-01) — 고르는 순간 서버에 JoinWorld 를 보낸다. 서버는 옛 맵에서 이 플레이어를 빼고
	//    (다른 사람에게 PlayerLeave) 새 맵을 준비해 WorldJoined 로 답한다. 그 전까지 입력 · 사격은 보내지 않는다.
	//    게임 맵(0)도 같은 길이다 — 서버 HandleWorldJoin 은 맵 번호를 가리지 않는다.
	void Engine::HandleDebugPlanetKeys()
	{
		if (input.WasPressed('0'))
		{
			if (!InDebugPlanet()) return;   // 이미 게임 맵이다

			if (network.Connected())
			{
				network.SendJoinWorld(Shared::kWorldGameMap);
				Log(L"[맵] 서버에 맵 %u(게임 맵)로 옮긴다고 알렸다", Shared::kWorldGameMap);
			}
			loadStart = std::chrono::steady_clock::now();
			// 게임 맵은 처음에 읽었다 — 묶음 없이 바로 넘어가고, 들어간 프레임에 Run 이 ReturnToGameMap 을 부른다.
			sceneManager.ChangeScene(SceneId::Game, nullptr, resources);
			return;
		}

		for (size_t i = 0; i < PlanetDebugScene::kCount; ++i)
		{
			if (!input.WasPressed('1' + int(i))) continue;
			if (i == activeDebugPlanet) return;   // 이미 그 행성이다

			if (network.Connected())
			{
				network.SendJoinWorld(Shared::WorldOfDebugPlanet(i));
				Log(L"[맵] 서버에 맵 %u(%s)로 옮긴다고 알렸다", Shared::WorldOfDebugPlanet(i), PlanetDebugScene::Name(i));
			}

			loadStart = std::chrono::steady_clock::now();
			const SceneId next = DebugPlanetSceneId(i);
			if (sceneManager.DebugPlanet(i).Loaded())
			{
				sceneManager.ChangeScene(next, nullptr, resources);
				return;
			}
			std::unique_ptr<LoadBatch> load = std::make_unique<LoadBatch>(
				std::wstring(L"디버그 행성 ") + PlanetDebugScene::Name(i));
			BuildDebugPlanetLoad(*load, i);
			sceneManager.ChangeScene(next, std::move(load), resources);
			return;
		}
	}

	// 게임 맵(BuildGameLoad)과 같은 길 — 맵 glb 를 로딩 스레드에서 한 번 파싱해 렌더 메시와 접지면을 함께 얻고,
	// 메인에서 GPU 에 올린 뒤 RAM 사본을 버린다. 배치 JSON 은 읽지 않는다(PlanetDebugScene.h).
	void Engine::BuildDebugPlanetLoad(LoadBatch& batch, size_t index)
	{
		ModelRequestOptions options;
		options.collectObject = &Shared::PlanetSurface::IsGroundObject;
		options.cacheTag = Shared::PlanetSurface::kGroundRuleTag;
		auto parsedGround = std::make_shared<Shared::PlanetSurface>();   // 로딩 스레드의 결과 슬롯
		options.onParsed = [parsedGround](Shared::ModelSource& source, std::wstring& error)
		{
			return parsedGround->Build(source.collected, error);
		};

		resources.RequestModelFile(batch, AssetPath(PlanetDebugScene::Asset(index)),
			[this, &batch, index, parsedGround](const ModelFileResources& loaded)
			{
				const std::wstring name = PlanetDebugScene::Name(index);
				const ModelData* data = resources.Get(loaded.model);
				if (!data)
				{
					batch.SetFailureDetail(name + L" 맵 로드 실패: " + resources.LastError());
					return false;
				}
				std::wstring error;
				const bool built = sceneManager.DebugPlanet(index).Build(*data, std::move(*parsedGround),
					renderer, scene, planet.center, error);
				// GPU 업로드가 끝났으므로 CPU 메시 · 픽셀을 버린다(실패해도 버린다).
				resources.ReleaseModel(loaded.model);
				if (!built)
				{
					batch.SetFailureDetail(name + L": " + error);
					return false;
				}
				LogMemory(L"debug planet CPU released", resources.ModelCount());
				return true;
			},
			std::move(options));

		// ── 배치 JSON → 오브젝트 모델 (2026-10-28) ──
		//  JSON 은 로딩 스레드에서 읽고(PlanetDebugScene::ReadPlacement), 메인에서 씬에 넘긴다.
		//  오브젝트 목록은 JSON 을 읽어야 알 수 있어서, 이 finish 안에서 NextStage + RequestModelFile 로 다음 단계를 덧붙인다.
		//  (LoadBatch 는 앞 단계가 메인에서 다 끝나야 다음 단계를 내므로, 아직 내지 않은 뒤 단계에 더하는 것은 안전하다.)
		//  JSON 이나 오브젝트 하나를 못 읽어도 맵은 띄운다 — 디버깅용이라 그 종류만 빼고 로그를 남긴다.
		auto placement = std::make_shared<PlanetPlacement>();   // 로딩 스레드의 결과 슬롯
		const std::wstring placementPath = AssetPath(PlanetDebugScene::PlacementAsset(index));
		batch.Add(L"배치 JSON",
			[placement, placementPath]()
			{
				return PlanetDebugScene::ReadPlacement(placementPath, *placement);
			},
			[this, &batch, index, placement](bool ok)
			{
				if (!ok)
				{
					Log(L"[디버그 행성] 배치 JSON 을 못 읽어 오브젝트 없이 띄운다 — %s", placement->error.c_str());
					return true;
				}
				PlanetDebugScene& target = sceneManager.DebugPlanet(index);
				// 지형 레이어 DDS 경로 — SetPlacement 가 배치 결과를 옮겨 가기 전에 뽑는다(2026-10-28).
				const std::vector<std::wstring> layerAssets = PlanetDebugScene::TerrainLayerAssets(index, *placement);
				// 충돌체 입력 — 같은 이유로 종류 이름 · 인스턴스를 먼저 복사한다(2026-10-29).
				auto collision = std::make_shared<CollisionLoadSlot>();
				collision->typeNames.reserve(placement->types.size());
				for (const PlanetPlacement::Type& type : placement->types)
					collision->typeNames.push_back(type.name);
				collision->instances.reserve(placement->instances.size());
				for (const PlanetPlacement::Instance& instance : placement->instances)
				{
					Shared::PlacedInstance placed;
					placed.type = instance.type;
					std::copy(std::begin(instance.position), std::end(instance.position), placed.position);
					std::copy(std::begin(instance.rotation), std::end(instance.rotation), placed.rotation);
					placed.scale = instance.scale;
					collision->instances.push_back(placed);
				}
				target.SetPlacement(std::move(*placement));

				batch.NextStage();

				// ── 충돌체 (2026-10-29, 사용자 «… 나머진은 저폴리로 충돌 가자 … c키를 눌러 … 와이어 프레임으로») ──
				//  맵 glb 옆 «<행성>_Collision.glb · .json» 을 읽고 배치 인스턴스를 씌운다(로딩 스레드).
				//  작은 식물(충돌체 없음)과 작은 돌(배율 < min_scale)은 여기서 빠진다. 못 읽어도 맵은 띄운다(로그만).
				{
					const std::wstring collisionPack = AssetPath(PlanetDebugScene::CollisionAsset(index).c_str());
					const std::wstring collisionManifest = AssetPath(PlanetDebugScene::CollisionManifestAsset(index).c_str());
					batch.Add(L"충돌체",
						[collision, collisionPack, collisionManifest]()
						{
							if (!collision->world.LoadPack(collisionPack.c_str(), collisionManifest.c_str(), collision->error))
								return false;
							collision->world.AddPlacement(collision->typeNames, collision->instances);
							collision->world.Finalize();
							collision->instances.clear();
							collision->instances.shrink_to_fit();
							return true;
						},
						[this, index, collision](bool ok)
						{
							if (!ok)
							{
								Log(L"[디버그 행성] %s 충돌체를 못 읽어 없이 띄운다 — %s",
									PlanetDebugScene::Name(index), collision->error.c_str());
								return true;
							}
							const Shared::CollisionWorld& world = collision->world;
							Log(L"[디버그 행성] %s 충돌체 — 종류 %zu · 인스턴스 %zu · 삼각형 %zu · 작은 돌 제외 %zu · 충돌체 없는 종류(작은 식물) %zu",
								PlanetDebugScene::Name(index), world.MeshCount(), world.InstanceCount(), world.MeshTriangleCount(),
								world.SkippedSmallRocks(), world.SkippedNoCollider());
							sceneManager.DebugPlanet(index).SetCollision(std::move(collision->world));
							colliderView.Clear();
							return true;
						});
				}

				// ── 지형 레이어 (2026-10-28, 사용자 «렌더러 밖 수정 이번만 허용») ──
				//  glb 의 지형 재질은 대체 레이어 한 장뿐이다. 레이어 4종 + 마스크의 밉맵 DDS 를 로딩 스레드에서 읽고,
				//  메인에서 렌더러 지형 재질을 만들어 지형 노드에 끼운다. 이 단계는 맵 Build(앞 단계) 뒤에 돈다.
				//  못 읽으면 대체 레이어 한 장 그대로 둔다(로그만).
				if (layerAssets.empty())
				{
					Log(L"[디버그 행성] 배치 JSON 에 terrain_layers 가 없어 지형은 대체 레이어 한 장으로 둔다");
				}
				else
				{
					auto layers = std::make_shared<TerrainLayerSet>();   // 로딩 스레드의 결과 슬롯
					std::vector<std::wstring> layerPaths;
					layerPaths.reserve(layerAssets.size());
					for (const std::wstring& asset : layerAssets)
						layerPaths.push_back(AssetPath(asset.c_str()));
					batch.Add(L"지형 레이어",
						[layers, layerPaths]()
						{
							return PlanetDebugScene::LoadTerrainLayers(layerPaths, *layers);
						},
						[this, index, layers](bool ok)
						{
							if (!ok)
							{
								Log(L"[디버그 행성] 지형 레이어를 못 읽어 대체 레이어 한 장으로 둔다 — %s", layers->error.c_str());
								return true;
							}
							std::wstring error;
							if (!sceneManager.DebugPlanet(index).ApplyTerrainLayers(index, std::move(*layers), renderer, scene, error))
								Log(L"[디버그 행성] 지형 레이어 적용 실패 — 대체 레이어 한 장으로 둔다: %s", error.c_str());
							else
								Log(L"[디버그 행성] %s 지형 레이어 4종 + 마스크 적용", PlanetDebugScene::Name(index));
							return true;
						});
				}
				for (size_t type = 0; type < target.ObjectTypeCount(); ++type)
				{
					const std::wstring asset = target.ObjectAsset(type);
					resources.RequestModelFile(batch, AssetPath(asset.c_str()),
						[this, index, type, asset](const ModelFileResources& loaded)
						{
							const ModelData* data = resources.Get(loaded.model);
							if (!data)
							{
								Log(L"[디버그 행성] 오브젝트 건너뜀 — %s: %s", asset.c_str(), resources.LastError().c_str());
								return true;
							}
							std::wstring error;
							if (!sceneManager.DebugPlanet(index).AddObjectModel(type, *data, renderer, error))
								Log(L"[디버그 행성] 오브젝트 건너뜀 — %s: %s", asset.c_str(), error.c_str());
							// GPU 업로드가 끝났으므로 CPU 메시 · 픽셀을 버린다.
							resources.ReleaseModel(loaded.model);
							return true;
						});
				}
				return true;
			});
	}

	void Engine::EnterDebugPlanet(size_t index)
	{
		PlanetDebugScene& target = sceneManager.DebugPlanet(index);
		if (!target.Loaded()) return;

		// 보이는 맵을 바꿔 끼운다 — 모든 맵이 같은 자리(행성 중심)에 겹쳐 있으므로 하나만 켠다.
		if (gameMapNode != kInvalidNode)
			scene.SetVisible(gameMapNode, false);
		// 총 진열은 게임 맵 스폰 앞에 있다(2026-11-14) — 다른 행성의 지형 속에 떠 보이지 않게 숨긴다.
		if (weaponShowcase != kInvalidNode)
			scene.SetVisible(weaponShowcase, false);
		for (size_t i = 0; i < PlanetDebugScene::kCount; ++i)
		{
			const PlanetDebugScene& other = sceneManager.DebugPlanet(i);
			if (other.Loaded())
				scene.SetVisible(other.Root(), i == index);
		}

		// 접지면도 바꾼다 — 이동 · 착지 · 카메라가 모두 Planet::SurfaceHeight 하나로 지형을 본다(Planet.h).
		terrain.Configure(&target.Ground(), planet.radius);
		planet.terrain = &terrain;

		// 옛 맵의 원격 플레이어 · NPC 를 치운다. 새 맵 사람들은 서버가 이 맵에 넣은 뒤(WorldJoined) 다시 받는다(2026-11-01 맵 번호).
		ParkNetworkNodes();

		// 충돌체도 이 행성 것으로(2026-10-29) — 못 읽었으면 지형만 막는다.
		controller.SetCollision(target.Collision().Loaded() ? &target.Collision() : nullptr);

		// 착륙 지점 L1 에 다시 세운다. 높이는 Spawn 이 새 접지면에서 잰다.
		controller.Spawn(planet.PositionAt(target.SpawnUp(), Shared::kGroundOffset), controller.Facing());
		spawnPosition = controller.Position();
		camera.SnapTo(controller.Position(), controller.Up(), controller.Facing());
		scene.SetLocalTransform(player, controller.WorldMatrix());

		// 해를 바라보는 쪽 45° 상공에 둔다 (2026-10-29 사용자 «태양 위치 플레이어 정면의 45도 상공에 위치하게») — 착륙 지점 L1 기준.
		renderer.SetSunDirection(SunDirectionInFront(controller.Facing(), controller.Up()));

		// 스폰 자리 주변 오브젝트를 바로 고른다(이후는 UpdateDebugPlanetObjects 가 움직일 때마다).
		target.StreamObjects(scene, controller.Position() - planet.center, true);
		terrainStatus = DebugPlanetStatus(index, target);

		activeDebugPlanet = index;
		titleTimer = 0.0f;

		const double seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - loadStart).count();
		Log(L"[디버그 행성] %zu %s 입장 — %.2fs · %s", index + 1, PlanetDebugScene::Name(index),
			seconds, terrainStatus.c_str());
	}

	// 0 키(2026-11-14) — 예시 행성에서 처음 씬(게임 맵)으로 돌아온다. EnterDebugPlanet 을 거꾸로 한다.
	//  서버에는 HandleDebugPlanetKeys 가 JoinWorld(0) 를 보냈다. 답(WorldJoined)이 오면 UpdateNetwork 가 서버 스폰 자리에 다시 세운다.
	void Engine::ReturnToGameMap()
	{
		for (size_t i = 0; i < PlanetDebugScene::kCount; ++i)
		{
			const PlanetDebugScene& other = sceneManager.DebugPlanet(i);
			if (other.Loaded())
				scene.SetVisible(other.Root(), false);
		}
		if (gameMapNode != kInvalidNode)
			scene.SetVisible(gameMapNode, true);
		if (weaponShowcase != kInvalidNode)
			scene.SetVisible(weaponShowcase, true);

		// 접지면 · 충돌체를 게임 맵 것으로(처음 로드 때 만든 것 그대로 남아 있다).
		terrain.Configure(&planetSurface, planet.radius);
		planet.terrain = &terrain;
		controller.SetCollision(gameMapCollision.Loaded() ? &gameMapCollision : nullptr);
		colliderView.Clear();

		// 옛 맵의 원격 플레이어 · NPC 를 치운다. 게임 맵 사람들 · NPC 는 서버가 넣어 준 뒤 다시 받는다.
		ParkNetworkNodes();

		controller.Spawn(GameMapSpawnPoint(), { 0.0, 0.0, 1.0 });
		spawnPosition = controller.Position();
		camera.SnapTo(controller.Position(), controller.Up(), controller.Facing());
		scene.SetLocalTransform(player, controller.WorldMatrix());
		renderer.SetSunDirection(SunDirectionInFront(controller.Facing(), controller.Up()));

		terrainStatus = GameMapTerrainStatus(planetSurface);
		activeDebugPlanet = kNoDebugPlanet;
		titleTimer = 0.0f;

		const double seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - loadStart).count();
		Log(L"[맵] 게임 맵(0)으로 돌아왔다 — %.2fs · %s", seconds, terrainStatus.c_str());
	}

	// 게임 맵 스폰 — 공통 시작 방향(Landing L1)의 지면 위 kGroundOffset (client2 2f15728). 처음 로드 · 0 키가 같이 쓴다.
	Vec3d Engine::GameMapSpawnPoint() const
	{
		return Shared::kUsePlanarMap
			? Vec3d{ Shared::kMapSpawnX, 0.0, Shared::kMapSpawnZ }
			: planet.PositionAt(Normalize({ Shared::kPlanetSpawnUpX,
				Shared::kPlanetSpawnUpY, Shared::kPlanetSpawnUpZ }), Shared::kGroundOffset);
	}

	// 총 모델 진열 (2026-11-14) — 씬 구성(로드 단계 1)에서 스폰한 직후 한 번. 단계 0 에서 올린 모델만 띄운다.
	//  루트 노드 하나 아래에 6개를 둔다 — 예시 행성에 들어가면 루트만 숨긴다.
	//  ★ 2026-11-15 충돌체 — 자리는 Shared::LayoutWeaponShowcase(서버 SetupWeaponShowcase 와 같은 식 · 같은 스폰 인자).
	//    게임 맵 충돌체(gameMapCollision)에 총마다 상자 묶음 메시 + 6개 인스턴스를 넣고 격자를 다시 만든다.
	//    모델을 못 올린 총도 충돌체는 넣는다(서버와 같게 — 상자는 코드 표라 파일을 읽지 않는다).
	void Engine::BuildWeaponShowcase()
	{
		if (weaponShowcase == kInvalidNode)
			weaponShowcase = scene.AddNode(kInvalidNode, kInvalidMesh, kInvalidMaterial);

		// 이동이 보는 세계와 같은 값(PlayerController::World 와 같은 planet · 지형). 충돌체는 자리 계산에 쓰지 않는다.
		const Shared::MoveWorld world = WeaponMoveWorld();

		// 2026-11-15 — 자리를 멤버에 남긴다: 줍는 출처(UpdateWeapons · FindPickupTarget). 주워도 진열은 그대로다.
		Shared::WeaponShowcaseSlot (&slots)[Shared::kWeaponShowcaseSlotCount] = showcaseSlots;
		Shared::LayoutWeaponShowcase(world, GameMapSpawnPoint(), { 0.0, 0.0, 1.0 }, slots);   // 씬 구성의 controller.Spawn 과 같은 인자
		hasShowcaseSlots = true;
		weaponScene.Bind(&weaponModels, &scene);

		size_t placed = 0;
		for (const Shared::WeaponShowcaseSlot& slot : slots)
		{
			const WeaponView view = slot.firstPerson ? WeaponView::FirstPerson : WeaponView::ThirdPerson;
			const NodeHandle node = weaponModels.Instantiate(slot.type, view, scene, weaponShowcase);
			if (node == kInvalidNode) continue;
			scene.SetLocalTransform(node, ShowcaseTransform(slot));
			++placed;
		}

		size_t colliders = 0;
		if (gameMapCollision.Loaded())
		{
			colliders = Shared::AddWeaponShowcaseColliders(gameMapCollision, world, slots);
			if (colliders > 0)
			{
				gameMapCollision.Finalize();
				colliderView.Clear();
			}
		}

		Log(L"[총] 1인칭 · 3인칭 모델 %zu / %zu 개를 스폰 앞 %.0f m 허공에 띄웠다(윗줄 1인칭 · 아랫줄 3인칭, 왼쪽부터 %S · %S · %S) · 충돌체 %zu 개(상자 묶음)",
			placed, Shared::kWeaponShowcaseSlotCount, Shared::kWeaponShowcaseDistance,
			Shared::SpecOf(Shared::WeaponType::Pistol).name,
			Shared::SpecOf(Shared::WeaponType::LaserRifle).name,
			Shared::SpecOf(Shared::WeaponType::MachineGun).name,
			colliders);
	}

	// ── 총 줍기 · 떨구기 (2026-11-15, Shared/GameLogic/WeaponPickup.h · Client/Weapon/WeaponScene.h) ──
	//  사용자: «주우면 애니메이션 시에 적절한 위치에 총을 고정시키고, 쏠 수 있도록 해줘. 줍기, 떨구기 둘 다 구현하면 되는거야.
	//          이때 공중에 있는것을 줍고, 떨구면 땅바닥에 떨어지도록. 공중에 있는 총은 사라지지 않게»
	//  ★ 접속 중이면 서버가 정한다 — E · G 는 요청만 보내고, 든 총 · 바닥 총은 서버가 알려 준 것으로 그린다(Network).
	//    오프라인이면 같은 Shared 계산으로 혼자 처리한다(offlineHeld · 바닥 총 번호를 클라가 센다).

	Shared::MoveWorld Engine::WeaponMoveWorld() const
	{
		Shared::MoveWorld world;
		world.center = planet.center;
		world.radius = planet.radius;
		world.gravity = planet.gravity;
		world.terrain = planet.terrain;
		return world;
	}

	Shared::WeaponType Engine::HeldWeapon() const
	{
		return network.Connected() ? network.MyWeapon() : offlineHeld;
	}

	// 가장 가까운 총 — 공중 진열(게임 맵에 있을 때) · 바닥 총. 몸통 중심에서 총 원점까지 kWeaponPickupRange 안.
	void Engine::FindPickupTarget()
	{
		pickupTarget = PickupTarget{};
		const Vec3d body = controller.Position();
		double best = Shared::kWeaponPickupRange;

		if (hasShowcaseSlots && !InDebugPlanet())
		{
			for (uint32_t i = 0; i < uint32_t(Shared::kWeaponShowcaseSlotCount); ++i)
			{
				const double d = Length(showcaseSlots[i].position - body);
				if (d > best) continue;
				best = d;
				pickupTarget = { true, Shared::kPickupFromShowcase, i, showcaseSlots[i].type, d };
			}
		}
		for (const WeaponScene::Ground& g : weaponScene.GroundWeapons())
		{
			const double d = Length(WeaponScene::CurrentPose(g).position - body);
			if (d > best) continue;
			best = d;
			pickupTarget = { true, Shared::kPickupFromGround, g.id, g.type, d };
		}
	}

	// 오프라인 떨구기 — 손 자리에서 떨어져 바로 아래 지면에 눕는다(서버 DropHeldWeapon 과 같은 계산).
	void Engine::OfflineDrop()
	{
		if (!Shared::IsWeaponType(offlineHeld)) return;
		const Shared::WeaponDrop drop = Shared::SettleDroppedWeapon(WeaponMoveWorld(), offlineHeld,
			controller.Position(), controller.Up(), controller.Facing());
		weaponScene.SpawnGround(offlineNextGroundId++, offlineHeld, drop.start, drop.rest, drop.fallSeconds);
		while (weaponScene.GroundWeapons().size() > Shared::kMaxGroundWeapons)
			weaponScene.RemoveGround(weaponScene.GroundWeapons().front().id);
		Log(L"[총] %S 떨굼(오프라인) — %.2fs 떨어져 눕는다", Shared::SpecOf(offlineHeld).name, drop.fallSeconds);
		offlineHeld = Shared::WeaponType::None;
	}

	// 오프라인 줍기 — 들고 있으면 먼저 떨군다(한 번에 한 자루). 진열은 그대로, 바닥 총은 없어진다.
	void Engine::OfflinePickup(const PickupTarget& target)
	{
		if (!target.valid || !Shared::IsWeaponType(target.type)) return;
		OfflineDrop();
		if (target.source == Shared::kPickupFromGround)
			weaponScene.RemoveGround(target.id);
		offlineHeld = target.type;
		Log(L"[총] %S 주움(오프라인, %s)", Shared::SpecOf(target.type).name,
			target.source == Shared::kPickupFromShowcase ? L"공중 진열" : L"바닥");
	}

	// 매 프레임 — UpdatePlayer 뒤 · UpdateFire 앞(이번 프레임 든 총으로 쏜다).
	void Engine::UpdateWeapons(float dt)
	{
		const bool online = network.Connected();

		// 1) 서버가 보낸 바닥 총 소식(받은 순서대로). 맵을 바꾸면 Clear 가 먼저 온다.
		if (online)
		{
			network.TakeGroundEvents(groundEventScratch);
			for (const GroundWeaponEvent& e : groundEventScratch)
			{
				switch (e.kind)
				{
				case GroundWeaponEvent::Kind::Clear:
					weaponScene.ClearGround();
					break;
				case GroundWeaponEvent::Kind::Remove:
					weaponScene.RemoveGround(e.groundId);
					break;
				case GroundWeaponEvent::Kind::Spawn:
				{
					auto pose = [](const float p[3], const float y[3], const float z[3])
					{
						Shared::WeaponPose out;
						out.position = Vec3d(p[0], p[1], p[2]);
						out.axisY = Vec3d(y[0], y[1], y[2]);
						out.axisZ = Vec3d(z[0], z[1], z[2]);
						out.axisX = Cross(out.axisY, out.axisZ);   // X = Y × Z (Z = X × Y 의 순환)
						return out;
					};
					weaponScene.SpawnGround(e.groundId, e.type, pose(e.startPos, e.startAxisY, e.startAxisZ),
						pose(e.restPos, e.restAxisY, e.restAxisZ), e.fallSeconds);
					break;
				}
				}
			}
		}

		// 2) 내 손 — 든 총의 1인칭 모델을 내 캐릭터에 붙인다(오른손 자리, Shared::kHeldWeapon*).
		weaponScene.SetLocalHeld(HeldWeapon(), player);

		// 3) 바닥 총 — 떨어지는 것 옮기기, 가까우면 1인칭 · 멀면 3인칭
		weaponScene.Update(dt, controller.Position());

		// 4) 줍기 대상 · 키 — E 줍기, G 떨구기(마우스를 잡은 게임 중에만)
		FindPickupTarget();
		if (!input.Captured()) return;

		// 접속 중이면 서버가 지금 맵에 넣어 준 뒤에만 보낸다(사격 · 입력과 같은 규칙) — 들어가는 중이면 판정할 곳이 없다.
		const bool judged = online && network.JoinedWorld() == CurrentWorld();
		if (input.WasPressed('G') && Shared::IsWeaponType(HeldWeapon()))
		{
			if (judged) network.SendDrop();
			else if (!online) OfflineDrop();
		}
		if (input.WasPressed('E') && pickupTarget.valid)
		{
			if (judged) network.SendPickup(pickupTarget.source, pickupTarget.id);
			else if (!online) OfflinePickup(pickupTarget);
		}
	}

	// 플레이어 주변 배치 오브젝트를 다시 고른다 — 15 m 움직일 때마다(PlanetDebugScene::StreamObjects).
	void Engine::UpdateDebugPlanetObjects()
	{
		if (!InDebugPlanet()) return;
		PlanetDebugScene& target = sceneManager.DebugPlanet(activeDebugPlanet);
		if (target.StreamObjects(scene, controller.Position() - planet.center, false))
			terrainStatus = DebugPlanetStatus(activeDebugPlanet, target);
	}

	// 원격 플레이어 · NPC 노드를 화면 밖으로 치우고 빈 노드 목록으로 돌린다(노드는 지우지 않고 재사용한다).
	void Engine::ParkNetworkNodes()
	{
		const XMMATRIX parked = ParkedTransform();
		for (const auto& entry : remoteNodes)
		{
			scene.SetLocalTransform(entry.second, parked);
			freeRemoteNodes.push_back(entry.second);
		}
		remoteNodes.clear();
		for (const auto& entry : npcNodes)
		{
			scene.SetLocalTransform(entry.second, parked);
			freeNpcNodes.push_back(entry.second);
		}
		npcNodes.clear();

		// 2026-11-15 — 남이 든 총 · 바닥 총도 옛 맵 것이다. 접속 중이면 서버가 새 맵 것을 다시 보낸다(WeaponEquip · GroundWeaponSpawn).
		//  오프라인 바닥 총은 맵마다 따로 두지 않는다 — 맵을 바꾸면 사라진다.
		weaponScene.ClearRemotes();
		weaponScene.ClearGround();
	}

	// ★ 게임 로직 — 명세 2절상 Engine 몫이 아니다.
	//   Input Manager → Class Bridge → Game Logic → Player (6.6.3) 로 옮길 대상이다.
	void Engine::UpdatePlayer(float dt)
	{
		camera.SetAiming(input.Captured() && input.MouseDown(1));
		if (input.Captured())
		{
			const float scale = kMouseSensitivity * camera.LookScale();
			camera.AddLook(input.MouseDeltaX() * scale, input.MouseDeltaY() * scale);
		}

		controller.Update(dt, input, camera);
		camera.SetSprinting(controller.IsSprinting());
		camera.Update(dt, controller.Position(), controller.Up(), planet);

		scene.SetLocalTransform(player, controller.WorldMatrix());
	}

	// ★ 게임 로직 — 명세 2절상 Engine 몫이 아니다.
	//   입력은 Class Bridge 를 거쳐야 하고(6.6.3), 이펙트는 표시 계층으로 가야 한다.
	//
	//  ★ 2026-11-15 든 총으로만 쏜다 — 단발(권총 · 레이저 라이플) = 누른 순간, 연발(기관총) = 누르고 있는 동안 발 간격마다.
	//    빈손이면 쏘지 않는다. (그 전: 총 없이 누른 순간 한 발, 몸통 중심에서)
	//  ★ 클라는 «어디서 어디로» 만 보낸다. 맞았는지는 서버가 정한다 (명세 18절 원칙 4).
	//    그래서 예광탄은 «맞은 곳» 이 아니라 고정 길이로 그린다. 서버 판정과 무관하다.
	//    총구 = 손에 든 총의 Muzzle 자리. 1인칭으로 바꾸면 이 원점만 눈 위치로 옮기면 된다.
	void Engine::UpdateFire(float dt)
	{
		// 1) 보이는 예광탄의 수명을 줄이고, 끝난 것은 치운다.
		for (size_t i = 0; i < tracers.size(); )
		{
			tracers[i].life -= dt;
			if (tracers[i].life > 0.0f) { ++i; continue; }

			scene.SetLocalTransform(tracers[i].node, ParkedTransform());
			freeTracerNodes.push_back(tracers[i].node);

			tracers[i] = tracers.back();
			tracers.pop_back();
		}

		// 2) 이번 프레임에 쐈는가.
		//  ★ 2026-11-15 든 총으로만 쏜다(사용자 «주우면 … 쏠 수 있도록») — 빈손이면 쏘지 않는다.
		//    발사 모드 · 발 간격은 그 총의 표(Shared::SpecOf): 단발 = 누른 순간, 연발 = 누르고 있는 동안 간격마다.
		//    ★ 같은 날 사용자 «눌려있다면 해당 속도에 맞게 자동 발사되게 하자» — 세 총 모두 연발(표의 fireMode = Auto).
		//    ★ 남은 시간을 이어 쓴다(같은 날) — 쏜 뒤 간격을 «더한다». 예전에는 간격으로 «되돌려» 프레임 끝에 남은 시간이 버려졌고,
		//      발 간격이 프레임 수로 올림돼 느려졌다(기관총 0.08 s → 60 fps 0.083 · 30 fps 0.1 s, 라이플 0.25 → 30 fps 0.267 s).
		//      이제 프레임마다 간격은 들쭉날쭉해도 평균은 표의 간격과 같다. 프레임이 간격보다 길면 한 프레임에 여러 발(kMaxShotsPerFrame 까지).
		//      손을 떼면 시간을 모아 두지 않는다(0 에서 멈춘다) — 쉬었다가 몰아 쏘기는 안 된다.
		//    서버도 같은 평균으로 걸러 낸다(Server OnPlayerFire 연사 예산). 탄창 · 장전은 아직 없다(탄 무한).
		//    ★ 같은 날 사용자 «권총은 자동 발사 없애주고, 클릭 시에만 발사. 그리고 연속 클릭이 들어오면 0.2초마다 게이트를 열어서
		//      딜레이를 줄 수 있게해줘» — 권총만 단발(Single)로 되돌림. 단발은 «게이트»:
		//      · 게이트가 열려 있으면(대기 0) 클릭한 프레임에 바로 한 발, 그리고 게이트를 발 간격(권총 0.2 s) 동안 닫는다.
		//      · 닫혀 있는 동안 들어온 클릭은 버린다 — 다시 눌러야 한다(같은 날 사용자 «0.2 게이트 닫힌 사이에 들어온 클릭 신호는 버려줘».
		//        잠깐 «한 발 맡아 두었다가 열리면 쏘기» 였다가 바꿈). 그래서 연타해도 초당 5발을 넘지 못한다.
		fireCooldown -= dt;
		const Shared::WeaponType held = HeldWeapon();
		const Shared::WeaponSpec& spec = Shared::SpecOf(held);   // 빈손이면 모든 값이 0 인 빈 줄
		const bool trigger = spec.fireMode == Shared::FireMode::Auto ? input.MouseDown(0) : input.MouseWasPressed(0);
		if (!Shared::IsWeaponType(held) || spec.fireInterval <= 0.0f ||
			!input.Captured() || suppressFire || !trigger || fireCooldown > kFireGateTolerance)   // 게이트가 닫혀 있으면 이번 클릭은 버려진다
		{
			fireCooldown = (std::max)(0.0f, fireCooldown);
			return;
		}

		// 총구 = 손에 든 총의 Muzzle 자리(그려지는 총과 같은 자세 — Shared::HeldWeaponPose).
		//  ★ 2026-11-15 방향 = 총구 → 겨눈 점(AimDirection). 예전에는 조준선과 «평행» 이라 총구가 눈에서 떨어진 만큼
		//    (오른쪽 · 위로 1 ~ 2 m) 크로스헤어에서 비껴 갔다(노션 「사격 시스템 설명」 4.1). 서버는 받은 원점 · 방향으로 판정하므로 그대로다.
		const Vec3d muzzle = Shared::MuzzlePosition(
			Shared::HeldWeaponPose(controller.Position(), controller.Up(), controller.Facing()), held);
		const Vec3d aim = AimDirection(muzzle, double(spec.range));
		const float origin[3] = { float(muzzle.x), float(muzzle.y), float(muzzle.z) };
		const float dir[3] = { float(aim.x), float(aim.y), float(aim.z) };

		// 효과는 접속 여부와 무관하게 보인다. 판정만 서버 몫이다.
		// 서버가 지금 맵에 넣어 준 뒤에만 보낸다(2026-11-01 맵 번호) — 들어가는 중 · 서버에 맵이 없으면 판정할 곳이 없다.
		const bool send = network.Connected() && network.JoinedWorld() == CurrentWorld();
		// 예광탄 — 그 총의 tracerEvery 발마다 하나(2026-11-15 부터 세 총 모두 매 발)
		//  ★ 2026-11-16 사용자 «예광탄은 여전히 언덕을 뚫고 생기는데 이것도 수정해줘» — 예전에는 늘 120 m 막대라
		//    겨눈 점이 언덕 · 벽이어도 그 너머까지 그려졌다. 이제 총구에서 이 발의 선(총구 → 겨눈 점)이 처음 닿는 곳까지만 —
		//    서버 판정과 같은 함수(TraceShot)로 같은 원점 · 방향을 본다. 몸이면 몸통 중심 가까이까지. 한 프레임의 발은 원점 · 방향이 같아 한 번만 잰다.
		const uint32_t every = spec.tracerEvery > 0 ? spec.tracerEvery : 1;
		const Shared::ShotHit along = TraceVisibleShot(muzzle, aim,
			(std::min)(double(spec.range), double(kTracerStart) + double(kTracerLength)), 0.0);
		const double tracerEnd = along.kind == Shared::ShotHitKind::Body ? along.closest : along.distance;
		const float tracerLength = float((std::min)(double(kTracerLength), tracerEnd - double(kTracerStart)));
		const uint32_t maxShots = spec.fireMode == Shared::FireMode::Auto ? kMaxShotsPerFrame : 1u;
		for (uint32_t shot = 0; shot < maxShots && fireCooldown <= kFireGateTolerance; ++shot)
		{
			// 연발은 남은 시간을 이어 쓴다(평균 = 표의 간격). 단발은 쏜 순간부터 꼭 간격만큼 닫는다 —
			//  클릭은 «게이트가 열린 뒤» 언제 와도 되므로 이어 쓸 시간이 없고, 이어 쓰면 프레임 하나만큼 일찍 열린다(0.2 → 0.194 s).
			if (spec.fireMode == Shared::FireMode::Auto)
				fireCooldown += spec.fireInterval;
			else
				fireCooldown = spec.fireInterval;
			if (send)
				network.SendFire(origin, dir);
			if (shotsFired++ % every == 0)
				SpawnTracer(muzzle, aim, tracerLength);
		}
		fireCooldown = (std::max)(0.0f, fireCooldown);   // 상한에 걸려 남은 발은 버린다(다음 프레임에 몰아 쏘지 않게)
	}

	// ── 크로스헤어 맞추기 — 겨눈 점 (2026-11-15) ──
	//  ① 카메라 눈에서 조준선(화면 가운데)으로 선을 긋는다. 카메라와 캐릭터 사이(어깨 뒤)는 보지 않는다 —
	//     조준선이 총구 옆을 지나는 거리(tStart)부터 «tStart + 사거리» 까지.
	//  ② 그 선이 처음 닿는 것 — Shared::TraceShot(서버 ResolveFire 와 같은 함수, 2026-11-15 사용자 «벽에 막히는건 당연히 막혀야지»):
	//     몸(다른 플레이어 · NPC — 서버와 같은 구, 화면에 그린 자리) · 충돌체(건물 · 바위 · 소품 · 나무 · 총 진열) · 지형.
	//     몸이면 그 몸통 중심에 가장 가까워지는 점을, 충돌체 · 지형이면 닿은 점을 겨눈다.
	//  ③ 아무것도 없으면 사거리 끝 점. 총구 → 겨눈 점이 너무 가깝거나 총구 뒤쪽이면 조준선 방향 그대로.
	//  ★ 전부 CPU(메인 스레드, 쏘는 발마다 한 번). 서버도 총구에서 같은 함수로 판정하므로 벽을 겨누면 벽에 막힌다.
	Vec3d Engine::AimDirection(const Vec3d& muzzle, double range) const
	{
		const XMFLOAT3& e = camera.EyePosition();
		const Vec3d eye{ double(e.x), double(e.y), double(e.z) };
		const Vec3d look = camera.LookDirection();

		// 카메라와 캐릭터 사이(어깨 뒤)는 보지 않는다 — 조준선이 총구 옆을 지나는 거리부터 «+ 사거리» 까지.
		const double tStart = (std::max)(0.0, Dot(muzzle - eye, look));

		// 땅을 겨누면 실제 지면을 겨눈다(지형 허용 0 — 서버 판정만 0.5 m 봐준다)
		const Shared::ShotHit hit = TraceVisibleShot(eye, look, tStart + (std::max)(range, 1.0), tStart);
		const double aimT = hit.kind == Shared::ShotHitKind::Body ? hit.closest : hit.distance;

		// 총구 → 겨눈 점
		const Vec3d toAim = (eye + look * aimT) - muzzle;
		const double length = Length(toAim);
		if (length < kAimMinDistance) return look;
		const Vec3d dir = toAim * (1.0 / length);
		return Dot(dir, look) > 0.0 ? dir : look;
	}

	Shared::ShotHit Engine::TraceVisibleShot(const Vec3d& origin, const Vec3d& direction,
		double maxDistance, double minDistance) const
	{
		// 몸 — 다른 플레이어 · NPC 의 그려진 자리(UpdateNetwork 가 채운 지난 프레임 값).
		//  연결이 끊기면 목록을 더 안 채워 옛 값이 남으므로 연결돼 있을 때만 넣는다.
		std::vector<Shared::ShotTarget> targets;
		if (network.Connected())
		{
			targets.reserve(remoteViews.size() + npcViews.size());
			for (const RemoteView& v : remoteViews)
				targets.push_back({ Vec3d{ double(v.pos[0]), double(v.pos[1]), double(v.pos[2]) }, v.playerId });
			for (const NpcView& v : npcViews)
				targets.push_back({ Vec3d{ double(v.pos[0]), double(v.pos[1]), double(v.pos[2]) }, v.npcId });
		}

		// 충돌체 · 지형은 이동이 보는 세계와 같다(지금 맵 — 게임 맵 · 예시 행성)
		Shared::MoveWorld world = WeaponMoveWorld();
		const Shared::CollisionWorld* collision = ActiveCollision();
		world.collision = (collision && collision->Loaded()) ? collision : nullptr;

		Shared::ShotOptions options;
		options.minDistance = minDistance;
		options.terrainTolerance = 0.0;   // 실제 지면 — 화면에 보이는 땅에서 멈춘다
		return Shared::TraceShot(world, origin, direction, maxDistance, targets.data(), targets.size(), options);
	}

	// 발사선 위에 상자를 놓는다. +Z 를 조준 방향에 맞추고 Z 만 길이만큼 늘린다.
	//  2026-11-16 — 길이는 부르는 쪽이 정한다(처음 닿는 곳까지, kTracerLength 까지). 너무 짧으면 그리지 않는다.
	void Engine::SpawnTracer(const Vec3d& muzzle, const Vec3d& aim, float length)
	{
		if (tracerMesh == kInvalidMesh || !(length >= kTracerMinLength)) return;

		NodeHandle node = kInvalidNode;

		if (!freeTracerNodes.empty())
		{
			node = freeTracerNodes.back();
			freeTracerNodes.pop_back();
		}
		else if (!tracers.empty())
		{
			// 풀이 다 차 있다 = 가장 오래된 것을 빼서 다시 쓴다.
			node = tracers.front().node;
			tracers.erase(tracers.begin());
		}
		else
		{
			return;
		}

		// 조준 방향을 Z 축으로 하는 기저.
		//  ★ 2026-11-15 — 방향이 «총구 → 겨눈 점» 이 되어 발밑을 겨누면 거의 수직이 될 수 있다 → up 과 나란하면 앞쪽 축으로 대신한다.
		const Vec3d zAxis = aim;
		Vec3d xRaw = Cross(controller.Up(), zAxis);
		if (Length(xRaw) < 1.0e-4)
			xRaw = Cross(controller.Facing(), zAxis);
		const Vec3d xAxis = Normalize(xRaw);
		const Vec3d yAxis = Cross(zAxis, xAxis);

		const Vec3d center = muzzle + aim * (double(kTracerStart) + double(length) * 0.5);

		XMMATRIX m;
		m.r[0] = XMVectorSet(float(xAxis.x), float(xAxis.y), float(xAxis.z), 0.0f);
		m.r[1] = XMVectorSet(float(yAxis.x), float(yAxis.y), float(yAxis.z), 0.0f);
		m.r[2] = XMVectorSet(float(zAxis.x) * length,
			float(zAxis.y) * length,
			float(zAxis.z) * length, 0.0f);
		m.r[3] = XMVectorSet(float(center.x), float(center.y), float(center.z), 1.0f);

		scene.SetLocalTransform(node, m);

		Tracer t;
		t.node = node;
		t.life = kTracerLife;
		tracers.push_back(t);
	}

	// ★ 게임 로직 — 명세 2절상 Engine 몫이 아니다.
	//   송신은 Class Bridge → Network(9절), 수신은 Network → State Manager → OtherPlayer · NPC(6.13) 로 옮길 대상이다.
	void Engine::UpdateNetwork(float dt)
	{
		if (!network.Connected())
			return;

		// ★ 맵 번호 (2026-11-01, 사용자 «맵번호 적용해줘») — 서버가 지금 맵(CurrentWorld)에 넣어 준 뒤에만 입력을 보내고 판정을 받는다.
		//   행성을 고르면 HandleDebugPlanetKeys 가 JoinWorld 를 보냈다. 답(WorldJoined)이 오기 전 · 서버가 맵을 못 읽었으면 혼자 움직인다(예측만).
		const uint32_t world = CurrentWorld();

		// ★ B안(입력 기반, 2026-10-29) — 좌표가 아니라 이번 프레임에 돈 고정 틱의 입력을 보낸다(틱마다 하나, 초당 60개).
		//   서버가 같은 이동 코드 · 같은 충돌체로 판정한다(명세 6.13 ②, 18절 원칙 4).
		if (network.JoinedWorld() == world)
			for (const Shared::MoveCommand& command : controller.FrameCommands())
				network.SendInput(command);

		//서버가 뿌린 다른 플레이어의 좌표와 내 판정 상태를 받는다. 논블로킹이라 즉시 돌아온다.
		network.Poll();

		// ── 서버가 이 맵에 넣었다 (2026-11-01) ──
		//  서버가 정한 자리 · 방향에 다시 선다 — 되감기 기록을 끊으므로, 답을 기다리는 동안 혼자 움직인 것은 버린다.
		//  이 뒤로 받는 판정 · 원격 좌표는 모두 이 맵 것이다(Network 가 들어가는 중의 옛 맵 패킷을 버렸다).
		Network::WorldJoin join;
		if (network.TakeWorldJoined(world, join))
		{
			if (join.ok)
			{
				controller.Spawn(Vec3d(double(join.pos[0]), double(join.pos[1]), double(join.pos[2])),
					Vec3d(double(join.facing[0]), double(join.facing[1]), double(join.facing[2])));
				spawnPosition = controller.Position();
				camera.SnapTo(controller.Position(), controller.Up(), controller.Facing());
				Log(L"[맵] 서버가 맵 %u(%s)에 넣었다 — 이제 서버가 이 맵을 판정한다", world, WorldLabel(world));
			}
			else
			{
				Log(L"[맵] 서버가 맵 %u(%s)을 준비하지 못했다 — 이 맵에서는 혼자 움직인다(서버 콘솔 참고)", world, WorldLabel(world));
			}
		}

		// ── 서버 판정으로 보정 (명세 6.13 ③ · 멀티스레딩 명세 §3.2 ②) ──
		//  서버가 마지막으로 처리한 입력 뒤의 내 예측과 벌어졌으면 서버 상태로 되감고 남은 입력을 다시 적용한다.
		//  서버가 이 맵에 넣어 주지 않았으면(들어가는 중 · 실패) 보정 · 리스폰을 하지 않는다 — 판정이 없다.
		const bool judged = network.JoinedWorld() == world;
		Shared::PlayerStatePacket serverState;
		if (judged && network.TakePlayerState(serverState))
		{
			ServerMoveState state;
			state.lastSeq = serverState.lastSeq;
			std::copy(std::begin(serverState.pos), std::end(serverState.pos), state.pos);
			std::copy(std::begin(serverState.velocity), std::end(serverState.velocity), state.velocity);
			std::copy(std::begin(serverState.facing), std::end(serverState.facing), state.facing);
			state.verticalSpeed = serverState.verticalSpeed;
			state.flags = serverState.flags;
			const double moved = controller.Reconcile(state);
			if (moved > 0.5)
				Log(L"[이동] 서버 보정 %.2fm (입력 %u, 누계 %u회)", moved, serverState.lastSeq, controller.Corrections());
		}

		// ── 리스폰 ──────────────────────────────────────────
		//  ★ 클라가 스스로 되살아나지 않는다. 체력이 0 인지도 서버가 판정하고,
		//    돌아갈 좌표도 서버가 보낸 것을 그대로 쓴다 (명세 18절 원칙 4).
		float respawn[3];
		if (judged && network.TakeRespawn(respawn))
		{
			controller.Spawn(Vec3d(double(respawn[0]), double(respawn[1]), double(respawn[2])),
				controller.Facing());
			camera.SnapTo(controller.Position(), controller.Up(), controller.Facing());
		}

		const XMMATRIX parkedTransform = ParkedTransform();

		// ── 원격 플레이어 노드 갱신 ─────────────────────
		//  ★ 노드를 지우지 않고 재사용한다
		//    Scene 에 노드 삭제 API 가 없다. 나갈 때마다 새로 만들면
		//    접속·퇴장을 반복하는 동안 노드가 계속 쌓인다.
		//    나간 노드는 화면 밖으로 치워 두었다가 다음 사람에게 다시 쓴다.
		network.RemotePlayers(remoteViews);

		// 이번 프레임 목록에 없는 = 나간 플레이어의 노드를 회수한다.
		for (std::unordered_map<uint32_t, NodeHandle>::iterator it = remoteNodes.begin();
			it != remoteNodes.end(); )
		{
			bool alive = false;
			for (size_t i = 0; i < remoteViews.size(); ++i)
			{
				if (remoteViews[i].playerId == it->first) { alive = true; break; }
			}

			if (alive) { ++it; continue; }

			scene.SetLocalTransform(it->second, parkedTransform);
			freeRemoteNodes.push_back(it->second);
			it = remoteNodes.erase(it);
		}

		// 보이는 플레이어를 그 자리에 놓는다. 처음 보는 번호면 노드를 하나 붙인다.
		//  2026-11-15 — 그 사람이 든 총(서버 WeaponEquip)을 3인칭 모델로 같은 손 자리에 놓는다. 이번에 안 보인 사람의 총은 치운다.
		weaponScene.BeginRemoteFrame();
		for (size_t i = 0; i < remoteViews.size(); ++i)
		{
			const RemoteView& v = remoteViews[i];

			std::unordered_map<uint32_t, NodeHandle>::iterator found =
				remoteNodes.find(v.playerId);

			if (found == remoteNodes.end())
			{
				NodeHandle handle;
				if (!freeRemoteNodes.empty())
				{
					handle = freeRemoteNodes.back();
					freeRemoteNodes.pop_back();
				}
				else
				{
					handle = characterModel.Instantiate(scene);
				}
				found = remoteNodes.emplace(v.playerId, handle).first;
			}

			const XMMATRIX character = CharacterTransform(planet, v.pos);
			scene.SetLocalTransform(found->second, character);
			weaponScene.PlaceRemote(v.playerId, network.WeaponOf(v.playerId), character);
		}
		weaponScene.EndRemoteFrame();

		// ── NPC 노드 갱신 ───────────────────────────────
		//  행동 계산은 전부 서버가 한다. 클라는 좌표를 받아 그리기만 하므로
		//  위 원격 플레이어 갱신과 같은 절차다. 다른 것은 메시 색뿐이다.
		network.Npcs(npcViews);

		// 이번 프레임 목록에 없는 NPC 의 노드를 회수한다.
		for (std::unordered_map<uint32_t, NodeHandle>::iterator it = npcNodes.begin();
			it != npcNodes.end(); )
		{
			bool alive = false;
			for (size_t i = 0; i < npcViews.size(); ++i)
			{
				if (npcViews[i].npcId == it->first) { alive = true; break; }
			}

			if (alive) { ++it; continue; }

			scene.SetLocalTransform(it->second, parkedTransform);
			freeNpcNodes.push_back(it->second);
			it = npcNodes.erase(it);
		}

		for (size_t i = 0; i < npcViews.size(); ++i)
		{
			const NpcView& v = npcViews[i];

			std::unordered_map<uint32_t, NodeHandle>::iterator found =
				npcNodes.find(v.npcId);

			if (found == npcNodes.end())
			{
				NodeHandle handle;
				if (!freeNpcNodes.empty())
				{
					handle = freeNpcNodes.back();
					freeNpcNodes.pop_back();
				}
				else
				{
					// NPC 도 같은 모델·재질을 쓴다(client2 이식 방침). 눈으로 가리는 것은
					// 머리 위 HP BAR 로 한다 — 교수님 09-15 ToDo.
					handle = characterModel.Instantiate(scene);
				}
				found = npcNodes.emplace(v.npcId, handle).first;
			}

			// 고도까지 서버가 지형으로 정해 보낸다. 받은 좌표를 그대로 그린다.
			scene.SetLocalTransform(found->second,
				CharacterTransform(planet, v.pos));
		}
	}

	// 렌더 추출(멀티스레딩 3.2 ④) → Renderer
	void Engine::RenderFrame()
	{
		scene.UpdateWorldTransforms();
		scene.Extract(packet);
		packet.view = RenderView{ };
		packet.view.viewProj = camera.ViewProj();
		packet.view.eyePosition = camera.EyePosition();
		// 2026-10-27 렌더 6단계 — 하늘 · 해 높이의 기준(행성 위 플레이어 자리의 위)
		packet.view.up = XMFLOAT3(float(controller.Up().x), float(controller.Up().y), float(controller.Up().z));

		uiSprites.clear();
		sceneManager.ExtractUI(uiSprites);

		// 임시 크로스헤어 (2026-11-14) — 화면 한가운데 = 조준선(명세 20절). 텍스처 1 텍셀 = 화면 1 px.
		if (crosshairTexture != kInvalidTexture)
		{
			UISprite crosshair;
			crosshair.texture = crosshairTexture;
			crosshair.center = XMFLOAT2(float(renderer.Width()) * 0.5f, float(renderer.Height()) * 0.5f);
			crosshair.halfSize = XMFLOAT2(float(kCrosshairTexels) * 0.5f, float(kCrosshairTexels) * 0.5f);
			uiSprites.push_back(crosshair);
		}

		renderer.BeginFrame();
		skyPlanet.Render(renderer, camera, kHeight);
		renderer.Render(packet);
		// C 키 충돌체 와이어프레임 — 메인 패스 뒤 · UI 앞(2026-10-29). 선은 맵 공간이라 원점 = 행성 중심(월드).
		if (showColliders)
		{
			colliderView.Update(ActiveCollision(), controller.Position() - planet.center, false);

			// 캐릭터 충돌 캡슐 — 매 프레임 그 자리에(2026-10-29 사용자 «플레이어 충돌 캡슐도 그려줘, npc도»).
			//  내 플레이어 = 그릴 자세(모델과 같은 자리). 다른 플레이어 · NPC = 모델을 놓은 것과 같은 받은 좌표.
			//  서버가 지금 맵에 넣어 주지 않았으면(들어가는 중 · 실패) 원격 목록이 비어 있거나 옛 맵 것이라 내 것만 그린다(2026-11-01 맵 번호).
			colliderView.ClearCapsules();
			colliderView.AddCapsule(controller.Position() - planet.center, controller.Up(), ColliderDebugView::kPlayerColor);
			if (network.Connected() && network.JoinedWorld() == CurrentWorld())
			{
				for (const RemoteView& v : remoteViews)
				{
					const Vec3d p{ v.pos[0], v.pos[1], v.pos[2] };
					colliderView.AddCapsule(p - planet.center, planet.Up(p), ColliderDebugView::kRemotePlayerColor);
				}
				for (const NpcView& v : npcViews)
				{
					const Vec3d p{ v.pos[0], v.pos[1], v.pos[2] };
					colliderView.AddCapsule(p - planet.center, planet.Up(p), ColliderDebugView::kNpcColor);
				}
			}
			renderer.DrawDebugLines(packet.view,
				XMFLOAT3(float(planet.center.x), float(planet.center.y), float(planet.center.z)), colliderView.Vertices());
		}
		renderer.DrawSprites(uiSprites);
		renderer.EndFrame();
	}

	// 로딩 씬 — 3D 는 그리지 않고 UI(배경 + 스피너)만 그린다.
	void Engine::RenderLoading()
	{
		uiSprites.clear();
		sceneManager.ExtractUI(uiSprites);

		renderer.BeginFrame();
		renderer.DrawSprites(uiSprites);
		renderer.EndFrame();
	}

	// 로딩 진행을 창 제목으로 확인 — 화면에는 글자를 그릴 수단이 아직 없다.
	void Engine::UpdateLoadingTitle(float dt)
	{
		titleTimer += dt;
		if (titleTimer < 0.25f)
			return;
		titleTimer = 0.0f;

		const LoadBatch* batch = sceneManager.CurrentBatch();
		if (!batch)
			return;

		wchar_t title[256];
		swprintf_s(title, L"SpaceWar   로딩 중  %zu/%zu  (%.0f%%)  %s",
			batch->FinishedCount(), batch->TaskCount(), batch->Progress() * 100.0f,
			batch->CurrentLabel().c_str());
		SetWindowText(hwnd, title);
	}

	// 델타타임 / 하이브리드 상태를 창 제목으로 확인
	void Engine::UpdateTitle(float dt)
	{
		// 게임 기능을 사용한 뒤에도 상주량이 계속 늘어나는지 출력창에서 비교한다.
		memoryLogTimer += dt;
		if (memoryLogTimer >= 30.0f)
		{
			memoryLogTimer = 0.0f;
			LogMemory(L"runtime", resources.ModelCount());
		}
		titleTimer += dt;
		if (titleTimer < 0.5f)
			return;
		titleTimer = 0.0f;

		const RayTracingParams& rt = renderer.GetRayTracingParams();
		// 장치가 못 하는 것과 실행 중 R 키로 끈 것을 구분해서 보여준다.
		//  (2026-10-27 렌더 2단계 — 임시 스위치 kEnableRaytracing 이 없어져 «임시끔(래스터만)» 상태도 없앴다)
		const wchar_t* rtState = !renderer.SupportsRaytracing() ? L"미지원"
			: (rt.enabled ? L"ON" : L"OFF");

		// 이동 검증용: 고도 / 접지 / 실제 스폰에서의 거리
		const Vec3d& p = controller.Position();
		const double distFromSpawn = Length(p - spawnPosition);

		// 네트워크 상태 — 보낸 수 / 에코 받은 수 / 마지막 에코 좌표
		wchar_t netText[240];
		if (network.Connected())
		{
			// 체력은 서버가 보내준 값만 쓴다. 아직 못 받았으면 물음표.
			wchar_t healthText[24];
			if (network.MyHealth() >= 0.0f)
				swprintf_s(healthText, L"%.0f", network.MyHealth());
			else
				swprintf_s(healthText, L"?");

			// 2026-11-01 맵 번호 — 서버가 넣어 준 맵(들어가는 중 · 서버에 맵 없음)
			wchar_t worldText[48];
			if (network.Joining())
				swprintf_s(worldText, L"맵 들어가는 중");
			else if (network.JoinFailed())
				swprintf_s(worldText, L"서버에 맵 없음(혼자)");
			else
				swprintf_s(worldText, L"맵 %u %s", network.JoinedWorld(), WorldLabel(network.JoinedWorld()));

			// 2026-10-29 B안 — 송신 = 입력 수, 판정 = 받은 서버 상태 수, 보정 = 되감은 횟수(마지막 거리)
			swprintf_s(netText,
				L"나=%u  %s  체력 %s  피격 %u  입력 %u  판정 %u  보정 %u회(%.2fm)  다른플레이어 %u명  NPC %u마리",
				network.MyId(), worldText, healthText, network.HitCount(),
				network.SentCount(), network.StateCount(),
				controller.Corrections(), controller.LastCorrection(),
				network.RemoteCount(), network.NpcCount());
		}
		else
		{
			swprintf_s(netText, L"%s", netStatus.c_str());
		}

		// 가속 구조 계측 — scratch 는 빌드 뒤 해제되므로 로드가 끝나면 0 이어야 정상이다.
		wchar_t blasText[128];
		swprintf_s(blasText,
			L"BLAS %zu개 %.0fMB(압축 -%.0fMB, scratch %.0fMB)  TLAS %u/%u%s",
			renderer.BlasCount(),
			double(renderer.BlasResultBytes())     / (1024.0 * 1024.0),
			double(renderer.BlasCompactionSaved()) / (1024.0 * 1024.0),
			double(renderer.BlasScratchBytes())    / (1024.0 * 1024.0),
			renderer.TlasInstanceCount(), renderer.TlasMaxInstances(),
			renderer.TlasDroppedInstances() ? L" ★유실" : L"");

		// 래스터 제출량 (client2 2f15728) — 프러스텀 컬링으로 빠진 메시 수와 제출 삼각형.
		//  Draw = 인스턴싱 묶음(실제 DrawIndexedInstanced 횟수), Inst = 그 안의 인스턴스 합.
		const RenderStats& renderStats = renderer.Stats();

		// C 키 충돌체 와이어프레임 (2026-10-29) — 켰을 때만 «그린 충돌체 수 · 선 수»
		wchar_t colliderText[128] = L"";
		if (showColliders)
		{
			const Shared::CollisionWorld* world = ActiveCollision();
			if (world && world->Loaded())
				swprintf_s(colliderText, L"  |  충돌체(C) %zu개 · 캡슐 %zu · 선 %zu%s", colliderView.InstanceCount(),
					colliderView.CapsuleCount(), colliderView.LineCount(), colliderView.Truncated() ? L" ★먼 것 잘림" : L"");
			else
				swprintf_s(colliderText, L"  |  충돌체(C) 없음 · 캡슐 %zu", colliderView.CapsuleCount());
		}

		// 총 (2026-11-15) — 든 총 · 줍기 안내(E) · 떨구기 안내(G) · 바닥 총 수
		wchar_t weaponText[200];
		{
			const Shared::WeaponType held = HeldWeapon();
			wchar_t heldText[64];
			if (Shared::IsWeaponType(held))
				swprintf_s(heldText, L"총 %S(G 떨구기)", Shared::SpecOf(held).name);
			else
				swprintf_s(heldText, L"빈손");
			wchar_t pickText[96] = L"";
			if (pickupTarget.valid)
				swprintf_s(pickText, L"  E 줍기: %S(%.1fm)", Shared::SpecOf(pickupTarget.type).name, pickupTarget.distance);
			swprintf_s(weaponText, L"%s%s  바닥 총 %zu", heldText, pickText, weaponScene.GroundWeapons().size());
		}

		wchar_t title[1280];
		swprintf_s(title,
			L"SpaceWar   FPS %.0f  dt %.1fms  |  고도 %.2fm  %s  스폰거리 %.0fm  속도 %.1f  "
			L"|  %s  |  %s  |  %s  |  RT %s view %u  |  %s  "
			L"|  행성 LOD%u %.0fpx  |  Draw %zu Inst %zu/%zu Cull %zu Tri %.2fM%s%s",
			timer.Fps(), dt * 1000.0f,
			controller.Altitude(), controller.IsGrounded() ? L"접지" : L"공중",
			distFromSpawn, controller.Speed(),
			weaponText,
			netText,
			terrainStatus.c_str(),
			rtState, renderer.DebugMode(),
			blasText,
			skyPlanet.SelectedLod(), skyPlanet.DiameterPixels(),
			renderStats.drawCalls, renderStats.instances, renderStats.submitted, renderStats.culled,
			double(renderStats.triangles) / 1000000.0,
			renderStats.dropped ? L" ★인스턴스 유실" : L"",
			colliderText);
		SetWindowText(hwnd, title);
	}

} // namespace swc
