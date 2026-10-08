#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <cstdint>
#include <DirectXMath.h>

// ============================================================
//  BLAS(메쉬당 1개, 최초 1회) + TLAS(매 프레임 재빌드).
//  렌더러 내부 전용 헤더 — 게임 로직은 절대 include 하지 않는다.
//
//  ★ 2026-10-08 — BLAS 메모리 구조 수정
//    행성 모델(삼각형 791만)을 넣고 보니 가속 구조가 상주 메모리의 큰 몫을 차지했다.
//    두 가지가 낭비였다:
//      ① scratch 버퍼를 빌드가 끝난 뒤에도 들고 있었다. scratch 는 보통 결과와 비슷한 크기라
//         «쓰지 않는 메모리» 가 결과만큼 남아 있었다 → 빌드 완료 후 해제한다.
//      ② 압축(ALLOW_COMPACTION + COPY_MODE_COMPACT)을 쓰지 않았다. 정적 지오메트리는
//         보통 20~50% 줄어든다 → 빌드 뒤 압축 크기를 질의해 더 작으면 옮겨 담는다.
//
//    쓰는 순서 (GRenderer::CreateMesh 가 이 순서를 지킨다)
//      AddMesh(cmd)        빌드 + 압축 크기 질의를 기록
//      → 커맨드 제출·GPU 대기
//      CompactMesh(cmd)    압축 복사를 기록 (이득이 없으면 아무것도 안 한다)
//      → 커맨드 제출·GPU 대기
//      FinishCompaction()  원본·scratch 해제. 여기서 메모리가 실제로 줄어든다
// ============================================================

namespace swc {
	class AccelStructure
	{
	public:
		bool Initialize(ID3D12Device5*, uint32_t maxInstances);

		// 메쉬 하나의 BLAS 를 빌드 (cmd 에 기록만 하고, 실행·대기는 호출자 책임)
		uint32_t AddMesh(ID3D12Device5*, ID3D12GraphicsCommandList4*,
			const D3D12_RAYTRACING_GEOMETRY_DESC&);

		// AddMesh 의 커맨드가 GPU 에서 끝난 뒤에 부른다. 압축이 이득이면 복사를 기록한다.
		bool CompactMesh(ID3D12Device5*, ID3D12GraphicsCommandList4*, uint32_t blasIndex);

		// CompactMesh 의 커맨드가 끝난 뒤에 부른다. 압축 원본과 scratch 를 해제한다.
		void FinishCompaction(uint32_t blasIndex);

		void ResetInstances() { instanceCount = 0; }
		void AddInstance(uint32_t blasIndex, const DirectX::XMFLOAT4X4& world);
		void BuildTlas(ID3D12GraphicsCommandList4*);

		D3D12_GPU_VIRTUAL_ADDRESS TlasAddress() const;
		uint32_t InstanceCount() const { return instanceCount; }

		// 계측용 — 창 제목·로그에 찍어 «BLAS 가 얼마나 먹는지» 를 눈으로 본다.
		uint64_t BlasResultBytes() const;
		uint64_t BlasScratchBytes() const;
		uint64_t BlasCompactionSaved() const { return compactionSaved; }
		size_t   BlasCount() const { return blas.size(); }

		static constexpr uint32_t kInvalidBlas = 0xFFFFFFFFu;

	private:
		struct Blas
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> result;
			Microsoft::WRL::ComPtr<ID3D12Resource> scratch;   // 빌드 뒤 해제된다
			Microsoft::WRL::ComPtr<ID3D12Resource> compacted; // 압축 대상 (성공하면 result 와 교체)
			uint64_t resultBytes = 0;
			uint64_t scratchBytes = 0;
		};

		std::vector<Blas> blas;
		Microsoft::WRL::ComPtr<ID3D12Resource> tlasResult;
		Microsoft::WRL::ComPtr<ID3D12Resource> tlasScratch;
		Microsoft::WRL::ComPtr<ID3D12Resource> instanceBuffer;

		// 압축 크기 질의용. UAV 로 쓴 뒤 READBACK 으로 복사해 CPU 가 읽는다.
		// 메쉬 하나를 만들 때마다 GPU 대기가 끼므로 하나를 돌려 쓴다.
		Microsoft::WRL::ComPtr<ID3D12Resource> postbuildWrite;
		Microsoft::WRL::ComPtr<ID3D12Resource> postbuildRead;

		D3D12_RAYTRACING_INSTANCE_DESC* instanceData = nullptr;   // UPLOAD 힙 영속 매핑
		uint32_t instanceCount = 0;
		uint32_t maxInstances = 0;
		uint64_t compactionSaved = 0;   // 압축으로 줄인 누적 바이트 (계측)
	};
}
