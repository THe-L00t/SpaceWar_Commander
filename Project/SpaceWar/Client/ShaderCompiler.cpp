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

		// 행렬은 DXC 기본(열 우선)을 유지한다 — CPU 에서 전치해 올리는 기존 규약과 맞춘다.
		std::vector<LPCWSTR> args;
		args.push_back(L"-E"); args.push_back(entry);
		args.push_back(L"-T"); args.push_back(target);
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
