#include "TextureLoader.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl.h>
#include <limits>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace swc {

	namespace {

		bool CreateFactory(ComPtr<IWICImagingFactory>& factory, std::wstring& error)
		{
			const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
				CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
			if (FAILED(hr))
			{
				error = hr == CO_E_NOTINITIALIZED
					? L"COM 초기화가 필요합니다."
					: L"WIC 팩토리 생성 실패.";
				return false;
			}
			return true;
		}

		// 디코더에서 첫 프레임을 RGBA8 로 꺼낸다. 파일·메모리 경로가 공유한다.
		bool DecodeFirstFrame(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
			bool srgb, const wchar_t* label, TextureData& out, std::wstring& error)
		{
			auto fail = [&](const wchar_t* reason)
			{
				error = std::wstring(reason) + L"\n" + (label ? label : L"");
				return false;
			};

			ComPtr<IWICBitmapFrameDecode> frame;
			if (FAILED(decoder->GetFrame(0, &frame)))
				return fail(L"텍스처 프레임 열기 실패.");

			UINT width = 0, height = 0;
			if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
				width > 16384 || height > 16384)
				return fail(L"텍스처 크기가 D3D12 의 2D 텍스처 범위를 벗어납니다.");

			const uint64_t bytes = uint64_t(width) * height * 4;
			if (bytes > (std::numeric_limits<UINT>::max)())
				return fail(L"텍스처 픽셀 버퍼가 너무 큽니다.");

			ComPtr<IWICFormatConverter> converter;
			if (FAILED(factory->CreateFormatConverter(&converter)) ||
				FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
					WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
				return fail(L"텍스처 RGBA8 변환 실패.");

			TextureData data;
			data.path = label ? label : L"";
			data.width = width;
			data.height = height;
			data.srgb = srgb;
			data.pixels.resize(static_cast<size_t>(bytes));
			if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes), data.pixels.data())))
				return fail(L"텍스처 픽셀 읽기 실패.");

			out = std::move(data);
			return true;
		}

	} // namespace

	bool LoadTextureImage(const wchar_t* path, bool srgb, TextureData& out, std::wstring& error)
	{
		error.clear();
		if (!path || !*path)
		{
			error = L"텍스처 경로가 비어 있습니다.";
			return false;
		}

		ComPtr<IWICImagingFactory> factory;
		if (!CreateFactory(factory, error)) return false;

		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
			WICDecodeMetadataCacheOnDemand, &decoder)))
		{
			error = std::wstring(L"텍스처 이미지 열기 실패.\n") + path;
			return false;
		}

		return DecodeFirstFrame(factory.Get(), decoder.Get(), srgb, path, out, error);
	}

	bool LoadTextureImageFromMemory(const uint8_t* bytes, size_t size, bool srgb,
		const wchar_t* label, TextureData& out, std::wstring& error)
	{
		error.clear();
		if (!bytes || size == 0)
		{
			error = L"내장 텍스처 바이트가 비어 있습니다.";
			return false;
		}
		if (size > (std::numeric_limits<DWORD>::max)())
		{
			error = L"내장 텍스처가 너무 큽니다.";
			return false;
		}

		ComPtr<IWICImagingFactory> factory;
		if (!CreateFactory(factory, error)) return false;

		// ★ WIC 스트림은 포인터를 들고만 있다 — 디코딩이 끝날 때까지 bytes 가 살아 있어야 한다.
		//   픽셀을 복사해 나가는 DecodeFirstFrame 안에서 끝나므로 호출자 버퍼로 충분하다.
		ComPtr<IWICStream> stream;
		if (FAILED(factory->CreateStream(&stream)) ||
			FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(size))))
		{
			error = L"내장 텍스처 스트림 생성 실패.";
			return false;
		}

		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
			WICDecodeMetadataCacheOnDemand, &decoder)))
		{
			error = std::wstring(L"내장 텍스처 형식을 알 수 없습니다(PNG·JPEG 만).\n")
				+ (label ? label : L"");
			return false;
		}

		return DecodeFirstFrame(factory.Get(), decoder.Get(), srgb, label, out, error);
	}
}
