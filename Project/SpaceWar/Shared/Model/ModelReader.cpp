#include "ModelReader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "MeshPostProcess.h"

// 포맷별 리더는 여기에 추가한다 (설계안 8장 올리기 순서: OBJ → .swm → glTF).
// #include "Readers/ObjReader.h"
// #include "Readers/SwmReader.h"
// #include "Readers/GltfReader.h"

namespace Shared {

	namespace {

		namespace fs = std::filesystem;

		// 파일 앞부분과 확장자를 읽어 둔다. 판별에 두 가지를 같이 쓴다 —
		// .swm·glb 는 매직이 있지만 obj·gltf(텍스트)는 확장자로 가려야 한다.
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
			case ModelFormat::Swm:  return L".swm";
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
		// readers.push_back(std::make_unique<SwmReader>());
		// readers.push_back(std::make_unique<GltfReader>());
		// readers.push_back(std::make_unique<ObjReader>());
	}

	ModelReader::~ModelReader() = default;

	ModelFormat ModelReader::Detect(const wchar_t* path)
	{
		const Probe probe = ReadProbe(path);
		if (!probe.ok) return ModelFormat::Unknown;

		// .swm — 매직 4바이트
		if (probe.headSize >= sizeof(uint32_t))
		{
			uint32_t magic = 0;
			::memcpy(&magic, probe.head, sizeof(magic));
			if (magic == swm::kMagic) return ModelFormat::Swm;
		}

		// .glb — 'glTF' 매직
		if (probe.headSize >= 4 &&
			probe.head[0] == 'g' && probe.head[1] == 'l' &&
			probe.head[2] == 'T' && probe.head[3] == 'F')
			return ModelFormat::Gltf;

		if (probe.extension == ".swm")  return ModelFormat::Swm;
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
		out.sourceDirectory = fs::absolute(path, ec).parent_path().string();

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
			error = L"아직 읽을 수 없는 모델 포맷입니다 (판별: ";
			error += FormatName(detected);
			error += L"). 지원 예정: .swm · glTF(.glb/.gltf) · OBJ";
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
		if (!ValidateModelSource(out, error))
			return false;

		for (SourceMesh& mesh : out.meshes)
		{
			GenerateNormals(mesh);
			if (options.generateTangents && !options.geometryOnly)
				GenerateTangents(mesh);
		}

		BuildBoundsAndCollision(out, options.generateCollision);
		return true;
	}

} // namespace Shared
