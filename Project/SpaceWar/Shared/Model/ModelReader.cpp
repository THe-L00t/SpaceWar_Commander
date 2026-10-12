#include "ModelReader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "MeshPostProcess.h"
#include "Readers/GltfReader.h"
#include "Readers/ObjReader.h"

namespace Shared {

	namespace {

		namespace fs = std::filesystem;

		// 이 메시의 재질에 노멀맵이 있는가. 탄젠트는 노멀맵을 쓸 때만 필요하다.
		bool UsesNormalMap(const ModelSource& source, const SourceMesh& mesh)
		{
			if (mesh.material == kInvalidIndex || mesh.material >= source.materials.size())
				return false;
			const SourceMaterial& material = source.materials[mesh.material];
			const size_t slot = static_cast<size_t>(SourceTextureSlot::Normal);
			return !material.texturePaths[slot].empty() || material.embeddedImages[slot] != kInvalidIndex;
		}

		// 파일 앞부분과 확장자를 읽어 둔다. 판별에 두 가지를 같이 쓴다 —
		// .glb 는 매직이 있지만 .gltf·.obj(텍스트)는 확장자로 가려야 한다.
		struct Probe
		{
			uint8_t     head[ModelReader::kProbeBytes] = {};
			size_t      headSize = 0;
			std::string extension;   // 소문자, 점 포함 (".obj")
			bool        ok = false;
		};

		Probe ReadProbe(const wchar_t* path)
		{
			Probe probe;
			if (!path || !*path) return probe;

			std::error_code ec;
			const fs::path full = fs::absolute(path, ec);
			if (ec) return probe;

			probe.extension = full.extension().string();
			std::transform(probe.extension.begin(), probe.extension.end(),
				probe.extension.begin(), [](unsigned char c) { return char(::tolower(c)); });

			FILE* file = nullptr;
			if (::_wfopen_s(&file, path, L"rb") != 0 || !file)
				return probe;

			probe.headSize = ::fread(probe.head, 1, sizeof(probe.head), file);
			::fclose(file);

			probe.ok = true;
			return probe;
		}

		const wchar_t* FormatName(ModelFormat format)
		{
			switch (format)
			{
			case ModelFormat::Gltf: return L"glTF";
			case ModelFormat::Obj:  return L"OBJ";
			default:                return L"알 수 없음";
			}
		}

	} // namespace

	ModelReader::ModelReader()
	{
		// ★ 등록 순서 = 판별 우선순위
		//   매직이 뚜렷한 포맷을 앞에 둔다. 확장자로만 가리는 포맷이 뒤에 온다.
		readers.push_back(std::make_unique<GltfReader>());
		readers.push_back(std::make_unique<ObjReader>());
	}

	ModelReader::~ModelReader() = default;

	ModelFormat ModelReader::Detect(const wchar_t* path)
	{
		const Probe probe = ReadProbe(path);
		if (!probe.ok) return ModelFormat::Unknown;

		// .glb — 'glTF' 매직
		if (probe.headSize >= 4 &&
			probe.head[0] == 'g' && probe.head[1] == 'l' &&
			probe.head[2] == 'T' && probe.head[3] == 'F')
			return ModelFormat::Gltf;

		if (probe.extension == ".glb" || probe.extension == ".gltf") return ModelFormat::Gltf;
		if (probe.extension == ".obj")  return ModelFormat::Obj;

		return ModelFormat::Unknown;
	}

	bool ModelReader::Load(const wchar_t* path, ModelSource& out, std::wstring& error,
		const ReadOptions& options)
	{
		error.clear();
		out = ModelSource{};

		if (!path || !*path)
		{
			error = L"모델 경로가 비어 있습니다.";
			return false;
		}

		const Probe probe = ReadProbe(path);
		if (!probe.ok)
		{
			error = L"모델 파일을 열 수 없습니다.\n";
			error += path;
			return false;
		}

		std::error_code ec;
		out.sourceDirectory = fs::absolute(path, ec).parent_path().wstring();

		// ── 1) 포맷에 맞는 리더 찾기 ────────────────────────
		IModelReader* chosen = nullptr;
		for (std::unique_ptr<IModelReader>& reader : readers)
		{
			if (reader->Matches(probe.head, probe.headSize, probe.extension.c_str()))
			{
				chosen = reader.get();
				break;
			}
		}

		if (!chosen)
		{
			const ModelFormat detected = Detect(path);
			error = L"읽을 수 없는 모델 포맷입니다 (판별: ";
			error += FormatName(detected);
			error += L"). 지원: glTF 2.0(.glb/.gltf) · OBJ(.obj)";
			return false;
		}

		// ── 2) 파일에 적힌 것만 꺼낸다 ──────────────────────
		if (!chosen->Read(path, options, out, error))
		{
			if (error.empty()) error = L"모델을 읽지 못했습니다.";
			return false;
		}
		out.format = chosen->Format();

		// ── 3) 공통 검사와 후처리 ───────────────────────────
		//  애니메이션 클립만 든 파일(메시 0개)도 있다 — 그때는 메시 후처리를 건너뛴다.
		if (!ValidateModelSource(out, error))
			return false;

		for (SourceMesh& mesh : out.meshes)
		{
			GenerateNormals(mesh);
			// ★ 노멀맵이 없는 재질이면 탄젠트를 만들지 않는다 (2026-10-09)
			//   셰이더는 hasNormalMap 일 때만 탄젠트를 쓴다(GRenderer CreateMaterial). 행성 MTL 은
			//   텍스처가 하나도 없어 정점 1천만 개분 임시 배열(24B/정점)과 계산이 통째로 빠진다.
			if (options.generateTangents && !options.geometryOnly && UsesNormalMap(out, mesh))
				GenerateTangents(mesh);
		}

		BuildBoundsAndCollision(out, options.generateCollision);
		return true;
	}

} // namespace Shared
