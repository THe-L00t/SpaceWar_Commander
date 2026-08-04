#pragma once
#include <string>
#include <vector>
#include <cstdint>

// DXC 런타임 컴파일러 래퍼 (dxcompiler.dll 동적 로드).
// DXR 인라인 레이트레이싱(RayQuery)은 SM 6.5 이상이 필요하고, fxc 로는 컴파일되지 않는다.
namespace swc {
	class ShaderCompiler
	{
	public:
		ShaderCompiler();
		~ShaderCompiler();

		ShaderCompiler(const ShaderCompiler&) = delete;
		ShaderCompiler& operator=(const ShaderCompiler&) = delete;

		bool Available() const { return createInstance != nullptr; }

		// 성공 시 DXIL 바이트코드, 실패 시 빈 벡터. 경고·오류 메시지는 log 에 담긴다.
		std::vector<uint8_t> CompileFromFile(const wchar_t* path, const wchar_t* entry,
			const wchar_t* target, const wchar_t* const* defines, size_t defineCount,
			std::string& log);

	private:
		void* dll = nullptr;
		void* createInstance = nullptr;
	};
}
