#include "ShaderCompiler.h"
#include <windows.h>
#include <dxcapi.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace swc {

	ShaderCompiler::ShaderCompiler()
	{
		dll = LoadLibraryW(L"dxcompiler.dll");
		if (dll)
			createInstance = GetProcAddress(static_cast<HMODULE>(dll), "DxcCreateInstance");

		if (!createInstance && dll)
		{
			FreeLibrary(static_cast<HMODULE>(dll));
			dll = nullptr;
		}
	}

	ShaderCompiler::~ShaderCompiler()
	{
		if (dll)
			FreeLibrary(static_cast<HMODULE>(dll));
	}

	std::vector<uint8_t> ShaderCompiler::CompileFromFile(const wchar_t* path, const wchar_t* entry,
		const wchar_t* target, const wchar_t* const* defines, size_t defineCount, std::string& log)
	{
		std::vector<uint8_t> result;

		if (!createInstance)
		{
			log = "dxcompiler.dll 을 로드하지 못했습니다 (exe 옆에 있어야 합니다).";
			return result;
		}

		auto create = reinterpret_cast<DxcCreateInstanceProc>(createInstance);

		ComPtr<IDxcUtils> utils;
		ComPtr<IDxcCompiler3> compiler;
		if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
			FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
		{
			log = "DXC 인스턴스를 만들지 못했습니다.";
			return result;
		}

		ComPtr<IDxcBlobEncoding> source;
		if (FAILED(utils->LoadFile(path, nullptr, &source)) || !source)
		{
			log = "셰이더 파일을 열 수 없습니다.";
			return result;
		}

		ComPtr<IDxcIncludeHandler> includeHandler;
		utils->CreateDefaultIncludeHandler(&includeHandler);

		// ── #include 경로 (2026-10-27, 파이프라인 1단계) ──
		//  소스를 파일 이름 없이 버퍼로 넘기므로, DXC 는 따옴표 include 를 «작업 디렉터리» 기준으로 찾는다.
		//  VS 에서 F5 로 띄우면 작업 디렉터리가 exe 옆이 아니라서 공용 .hlsli 를 못 찾는다.
		//  → 셰이더 파일이 있는 폴더(exe 옆 Shaders\)를 -I 로 넘겨, 어디서 실행해도 같은 폴더에서 찾게 한다.
		std::wstring includeDir(path);
		const size_t slash = includeDir.find_last_of(L"\\/");
		includeDir = (slash == std::wstring::npos) ? std::wstring(L".") : includeDir.substr(0, slash);

		// 행렬은 DXC 기본(열 우선)을 유지한다 — CPU 에서 전치해 올리는 기존 규약과 맞춘다.
		std::vector<LPCWSTR> args;
		// ★ 2026-10-27 렌더 4단계 — DXR 셰이더 라이브러리(lib_6_x)는 진입점 하나가 아니라 [shader("…")] 들을 내보낸다.
		//   entry 가 비어 있으면 -E 를 넘기지 않는다.
		if (entry && entry[0] != L'\0')
		{
			args.push_back(L"-E"); args.push_back(entry);
		}
		args.push_back(L"-T"); args.push_back(target);
		args.push_back(L"-I"); args.push_back(includeDir.c_str());
		for (size_t i = 0; i < defineCount; ++i)
		{
			args.push_back(L"-D");
			args.push_back(defines[i]);
		}
#if defined(_DEBUG)
		args.push_back(L"-Zi");
		args.push_back(L"-Qembed_debug");
		args.push_back(L"-Od");
#else
		args.push_back(L"-O3");
#endif

		DxcBuffer buffer = {};
		buffer.Ptr = source->GetBufferPointer();
		buffer.Size = source->GetBufferSize();
		buffer.Encoding = DXC_CP_ACP;

		ComPtr<IDxcResult> compiled;
		if (FAILED(compiler->Compile(&buffer, args.data(), static_cast<UINT32>(args.size()),
			includeHandler.Get(), IID_PPV_ARGS(&compiled))))
		{
			log = "DXC Compile 호출이 실패했습니다.";
			return result;
		}

		ComPtr<IDxcBlobUtf8> errors;
		if (SUCCEEDED(compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
			errors && errors->GetStringLength() > 0)
			log = errors->GetStringPointer();

		HRESULT status = E_FAIL;
		compiled->GetStatus(&status);
		if (FAILED(status))
			return result;

		ComPtr<IDxcBlob> object;
		if (FAILED(compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
		{
			log = "DXIL 출력이 없습니다.";
			return result;
		}

		result.resize(object->GetBufferSize());
		memcpy(result.data(), object->GetBufferPointer(), result.size());
		return result;
	}
}
