#include "BackdropFactory.hpp"
#include "DcompBackdrop.hpp"
#include <Shlwapi.h>

#include "AeroBackdrop.hpp"
#include "BlurBackdrop.hpp"
#include "AcrylicBackdrop.hpp"
#include "MicaBackdrop.hpp"
#include "LiquidGlassBackdrop.hpp"

namespace MDWMBlurGlassExt
{
	effectType g_type{ effectType::Blur };

	wuc::CompositionSurfaceBrush g_materialTextureBrush{ nullptr };
	wuc::CompositionBrush g_rimNormalMapBrush{ nullptr };
	std::chrono::steady_clock::time_point g_currentTimeStamp{};
	std::unordered_map<DWORD, wuc::CompositionBrush> g_backdropActiveBrushMap{};
	std::unordered_map<DWORD, wuc::CompositionBrush> g_backdropInactiveBrushMap{};

	wuc::CompositionSurfaceBrush CreateMaterialTextureBrush()
	{
		com_ptr<DCompPrivate::IDCompositionDesktopDevicePartner> dcompDevice{ nullptr };
		copy_from_abi(dcompDevice, DWM::CDesktopManager::s_pDesktopManagerInstance->GetDCompositionInteropDevice());
		auto compositor{ dcompDevice.as<wuc::Compositor>() };

		winrt::Windows::UI::Composition::CompositionGraphicsDevice graphicsDevice{ nullptr };
		THROW_IF_FAILED(
			compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>()->CreateGraphicsDevice(
				DWM::CDesktopManager::s_pDesktopManagerInstance->GetD2DDevice(),
				reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(graphicsDevice))
			)
		);
		auto compositionSurface
		{
			graphicsDevice.CreateDrawingSurface(
				{ 256.f, 256.f },
				winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R16G16B16A16Float,
				winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied
			)
		};
		auto noiceBrush{ compositor.CreateSurfaceBrush(compositionSurface) };

		wil::unique_hmodule wuxcModule{ LoadLibraryExW(L"Windows.UI.Xaml.Controls.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE) };
		THROW_LAST_ERROR_IF_NULL(wuxcModule);
		auto resourceHandle{ FindResourceW(wuxcModule.get(), MAKEINTRESOURCE(2000), RT_RCDATA) };
		THROW_LAST_ERROR_IF_NULL(resourceHandle);
		auto globalHandle{ LoadResource(wuxcModule.get(), resourceHandle) };
		THROW_LAST_ERROR_IF_NULL(globalHandle);
		auto cleanUp = wil::scope_exit([&]
			{
				if (globalHandle)
				{
					UnlockResource(globalHandle);
					FreeResource(globalHandle);
				}
			});
		DWORD resourceSize{ SizeofResource(wuxcModule.get(), resourceHandle) };
		THROW_LAST_ERROR_IF(resourceSize == 0);
		auto resourceAddress{ reinterpret_cast<PBYTE>(LockResource(globalHandle)) };
		com_ptr<IStream> stream{ SHCreateMemStream(resourceAddress, resourceSize), winrt::take_ownership_from_abi };
		THROW_LAST_ERROR_IF_NULL(stream);

		com_ptr<IWICImagingFactory2> wicFactory{ nullptr };
		wicFactory.copy_from(DWM::CDesktopManager::s_pDesktopManagerInstance->GetWICFactory());
		com_ptr<IWICBitmapDecoder> wicDecoder{ nullptr };
		THROW_IF_FAILED(wicFactory->CreateDecoderFromStream(stream.get(), &GUID_VendorMicrosoft, WICDecodeMetadataCacheOnDemand, wicDecoder.put()));
		com_ptr<IWICBitmapFrameDecode> wicFrame{ nullptr };
		THROW_IF_FAILED(wicDecoder->GetFrame(0, wicFrame.put()));
		com_ptr<IWICFormatConverter> wicConverter{ nullptr };
		THROW_IF_FAILED(wicFactory->CreateFormatConverter(wicConverter.put()));
		com_ptr<IWICPalette> wicPalette{ nullptr };
		THROW_IF_FAILED(
			wicConverter->Initialize(
				wicFrame.get(),
				GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone,
				wicPalette.get(),
				0, WICBitmapPaletteTypeCustom
			)
		);
		com_ptr<IWICBitmap> wicBitmap{ nullptr };
		THROW_IF_FAILED(wicFactory->CreateBitmapFromSource(wicConverter.get(), WICBitmapCreateCacheOption::WICBitmapNoCache, wicBitmap.put()));

		auto drawingSurfaceInterop{ compositionSurface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>() };
		POINT offset = { 0, 0 };
		com_ptr<ID2D1DeviceContext> d2dContext{ nullptr };
		THROW_IF_FAILED(
			drawingSurfaceInterop->BeginDraw(nullptr, IID_PPV_ARGS(d2dContext.put()), &offset)
		);
		d2dContext->Clear();
		com_ptr<ID2D1Bitmap1> d2dBitmap{ nullptr };
		d2dContext->CreateBitmapFromWicBitmap(
			wicBitmap.get(),
			D2D1::BitmapProperties1(
				D2D1_BITMAP_OPTIONS_NONE,
				D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
			),
			d2dBitmap.put()
		);
		d2dContext->DrawBitmap(d2dBitmap.get());
		THROW_IF_FAILED(
			drawingSurfaceInterop->EndDraw()
		);

		return noiceBrush;
	}

	// Builds a small normal-map texture whose interior is neutral (no displacement)
	// and whose border ramps the normal outward over 'rimThickness' pixels, then
	// wraps it in a nine-grid brush so the refracting rim keeps a constant pixel
	// width no matter how large the window is. Consumed by LiquidGlassBackdrop.
	wuc::CompositionBrush CreateRimNormalMapBrush(int rimThickness)
	{
		rimThickness = std::clamp(rimThickness, 1, 64);

		com_ptr<DCompPrivate::IDCompositionDesktopDevicePartner> dcompDevice{ nullptr };
		copy_from_abi(dcompDevice, DWM::CDesktopManager::s_pDesktopManagerInstance->GetDCompositionInteropDevice());
		auto compositor{ dcompDevice.as<wuc::Compositor>() };

		winrt::Windows::UI::Composition::CompositionGraphicsDevice graphicsDevice{ nullptr };
		THROW_IF_FAILED(
			compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>()->CreateGraphicsDevice(
				DWM::CDesktopManager::s_pDesktopManagerInstance->GetD2DDevice(),
				reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(graphicsDevice))
			)
		);

		// 1px neutral centre + rimThickness border on each side; nine-grid insets
		// equal rimThickness so only the border is preserved and the centre stretches.
		const int side{ rimThickness * 2 + 1 };
		auto compositionSurface
		{
			graphicsDevice.CreateDrawingSurface(
				{ static_cast<float>(side), static_cast<float>(side) },
				winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
				winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied
			)
		};

		// CPU-fill the normal map. Channel 0.5 (=128) means "no displacement".
		// Left edge pushes +X (sample rightward => magnify inward), right edge -X;
		// top pushes +Y, bottom -Y. Ramp smoothly to neutral at the inner rim edge.
		std::vector<uint32_t> pixels(static_cast<size_t>(side) * side);
		auto encode = [](float nx, float ny) -> uint32_t
		{
			auto toByte = [](float n) -> uint32_t
			{
				float v = std::clamp(0.5f + n * 0.5f, 0.f, 1.f);
				return static_cast<uint32_t>(v * 255.f + 0.5f);
			};
			uint32_t r = toByte(nx);
			uint32_t g = toByte(ny);
			// premultiplied BGRA, opaque: A=255, RGB unchanged
			return (0xFFu << 24) | (r << 16) | (g << 8) | 0x00u;
		};
		for (int y = 0; y < side; ++y)
		{
			for (int x = 0; x < side; ++x)
			{
				// signed distance into the rim from each edge, 0..1 (1 at outer edge)
				float leftT   = x < rimThickness ? (rimThickness - x) / static_cast<float>(rimThickness) : 0.f;
				float rightT  = x >= side - rimThickness ? (x - (side - 1 - rimThickness)) / static_cast<float>(rimThickness) : 0.f;
				float topT    = y < rimThickness ? (rimThickness - y) / static_cast<float>(rimThickness) : 0.f;
				float bottomT = y >= side - rimThickness ? (y - (side - 1 - rimThickness)) / static_cast<float>(rimThickness) : 0.f;
				// square the profile for a lens-like curve concentrated at the very edge
				auto curve = [](float t) { return t * t; };
				float nx = curve(leftT) - curve(rightT);
				float ny = curve(topT) - curve(bottomT);
				pixels[static_cast<size_t>(y) * side + x] = encode(nx, ny);
			}
		}

		auto drawingSurfaceInterop{ compositionSurface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>() };
		POINT offset{ 0, 0 };
		com_ptr<ID2D1DeviceContext> d2dContext{ nullptr };
		THROW_IF_FAILED(
			drawingSurfaceInterop->BeginDraw(nullptr, IID_PPV_ARGS(d2dContext.put()), &offset)
		);
		d2dContext->Clear();
		com_ptr<ID2D1Bitmap1> d2dBitmap{ nullptr };
		D2D1_SIZE_U bmpSize{ static_cast<UINT32>(side), static_cast<UINT32>(side) };
		THROW_IF_FAILED(
			d2dContext->CreateBitmap(
				bmpSize,
				pixels.data(),
				static_cast<UINT32>(side * sizeof(uint32_t)),
				D2D1::BitmapProperties1(
					D2D1_BITMAP_OPTIONS_NONE,
					D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
				),
				d2dBitmap.put()
			)
		);
		d2dContext->DrawBitmap(
			d2dBitmap.get(),
			D2D1::RectF(static_cast<float>(offset.x), static_cast<float>(offset.y),
				static_cast<float>(offset.x + side), static_cast<float>(offset.y + side))
		);
		THROW_IF_FAILED(drawingSurfaceInterop->EndDraw());

		auto surfaceBrush{ compositor.CreateSurfaceBrush(compositionSurface) };
		auto nineGrid{ compositor.CreateNineGridBrush() };
		nineGrid.Source(surfaceBrush);
		nineGrid.SetInsets(static_cast<float>(rimThickness));
		nineGrid.IsCenterHollow(false);
		return nineGrid;
	}

	wuc::CompositionBrush BackdropFactory::GetOrCreateBackdropBrush(
		const wuc::Compositor& compositor,
		DWORD color,
		bool active,
		DWM::ACCENT_POLICY* policy
	)
	{
		// these backdrops has no difference other than color
		if (g_type != effectType::Aero)
		{
			active = false;
		}

		auto& map{ active ? g_backdropActiveBrushMap : g_backdropInactiveBrushMap };
		auto it{ map.find(color) };
		// find cached brush
		if (it != map.end())
		{
			if (it->second.Compositor() == compositor)
			{
				return it->second;
			}
			else
			{
				Shutdown();
			}
		}

		wuc::CompositionBrush brush{ nullptr };
		if (policy)
		{
			color = policy->nColor | 0xFF000000;
			if (policy->nAccentState == 3 && !(policy->nFlags & 2) && os::buildNumber < 22621)
			{
				color = 0;
			}

			if (
				policy->nAccentState == 4 &&
				policy->nColor == 0
				)
			{
				brush = compositor.CreateColorBrush({});
			}
			if (policy->nAccentState == 2)
			{
				brush = compositor.CreateColorBrush(MakeWinrtColor(policy->nColor));
			}
			if (policy->nAccentState == 1)
			{
				brush = compositor.CreateColorBrush(MakeWinrtColor(color));
			}

			if (brush)
			{
				return brush;
			}
		}

		auto winrtColor{ MakeWinrtColor(color) };
		auto glassOpacity{ policy ? static_cast<float>(policy->nColor >> 24 & 0xFF) / 255.f : 1.f };
		switch (g_type)
		{
		case effectType::Blur:
		{
			brush = BlurBackdrop::CreateBrush(
				compositor,
				winrtColor,
				glassOpacity,
				g_configData.customBlurAmount
			);
			break;
		}
		case effectType::Aero:
		{
			winrtColor = MakeWinrtColor(color, true);
			brush = AeroBackdrop::CreateBrush(
				compositor,
				winrtColor,
				winrtColor,
				policy ? static_cast<float>(policy->nColor >> 24 & 0xFF) / 255.f : 
				(active ? g_configData.aeroColorBalance : g_configData.aeroColorBalance * 0.4f),
				g_configData.aeroAfterglowBalance,
				active ? g_configData.aeroBlurBalance : 0.4f * g_configData.aeroBlurBalance + 0.6f,
				g_configData.customBlurAmount
			);
			break;
		}
		case effectType::Acrylic:
		case effectType::Mica:
		{
			auto useLuminosity
			{
				policy &&
				os::buildNumber >= 22000 &&
				(policy->nFlags & 2) != 0 &&
				(
					(policy->nAccentState == 3 && os::buildNumber > 22000) ||
					(policy->nAccentState == 4)
				)
			};
			auto luminosity{ policy ? (useLuminosity ? std::optional{ 1.03f } : std::nullopt) : g_configData.luminosityOpacity };
			if (g_type == effectType::Acrylic)
			{
				brush = AcrylicBackdrop::CreateBrush(
					compositor,
					g_materialTextureBrush,
					AcrylicBackdrop::GetEffectiveTintColor(winrtColor, glassOpacity, luminosity),
					AcrylicBackdrop::GetEffectiveLuminosityColor(winrtColor, glassOpacity, luminosity),
					g_configData.customBlurAmount,
					0.02f
				);
			}
			else
			{
				brush = MicaBackdrop::CreateBrush(
					compositor,
					AcrylicBackdrop::GetEffectiveTintColor(winrtColor, glassOpacity, luminosity),
					AcrylicBackdrop::GetEffectiveLuminosityColor(winrtColor, glassOpacity, luminosity),
					glassOpacity,
					g_configData.luminosityOpacity
				);
			}
			break;
		}
		case effectType::LiquidGlass:
		{
			if (!g_rimNormalMapBrush)
				g_rimNormalMapBrush = CreateRimNormalMapBrush(g_configData.glassRimThickness);
			brush = LiquidGlassBackdrop::CreateBrush(
				compositor,
				g_rimNormalMapBrush,
				winrtColor,
				glassOpacity,
				g_configData.customBlurAmount,
				g_configData.glassRefractionAmount
			);
			break;
		}
		default:
			brush = compositor.CreateColorBrush(MakeWinrtColor(policy ? policy->nColor : color));
			break;
		}

		if (!policy)
		{
			map.insert_or_assign(color, brush);
		}

		return brush;
	}

	std::chrono::steady_clock::time_point BackdropFactory::GetBackdropBrushTimeStamp()
	{
		return g_currentTimeStamp;
	}

	void BackdropFactory::Shutdown()
	{
		g_backdropActiveBrushMap.clear();
		g_backdropInactiveBrushMap.clear();
		g_materialTextureBrush = nullptr;
		g_rimNormalMapBrush = nullptr;
	}

	void BackdropFactory::RefreshConfig()
	{
		if (!g_materialTextureBrush)
		{
			g_materialTextureBrush = CreateMaterialTextureBrush();
		}

		// rebuild the rim map on next use so a changed rim thickness takes effect
		g_rimNormalMapBrush = nullptr;

		g_type = g_configData.effectType;
		// mica is not available in windows 10
		if (os::buildNumber < 22000 && g_type == effectType::Mica)
		{
			g_type = effectType::Blur;
		}

		g_backdropActiveBrushMap.clear();
		g_backdropInactiveBrushMap.clear();
		g_currentTimeStamp = std::chrono::steady_clock::now();
	}
}
