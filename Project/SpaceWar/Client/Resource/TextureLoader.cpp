#include "TextureLoader.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl.h>
#include <limits>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace swc {

	bool LoadTextureImage(const wchar_t* path, bool srgb, TextureData& out, std::wstring& error)
	{
		error.clear();
		if (!path || !*path)
		{
			error = L"?ìŠ¤ì²?ê²½ë¡œê°€ ë¹„ì–´ ?ˆìŠµ?ˆë‹¤.";
			return false;
		}

		auto fail = [&](const wchar_t* reason)
		{
			error = std::wstring(reason) + L"\n" + path;
			return false;
		};

		ComPtr<IWICImagingFactory> factory;
		HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if (FAILED(hr))
			return fail(hr == CO_E_NOTINITIALIZED ? L"COM ì´ˆê¸°?”ê? ?„ìš”?©ë‹ˆ??" : L"WIC ?©í† ë¦??ì„± ?¤íŒ¨.");

		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
			WICDecodeMetadataCacheOnDemand, &decoder)))
			return fail(L"?ìŠ¤ì²??´ë?ì§€ ?´ê¸° ?¤íŒ¨.");

		ComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame)))
			return fail(L"?ìŠ¤ì²??„ë ˆ???½ê¸° ?¤íŒ¨.");

		UINT width = 0, height = 0;
		if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
			width > 16384 || height > 16384)
			return fail(L"?ìŠ¤ì²??¬ê¸°ê°€ D3D12??2D ?ìŠ¤ì²?ë²”ìœ„ë¥?ë²—ì–´?©ë‹ˆ??");

		const uint64_t bytes = uint64_t(width) * height * 4;
		if (bytes > (std::numeric_limits<UINT>::max)())
			return fail(L"?ìŠ¤ì²??½ì? ë²„í¼ê°€ ?ˆë¬´ ?½ë‹ˆ??");

		ComPtr<IWICFormatConverter> converter;
		if (FAILED(factory->CreateFormatConverter(&converter)) ||
			FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
				WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
			return fail(L"?ìŠ¤ì²?RGBA8 ë³€???¤íŒ¨.");

		TextureData data;
		data.path = path;
		data.width = width;
		data.height = height;
		data.srgb = srgb;
		data.pixels.resize(static_cast<size_t>(bytes));
		if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes), data.pixels.data())))
			return fail(L"?ìŠ¤ì²??½ì? ?½ê¸° ?¤íŒ¨.");

		out = std::move(data);
		return true;
	}
}
