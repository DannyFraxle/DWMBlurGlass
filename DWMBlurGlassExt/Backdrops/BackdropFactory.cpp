#include "BackdropFactory.hpp"
#include "DcompBackdrop.hpp"
#include <Shlwapi.h>

#include "AeroBackdrop.hpp"
#include "BlurBackdrop.hpp"
#include "AcrylicBackdrop.hpp"
#include "MicaBackdrop.hpp"
#include "LiquidGlassBackdrop.hpp"
#include "../Helper/ReconLog.h"
#include "../Effects/ProbeEffect.hpp"

namespace MDWMBlurGlassExt
{
	effectType g_type{ effectType::Blur };

	wuc::CompositionSurfaceBrush g_materialTextureBrush{ nullptr };
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

	// Asks Composition which D2D effects it will actually build, and writes the
	// answer to effects.log. Runs once, off the render path, only when
	// glassProbeEffects is set in config.ini.
	//
	// This exists because guessing costs a dwm restart per guess, and because the
	// error messages cannot be taken at face value: DistantSpecular is refused
	// with "Invalid effect input" even when its input is a self-contained colour
	// source, so only an actual construction attempt settles the question.
	void ProbeEffectSupport(const wuc::Compositor& compositor)
	{
		struct Candidate
		{
			const GUID* clsid;
			const char* name;
			int inputs;
		};

		static const Candidate candidates[]
		{
			// Already known to work; included as a control - if these fail the
			// probe itself is wrong.
			{ &CLSID_D2D1GaussianBlur,        "GaussianBlur",        1 },
			{ &CLSID_D2D1ColorMatrix,         "ColorMatrix",         1 },
			{ &CLSID_D2D1Composite,           "Composite",           2 },

			// The interesting ones: a blur that gathers along a direction, and
			// the transforms that magnify the backdrop toward the rim.
			{ &CLSID_D2D1DirectionalBlur,     "DirectionalBlur",     1 },
			{ &CLSID_D2D1Scale,               "Scale",               1 },
			{ &CLSID_D2D12DAffineTransform,   "2DAffineTransform",   1 },
			{ &CLSID_D2D1Border,              "Border",              1 },
			{ &CLSID_D2D1Crop,                "Crop",                1 },
			{ &CLSID_D2D1Tile,                "Tile",                1 },

			// Masking, for confining an effect to the rim band.
			{ &CLSID_D2D1AlphaMask,           "AlphaMask",           2 },
			{ &CLSID_D2D1ArithmeticComposite, "ArithmeticComposite", 2 },
			{ &CLSID_D2D1Blend,               "Blend",               2 },

			// Refraction and shading, all previously assumed one way or another.
			{ &CLSID_D2D1DisplacementMap,     "DisplacementMap",     2 },
			{ &CLSID_D2D1DistantSpecular,     "DistantSpecular",     1 },
			{ &CLSID_D2D1PointSpecular,       "PointSpecular",       1 },
			{ &CLSID_D2D1SpotSpecular,        "SpotSpecular",        1 },
			{ &CLSID_D2D1DistantDiffuse,      "DistantDiffuse",      1 },
			{ &CLSID_D2D1PointDiffuse,        "PointDiffuse",        1 },
			{ &CLSID_D2D1Morphology,          "Morphology",          1 },
			{ &CLSID_D2D1ConvolveMatrix,      "ConvolveMatrix",      1 },
			{ &CLSID_D2D1Emboss,              "Emboss",              1 },
			{ &CLSID_D2D1EdgeDetection,       "EdgeDetection",       1 },
			{ &CLSID_D2D1Shadow,              "Shadow",              1 },
			{ &CLSID_D2D1Sharpen,             "Sharpen",             1 },
		};

		ReconLog::Start(L"C:\\ProgramData\\DWMBlurGlassRecon\\effects.log");
		ReconLog::Write("=== Composition effect support, as seen from inside dwm.exe ===");

		for (const auto& candidate : candidates)
		{
			try
			{
				auto sourceA{ winrt::make_self<ColorSourceEffect>() };
				sourceA->SetName(L"ProbeSourceA");
				auto sourceB{ winrt::make_self<ColorSourceEffect>() };
				sourceB->SetName(L"ProbeSourceB");

				auto probe{ winrt::make_self<ProbeEffect>(*candidate.clsid) };
				probe->SetName(L"Probe");
				probe->SetInput(0, *sourceA);
				if (candidate.inputs > 1)
					probe->SetInput(1, *sourceB);

				compositor.CreateEffectFactory(*probe);
				ReconLog::Write("  %-20s OK", candidate.name);
			}
			catch (const winrt::hresult_error& e)
			{
				const auto message{ winrt::to_string(e.message()) };
				ReconLog::Write("  %-20s REJECTED hr=0x%08X %s",
					candidate.name, static_cast<unsigned>(e.code()), message.c_str());
			}
			catch (...)
			{
				ReconLog::Write("  %-20s REJECTED (non-hresult)", candidate.name);
			}
		}

		ReconLog::Write("=== end ===");
		ReconLog::Flush();
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
		auto it{ g_type == effectType::LiquidGlass ? map.end() : map.find(color) };
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
			// Runs on DWM's render path: nothing may escape. If Composition
			// refuses the graph, degrade to the plain blur rather than taking the
			// compositor down with us - and say why, once, since a silent
			// fallback looks exactly like the material doing nothing.
			if (LiquidGlassBackdrop::IsSupported())
			{
				try
				{
					brush = LiquidGlassBackdrop::CreateBrush(
						compositor,
						winrtColor,
						glassOpacity,
						g_configData.customBlurAmount,
						g_configData.glassIntensity
					);
				}
				catch (const winrt::hresult_error& e)
				{
					static std::atomic_bool reported{ false };
					if (!reported.exchange(true))
					{
						ReconLog::Start(L"C:\\ProgramData\\DWMBlurGlassRecon\\material.log");
						const auto message{ winrt::to_string(e.message()) };
						ReconLog::Write("[material] LiquidGlass brush FAILED hr=0x%08X: %s",
							static_cast<unsigned>(e.code()), message.c_str());
						ReconLog::Flush();
					}
					brush = nullptr;
				}
				catch (...)
				{
					brush = nullptr;
				}
			}
			if (!brush)
			{
				brush = BlurBackdrop::CreateBrush(
					compositor,
					winrtColor,
					glassOpacity,
					g_configData.customBlurAmount
				);
			}
			break;
		}
		default:
			brush = compositor.CreateColorBrush(MakeWinrtColor(policy ? policy->nColor : color));
			break;
		}

		// LiquidGlass is deliberately NOT cached. Its lip gradients are laid out
		// in pixels for one particular window, so a brush shared between windows
		// would draw every other window's lip in the wrong place.
		if (!policy && g_type != effectType::LiquidGlass)
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
	}

	void BackdropFactory::RefreshConfig()
	{
		if (!g_materialTextureBrush)
		{
			g_materialTextureBrush = CreateMaterialTextureBrush();
		}

		if (g_configData.glassProbeEffects)
		{
			try
			{
				com_ptr<DCompPrivate::IDCompositionDesktopDevicePartner> dcompDevice{ nullptr };
				copy_from_abi(dcompDevice, DWM::CDesktopManager::s_pDesktopManagerInstance->GetDCompositionInteropDevice());
				ProbeEffectSupport(dcompDevice.as<wuc::Compositor>());
			}
			catch (...)
			{
			}
		}

		g_type = g_configData.effectType;
		// Liquid glass is a plain Composition effect brush now - a blurred pane
		// with a lit rim - so it works on every build this project supports.
		// Nothing to downgrade.
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
