#include "../Backdrops/DCompBackdrop.hpp"
#include "../Backdrops/BackdropFactory.hpp"
#include "CustomBackdropEffect.h"
#include "VersionHelper.h"
#include "CommonDef.h"
#include "../Backdrops/LiquidGlassBackdrop.hpp"
#include "../Helper/CanvasGeometry.hpp"
#include "GlassScene.h"
#include <windowsx.h>

namespace MDWMBlurGlassExt
{
	using namespace CommonDef;

	HRESULT CCompositedBackdropVisual::InitializeVisual()
	{
		RETURN_IF_FAILED(CBackdropVisual::InitializeVisual());

		auto compositor{ m_dcompDevice.as<wuc::Compositor>() };
		m_wucRootVisual = compositor.CreateContainerVisual();
		m_wucRootVisual.BorderMode(wuc::CompositionBorderMode::Soft);
		m_containerVisual = compositor.CreateContainerVisual();
		m_containerVisual.BorderMode(wuc::CompositionBorderMode::Hard);
		m_spriteVisual = compositor.CreateSpriteVisual();
		m_spriteVisual.RelativeSizeAdjustment({ 1.f, 1.f });

		// prepare backdrop brush
		OnBackdropBrushUpdated();

		// if the effect brush is compiled during the maximize/minimize animation
		// then it will NOT show normally, that's because the compilation is asynchronous
		m_reflectionVisual->InitializeVisual(compositor);
		m_wucRootVisual.Children().InsertAtTop(m_containerVisual);
		m_containerVisual.Children().InsertAtBottom(m_spriteVisual);
		m_containerVisual.Children().InsertAtTop(m_reflectionVisual->GetVisual());
		m_pathGeometry = compositor.CreatePathGeometry();

		com_ptr<ID2D1Factory> factory{ nullptr };
		DWM::CDesktopManager::s_pDesktopManagerInstance->GetD2DDevice()->GetFactory(factory.put());
		// since build 22621, Path propertie cannot be empty or the dwmcore will raise a null pointer exception
		m_pathGeometry.Path(wuc::CompositionPath{ Geometry::CanvasGeometry::CreateGeometryFromHRGN(factory.get(), m_compositedRgn.get()).as<wg::IGeometrySource2D>() });
		
		m_rootVisual.Clip(compositor.CreateGeometricClip(m_pathGeometry));
		m_visualCollection.InsertAtBottom(m_wucRootVisual);

		return S_OK;
	}

	void CCompositedBackdropVisual::UninitializeVisual()
	{
		CancelAnimationCompleteWait();

		m_pathGeometry = nullptr;
		m_spriteVisual = nullptr;
		m_wucRootVisual = nullptr;
		m_containerVisual = nullptr;
		m_backdropBrush = nullptr;
		m_previousBackdropBrush = nullptr;
		m_glassLaidOutBrush = nullptr;
		GlassScene::RemoveGlass(this);

		m_gradientVisual = nullptr;
		m_gradientBrush = nullptr;
		m_gradientStops = { nullptr, nullptr, nullptr };
		m_gradientStopColors = {};
		m_gradientDirection = gradientDirection::None;
		m_gradientMid = false;
		m_gradientMidPos = -1.f;
		m_gradientRect = {};

		m_reflectionVisual->UninitializeVisual();
		CBackdropVisual::UninitializeVisual();
	}

	void CCompositedBackdropVisual::OnDeviceLost()
	{
		UninitializeVisual();
		InitializeInteropDevice(DWM::CDesktopManager::s_pDesktopManagerInstance->GetDCompositionInteropDevice());
		InitializeVisual();
		m_backdropDataChanged = true;
	}

	void CCompositedBackdropVisual::HandleChanges() try
	{
		wil::unique_hrgn compositedRgn{ CompositeNewBackdropRegion() };
		if (!EqualRgn(m_compositedRgn.get(), compositedRgn.get()))
		{
			OnBackdropRegionChanged(compositedRgn);
		}
		m_backdropDataChanged = false;
	}
	CATCH_LOG_RETURN()

	void CCompositedBackdropVisual::OnBackdropKindUpdated(CompositedBackdropKind kind)
	{
		if (m_kind != kind)
		{
			m_kind = kind;
			m_backdropDataChanged = true;
			m_previousBackdropBrush = nullptr;
			m_backdropBrush = nullptr;
			m_backdropBrushChanged = true;
		}
	}

	void CCompositedBackdropVisual::OnBackdropRegionChanged(wil::unique_hrgn& newBackdropRegion)
	{
		m_compositedRgn = std::move(newBackdropRegion);

		bool isVisible{};
		RECT regionBox{};
		auto regionType{ GetRgnBox(m_compositedRgn.get(), &regionBox) };
		if (
			regionType == NULLREGION ||
			IsRectEmpty(&regionBox)
			)
		{
			isVisible = false;
		}
		else
		{
			isVisible = true;
		}

		if (m_visible != isVisible)
		{
			m_rootVisual.IsVisible(isVisible);
			m_visible = isVisible;
		}

		if (!m_visible)
		{
			return;
		}

		bool clipApplied{ false };
		if (regionType == SIMPLEREGION)
		{
			clipApplied = false;
		}
		if (regionType == COMPLEXREGION)
		{
			clipApplied = true;
		}
		// If clipRound is configured, always apply clipping
		if (g_configData.clipRound > 0)
		{
			clipApplied = true;
		}
		if (m_clipApplied2 != clipApplied)
		{
			m_rootVisual.Clip(clipApplied ? m_dcompDevice.as<wuc::Compositor>().CreateGeometricClip(m_pathGeometry) : nullptr);
			m_clipApplied2 = clipApplied;
		}

		wfn::float3 offset{ static_cast<float>(regionBox.left), static_cast<float>(regionBox.top), 0.f };
		m_containerVisual.Offset(offset);
		m_containerVisual.Size({ static_cast<float>(max(wil::rect_width(regionBox), 0)), static_cast<float>(max(wil::rect_height(regionBox), 0)) });
		m_reflectionVisual->NotifyOffsetToWindow(offset);

		winrt::com_ptr<ID2D1Factory> factory{ nullptr };
		DWM::CDesktopManager::s_pDesktopManagerInstance->GetD2DDevice()->GetFactory(factory.put());
		auto canvasGeometry
		{
			Geometry::CanvasGeometry::CreateGeometryFromHRGN(
				factory.get(),
				m_compositedRgn.get()
			)
		};

		// Apply rounded rectangle clipping
		if (g_configData.clipRound > 0)
		{
			winrt::com_ptr<ID2D1RoundedRectangleGeometry> roundedRectGeometry{ nullptr };
			D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(
				D2D1::RectF(0.f, 0.f, static_cast<float>(wil::rect_width(regionBox)), static_cast<float>(wil::rect_height(regionBox))),
				static_cast<float>(g_configData.clipRound),
				static_cast<float>(g_configData.clipRound)
			);
			THROW_IF_FAILED(factory->CreateRoundedRectangleGeometry(roundedRect, roundedRectGeometry.put()));

			winrt::com_ptr<ID2D1PathGeometry> clippedGeometry{ nullptr };
			THROW_IF_FAILED(factory->CreatePathGeometry(clippedGeometry.put()));
			winrt::com_ptr<ID2D1GeometrySink> sink{ nullptr };
			THROW_IF_FAILED(clippedGeometry->Open(sink.put()));

			winrt::com_ptr<ID2D1Geometry> baseGeometry{ nullptr };
			canvasGeometry->GetGeometry(baseGeometry.put());
			THROW_IF_FAILED(
				baseGeometry->CombineWithGeometry(
					roundedRectGeometry.get(),
					D2D1_COMBINE_MODE_INTERSECT,
					nullptr,
					sink.get()
				)
			);
			THROW_IF_FAILED(sink->Close());

			auto clippedCanvasGeometry{ winrt::make_self<Geometry::CanvasGeometry>() };
			clippedCanvasGeometry->SetGeometry(clippedGeometry.get());
			m_pathGeometry.Path(wuc::CompositionPath{ clippedCanvasGeometry.as<wg::IGeometrySource2D>() });
		}
		else
		{
			m_pathGeometry.Path(wuc::CompositionPath{ canvasGeometry.as<wg::IGeometrySource2D>() });
		}
	}

	void CCompositedBackdropVisual::OnBackdropBrushUpdated()
	{
		m_backdropBrushChanged = false;
		m_shouldPlayCrossFadeAnimation = false;
		if (m_kind == CompositedBackdropKind::Accent)
		{
			auto policy{ m_data->GetAccentPolicy() };
			if (
				policy->nAccentState != m_state ||
				policy->nColor != m_color ||
				policy->nFlags != m_accentFlags
				)
			{
				m_state = policy->nAccentState;
				m_color = policy->nColor;
				m_accentFlags = policy->nFlags;
				m_backdropBrushChanged = true;
			}

			if (m_activate != true)
			{
				m_activate = true;
				m_backdropBrushChanged = true;
			}
		}
		else
		{
			auto active{ GetSrcWindow()->TreatAsActiveWindow()};
			if (active != m_activate)
			{
				m_shouldPlayCrossFadeAnimation = true;
				m_activate = active;
				m_backdropBrushChanged = true;
			}

			DWORD color;
			if (g_configData.useAccentColor)
			{
				BYTE alpha = BYTE(ULONG((active ? g_configData.activeBlendColor : g_configData.inactiveBlendColor) >> 24) & 0xff);
				color = RGB(GetRValue(g_accentColor), GetGValue(g_accentColor), GetBValue(g_accentColor));
				color = color | (alpha << 24);
			}
			else
			{
				if(m_data->IsUsingDarkMode())
					color = active ? g_configData.activeBlendColorDark : g_configData.inactiveBlendColorDark;
				else
					color = active ? g_configData.activeBlendColor : g_configData.inactiveBlendColor;
			}
			if (color != m_color)
			{
				m_shouldPlayCrossFadeAnimation = true;
				m_color = color;
				m_backdropBrushChanged = true;
			}
		}

		auto timeStamp{ BackdropFactory::GetBackdropBrushTimeStamp() };
		if (m_backdropTimeStamp != timeStamp)
		{
			m_backdropTimeStamp = timeStamp;
			m_backdropBrushChanged = true;
		}

		if (m_backdropBrushChanged || !m_backdropBrush)
		{
			m_backdropBrush = BackdropFactory::GetOrCreateBackdropBrush(
				m_dcompDevice.as<wuc::Compositor>(),
				m_color,
				m_activate,
				m_kind == CompositedBackdropKind::Accent ? m_data->GetAccentPolicy() : nullptr
			);
			OnBackdropBrushChanged();
		}
	}

	void CCompositedBackdropVisual::OnBackdropBrushChanged()
	{
		if (m_previousBackdropBrush == m_backdropBrush)
			return;

		if (!m_previousBackdropBrush)
		{
			m_shouldPlayCrossFadeAnimation = false;
		}
		CancelAnimationCompleteWait();

		if (m_shouldPlayCrossFadeAnimation && g_configData.crossFade)
		{
			auto compositor{ m_backdropBrush.Compositor() };
			auto crossfadeBrush{ CreateCrossFadeBrush(compositor, m_previousBackdropBrush, m_backdropBrush) };

			winrt::Windows::Foundation::TimeSpan crossfadeTime{ std::chrono::milliseconds{ g_configData.crossfadeTime } };
			// the completed handler won't be called under windows 10 2004, idk why...
			if (os::buildNumber >= 19041)
			{
				m_isWaitingForAnimationComplete = true;
				ThisModule_AddRef();

				m_waitingForAnimationCompleteBatch = compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
				{
					auto strongThis{ get_strong() };
					auto handler = [strongThis](auto sender, auto args)
						{
							strongThis->CancelAnimationCompleteWait();
							strongThis->m_spriteVisual.Brush(strongThis->m_backdropBrush);
						};

					crossfadeBrush.StartAnimation(L"Crossfade.Weight", CreateCrossFadeAnimation(compositor,
						1, crossfadeTime));

					m_waitingForAnimationCompleteToken = m_waitingForAnimationCompleteBatch.Completed(handler);
					m_waitingForAnimationCompleteBatch.End();
				}
			}
			else
			{
				crossfadeBrush.StartAnimation(L"Crossfade.Weight", CreateCrossFadeAnimation(compositor,
					1, crossfadeTime));
			}

			m_spriteVisual.Brush(crossfadeBrush);
		}
		else
		{
			m_spriteVisual.Brush(m_backdropBrush);
		}
		m_previousBackdropBrush = m_backdropBrush;
	}

	// Lays a linear gradient over the caption, above the backdrop and below the
	// reflection, title text and buttons. It runs from fully clear at the start
	// edge to the configured colour at the far edge, so the start shows the
	// normal (blend-coloured) titlebar and the gradient fades to the new colour
	// across it - an opaque blend colour plus an opaque gradient colour gives
	// the solid Windows 98 look, translucent ones keep the blur showing.
	// An optional middle point gives three: blend colour -> middle -> end,
	// which is what the glossy XP titlebar needs.
	//
	// Runs on DWM's render path: nothing may escape.
	void CCompositedBackdropVisual::UpdateTitleGradient() try
	{
		const auto direction
		{
			m_kind == CompositedBackdropKind::Accent ?
			gradientDirection::None :
			g_configData.titleGradient
		};

		// The caption, in the container's coordinates. The region and the
		// caption rect share the window-relative space the container is
		// offset into; clamp to the region so the sprite never pokes out of
		// an unclipped (rectangular) backdrop.
		RECT box{};
		if (direction != gradientDirection::None)
		{
			RECT regionBox{};
			RECT caption{};
			if (GetRgnBox(m_compositedRgn.get(), &regionBox) != NULLREGION &&
				IntersectRect(&caption, &m_captionRect, &regionBox))
			{
				box =
				{
					caption.left - regionBox.left,
					caption.top - regionBox.top,
					caption.right - regionBox.left,
					caption.bottom - regionBox.top
				};
			}
		}

		if (direction == gradientDirection::None || IsRectEmpty(&box))
		{
			if (m_gradientVisual)
			{
				m_gradientVisual.IsVisible(false);
			}
			return;
		}

		const bool dark{ m_data->IsUsingDarkMode() };
		const COLORREF endRef
		{
			dark ?
			(m_activate ? g_configData.activeGradientColorDark : g_configData.inactiveGradientColorDark) :
			(m_activate ? g_configData.activeGradientColor : g_configData.inactiveGradientColor)
		};
		const COLORREF midRef
		{
			dark ?
			(m_activate ? g_configData.activeGradientMidColorDark : g_configData.inactiveGradientMidColorDark) :
			(m_activate ? g_configData.activeGradientMidColor : g_configData.inactiveGradientMidColor)
		};
		const bool mid{ g_configData.titleGradientMid };
		const float midPos{ static_cast<float>(std::clamp(g_configData.titleGradientMidPos, 1, 99)) / 100.f };

		// Gradient stops are straight colours, so the clear start keeps the
		// RGB of the stop after it - interpolating towards transparent black
		// would darken the bar.
		const auto endColor{ MakeWinrtColor(endRef) };
		const auto midColor{ MakeWinrtColor(midRef) };
		const auto& nextColor{ mid ? midColor : endColor };
		const std::array<wu::Color, 3> targets
		{
			wu::Color{ 0, nextColor.R, nextColor.G, nextColor.B },
			midColor,
			endColor
		};

		auto compositor{ m_dcompDevice.as<wuc::Compositor>() };
		if (!m_gradientVisual)
		{
			m_gradientStops[0] = compositor.CreateColorGradientStop(0.f, targets[0]);
			m_gradientStops[1] = compositor.CreateColorGradientStop(midPos, targets[1]);
			m_gradientStops[2] = compositor.CreateColorGradientStop(1.f, targets[2]);
			m_gradientStopColors = targets;
			m_gradientBrush = compositor.CreateLinearGradientBrush();

			m_gradientVisual = compositor.CreateSpriteVisual();
			m_gradientVisual.Brush(m_gradientBrush);
			m_containerVisual.Children().InsertAbove(m_gradientVisual, m_spriteVisual);

			m_gradientDirection = gradientDirection::None;
			m_gradientMid = !mid;	// forces the stop collection to be filled below
			m_gradientMidPos = midPos;
			m_gradientRect = {};
		}
		m_gradientVisual.IsVisible(true);

		if (mid != m_gradientMid)
		{
			auto stops{ m_gradientBrush.ColorStops() };
			stops.Clear();
			stops.Append(m_gradientStops[0]);
			if (mid)
				stops.Append(m_gradientStops[1]);
			stops.Append(m_gradientStops[2]);
			m_gradientMid = mid;
		}

		if (midPos != m_gradientMidPos)
		{
			m_gradientStops[1].Offset(midPos);
			m_gradientMidPos = midPos;
		}

		if (!EqualRect(&box, &m_gradientRect))
		{
			m_gradientVisual.Offset({ static_cast<float>(box.left), static_cast<float>(box.top), 0.f });
			m_gradientVisual.Size({ static_cast<float>(wil::rect_width(box)), static_cast<float>(wil::rect_height(box)) });
			m_gradientRect = box;
		}

		if (direction != m_gradientDirection)
		{
			// Relative mapping: 0..1 spans the sprite, which is exactly the
			// caption, so the same brush fits any window size.
			m_gradientBrush.StartPoint({ 0.f, 0.f });
			m_gradientBrush.EndPoint(direction == gradientDirection::Horizontal ? wfn::float2{ 1.f, 0.f } : wfn::float2{ 0.f, 1.f });
			m_gradientDirection = direction;
		}

		// Follow the backdrop's own crossfade when the window is (de)activated,
		// so the bar does not snap ahead of the blur.
		const bool animate{ m_shouldPlayCrossFadeAnimation && g_configData.crossFade && g_configData.crossfadeTime > 0 };
		for (size_t i = 0; i < targets.size(); ++i)
		{
			const auto& from{ m_gradientStopColors[i] };
			const auto& to{ targets[i] };
			if (from == to)
				continue;

			const auto& stop{ m_gradientStops[i] };
			bool animated{ false };
			if (animate)
			{
				try
				{
					auto animation{ compositor.CreateColorKeyFrameAnimation() };
					animation.InsertKeyFrame(0.f, from);
					animation.InsertKeyFrame(1.f, to);
					animation.Duration(std::chrono::milliseconds{ g_configData.crossfadeTime });
					stop.StartAnimation(L"Color", animation);
					animated = true;
				}
				catch (...)
				{
				}
			}
			if (!animated)
			{
				stop.StopAnimation(L"Color");
				stop.Color(to);
			}
			m_gradientStopColors[i] = to;
		}
	}
	CATCH_LOG_RETURN()

	void CCompositedBackdropVisual::CancelAnimationCompleteWait()
	{
		if (m_waitingForAnimationCompleteBatch)
		{
			m_waitingForAnimationCompleteBatch.Completed(m_waitingForAnimationCompleteToken);
			m_waitingForAnimationCompleteBatch = nullptr;
			m_waitingForAnimationCompleteToken = {};
		}
		if (m_isWaitingForAnimationComplete)
		{
			m_isWaitingForAnimationComplete = false;
			ThisModule_Release();
		}
	}

	POINT CCompositedBackdropVisual::GetClientAreaOffset() const
	{
		// GetClientBlurVisual() somtimes cannot work in ClonedVisual
		// so here we use the original offset get from CTopLevelWindow::UpdateClientBlur
		auto margins{ GetSrcWindow()->GetClientAreaContainerParentVisual()->GetMargins()};
		return { margins->cxLeftWidth, margins->cyTopHeight };
	}

	bool CCompositedBackdropVisual::DoesBorderParticipateInBackdropRegion() const
	{
		return g_configData.extendBorder || m_data->IsFrameExtendedIntoClientAreaLRB();
	}

	wil::unique_hrgn CCompositedBackdropVisual::CompositeNewBackdropRegion() const
	{
		wil::unique_hrgn compositedRgn{ CreateRectRgn(0, 0, 0, 0) };
		wil::unique_hrgn nonClientRgn{ CreateRectRgn(0, 0, 0, 0) };
		wil::unique_hrgn realClientBlurRgn{ CreateRectRgn(0, 0, 0, 0) };
		wil::unique_hrgn captionRgn{ CreateRectRgnIndirect(&m_captionRect) };
		wil::unique_hrgn windowRgn{ CreateRectRgnIndirect(&m_windowRect) };

		auto includedBorder{ DoesBorderParticipateInBackdropRegion() };
		CombineRgn(nonClientRgn.get(), captionRgn.get(), includedBorder ? m_borderRgn.get() : nullptr, includedBorder ? RGN_OR : RGN_COPY);
		// DwmEnableBlurBehind
		if (m_clientBlurRgn)
		{
			CopyRgn(realClientBlurRgn.get(), m_clientBlurRgn.get());
			auto clientOffset{ GetClientAreaOffset() };
			OffsetRgn(realClientBlurRgn.get(), clientOffset.x, clientOffset.y);
			CombineRgn(compositedRgn.get(), compositedRgn.get(), realClientBlurRgn.get(), RGN_OR);
		}
		if (m_kind != CompositedBackdropKind::Accent)
		{
			CombineRgn(compositedRgn.get(), compositedRgn.get(), nonClientRgn.get(), RGN_OR);
			if (m_kind == CompositedBackdropKind::SystemBackdrop)
			{
				CombineRgn(compositedRgn.get(), compositedRgn.get(), windowRgn.get(), RGN_OR);
			}
		}
		else
		{
			CombineRgn(compositedRgn.get(), compositedRgn.get(), windowRgn.get(), RGN_OR);
			if (m_accentRect)
			{
				wil::unique_hrgn accentRgn{ CreateRectRgnIndirect(&m_accentRect.value()) };
				CombineRgn(compositedRgn.get(), compositedRgn.get(), accentRgn.get(), RGN_AND);
			}

			if (
				m_gdiWindowRgn &&
				m_data->GetAccentPolicy()->IsGdiRegionRespected()
				)
			{
				CombineRgn(compositedRgn.get(), compositedRgn.get(), m_gdiWindowRgn.get(), RGN_AND);
			}
		}
		CombineRgn(compositedRgn.get(), compositedRgn.get(), windowRgn.get(), RGN_AND);

		return compositedRgn;
	}

	CCompositedBackdropVisual::CCompositedBackdropVisual(DWM::CTopLevelWindow* window) :
		m_window{ window },
		m_data{ window->GetData() },
		m_reflectionVisual{ winrt::make_self<CGlassReflectionVisual>(m_window, m_data) },
		CBackdropVisual{ window->GetVisual() }
	{
		m_borderRgn.reset(CreateRectRgn(0, 0, 0, 0));
		m_compositedRgn.reset(CreateRectRgn(0, 0, 0, 0));

		OnDeviceLost();
		OnBackdropKindUpdated(CustomBackdrop::GetActualBackdropKind(window));
		if (m_kind == CompositedBackdropKind::Accent)
		{
			wil::unique_hrgn clipRgn{ CreateRectRgn(0, 0, 0, 0) };
			if (GetWindowRgn(m_data->GetHWND(), clipRgn.get()) != ERROR)
			{
				CCompositedBackdropVisual::SetGdiWindowRegion(clipRgn.get());
			}
			else
			{
				CCompositedBackdropVisual::SetGdiWindowRegion(nullptr);
			}
		}
	}

	CCompositedBackdropVisual::~CCompositedBackdropVisual()
	{
		CCompositedBackdropVisual::UninitializeVisual();
	}

	void CCompositedBackdropVisual::SetClientBlurRegion(HRGN region)
	{
		if (!m_clientBlurRgn)
		{
			m_clientBlurRgn.reset(CreateRectRgn(0, 0, 0, 0));
		}
		CopyRgn(m_clientBlurRgn.get(), region);
		m_backdropDataChanged = true;
	}

	void CCompositedBackdropVisual::SetCaptionRegion(HRGN region)
	{
		GetRgnBox(region, &m_captionRect);
		m_backdropDataChanged = true;
	}

	void CCompositedBackdropVisual::SetBorderRegion(HRGN region)
	{
		CopyRgn(m_borderRgn.get(), region);
		m_backdropDataChanged = true;
	}

	void CCompositedBackdropVisual::SetAccentRect(LPCRECT lprc)
	{
		if (lprc)
		{
			m_accentRect = *lprc;
			m_gdiWindowRgn.reset();
		}
		else if (m_accentRect.has_value())
		{
			m_accentRect = std::nullopt;
			if (m_kind == CompositedBackdropKind::Accent)
			{
				wil::unique_hrgn clipRgn{ CreateRectRgn(0, 0, 0, 0) };
				if (GetWindowRgn(m_data->GetHWND(), clipRgn.get()) != ERROR)
				{
					m_gdiWindowRgn.reset(clipRgn.release());
				}
				else
				{
					m_gdiWindowRgn.reset();
				}
			}
		}
		m_backdropDataChanged = true;
	}

	void CCompositedBackdropVisual::SetGdiWindowRegion(HRGN region)
	{
		if (region)
		{
			if (!m_gdiWindowRgn)
			{
				m_gdiWindowRgn.reset(CreateRectRgn(0, 0, 0, 0));
			}
			CopyRgn(m_gdiWindowRgn.get(), region);
			m_accentRect = std::nullopt;
		}
		else if (m_gdiWindowRgn)
		{
			m_gdiWindowRgn.reset();
			m_accentRect = std::nullopt;
		}
		m_backdropDataChanged = true;
	}

	void CCompositedBackdropVisual::ValidateVisual()
	{
		if (GetSrcWindow()->IsTrullyMinimized())
		{
			GlassScene::RemoveGlass(this);
			return;
		}

		if (m_visible)
		{
			if (!CheckDeviceState(m_dcompDevice))
			{
				OnDeviceLost();
			}
		}

		OnBackdropKindUpdated(CustomBackdrop::GetActualBackdropKind(GetSrcWindow()));
		if (m_backdropDataChanged) { HandleChanges(); }
		if (m_visible)
		{
			OnBackdropBrushUpdated();
			m_reflectionVisual->ValidateVisual();
			UpdateTitleGradient();

			// Lay this window's glass lip out in pixels. The brush cannot do it
			// itself: its gradients are relative to the visual, and the lip has to
			// stay the same width however big the window is.
			//
			// Only edges of the backdrop that are real window edges get a lip - a
			// caption-only backdrop meets the client area along its bottom, and a
			// lit edge there would read as a seam. The region and m_windowRect
			// share a coordinate space; the slack absorbs resize borders.
			if (g_configData.effectType == effectType::LiquidGlass && m_backdropBrush && m_containerVisual)
			{
				const auto size{ m_containerVisual.Size() };
				RECT regionBox{};
				GetRgnBox(m_compositedRgn.get(), &regionBox);
				constexpr LONG slack{ 16 };
				uint8_t edges{ 0 };
				if (regionBox.left - m_windowRect.left <= slack) edges |= LiquidGlassBackdrop::LeftEdge;
				if (regionBox.top - m_windowRect.top <= slack) edges |= LiquidGlassBackdrop::TopEdge;
				if (m_windowRect.right - regionBox.right <= slack) edges |= LiquidGlassBackdrop::RightEdge;
				if (m_windowRect.bottom - regionBox.bottom <= slack) edges |= LiquidGlassBackdrop::BottomEdge;

				// While the edge pinch is running it draws the edge - compacted
				// surroundings and its own specular line - so the Composition lip
				// steps aside entirely rather than doubling it up.
				const uint8_t lipEdges{ GlassScene::IsEnabled() ? uint8_t{ 0 } : edges };
				if (m_glassLaidOutBrush != m_backdropBrush || m_glassLaidOutSize != size || m_glassLaidOutEdges != lipEdges)
				{
					LiquidGlassBackdrop::UpdateLayout(
						m_backdropBrush,
						size.x,
						size.y,
						lipEdges,
						g_configData.glassRimThickness,
						g_configData.glassIntensity);
					m_glassLaidOutBrush = m_backdropBrush;
					m_glassLaidOutSize = size;
					m_glassLaidOutEdges = lipEdges;
				}

				// And tell the edge pinch (Windows 11, drawn on the finished
				// frame) where the TITLEBAR is on screen - only the titlebar gets
				// it. The backdrop region is no use for this: it includes the thin
				// side and bottom borders, so its bounding box is the whole window,
				// and pinching that cuts into the app's own content all round.
				//
				// The caption rect lives in the same window-relative space as
				// m_windowRect; the same rect asked for without erasing the offset
				// is in screen pixels, so the difference places it on screen. Its
				// top, left and right get the pinch where they are real window
				// edges; never its bottom, which is where it meets the client area.
				//
				// Apps that draw their own titlebar leave DWM a caption only a
				// pixel or so tall; that is not a titlebar, and a band there would
				// just be a stray line along the top of the window.
				constexpr LONG minCaptionHeight{ 8 };
				RECT caption{};
				RECT screenRect{};
				wuc::CompositionEffectBrush material{ nullptr };
				if (GlassScene::IsEnabled())
					material = LiquidGlassBackdrop::GetMaterial(m_backdropBrush);
				if (material &&
					IntersectRect(&caption, &m_captionRect, &regionBox) &&
					caption.bottom - caption.top >= minCaptionHeight &&
					GetSrcWindow()->GetActualWindowRect(&screenRect, false, true, false))
				{
					uint8_t captionEdges{ 0 };
					if (caption.left - m_windowRect.left <= slack) captionEdges |= LiquidGlassBackdrop::LeftEdge;
					if (caption.top - m_windowRect.top <= slack) captionEdges |= LiquidGlassBackdrop::TopEdge;
					if (m_windowRect.right - caption.right <= slack) captionEdges |= LiquidGlassBackdrop::RightEdge;

					const LONG dx{ screenRect.left - m_windowRect.left };
					const LONG dy{ screenRect.top - m_windowRect.top };
					const RECT glass{ caption.left + dx, caption.top + dy, caption.right + dx, caption.bottom + dy };
					// The band is composited inside the material, and the sprite
					// it paints starts at the region box's corner.
					const POINT bandOffset{ caption.left - regionBox.left, caption.top - regionBox.top };
					if (captionEdges)
					{
						GlassScene::UpdateGlass(this, m_data->GetHWND(), glass, captionEdges, m_activate,
							m_spriteVisual, material, bandOffset);
						return;
					}
				}

				// Elements that are glass all over - the taskbar, Start, flyouts,
				// anything that asked DWM for an accent blur - have no titlebar and
				// no client area for a band to run into, so the reflection goes
				// along every side of them. (Sides at the screen border, like the
				// taskbar's bottom, are dropped when the scene is published.)
				// Ordinary windows still only ever get it on their titlebar.
				constexpr LONG minElementSide{ 8 };
				if (material &&
					m_kind == CompositedBackdropKind::Accent &&
					regionBox.right - regionBox.left >= minElementSide &&
					regionBox.bottom - regionBox.top >= minElementSide &&
					GetSrcWindow()->GetActualWindowRect(&screenRect, false, true, false))
				{
					const LONG dx{ screenRect.left - m_windowRect.left };
					const LONG dy{ screenRect.top - m_windowRect.top };
					const RECT glass{ regionBox.left + dx, regionBox.top + dy, regionBox.right + dx, regionBox.bottom + dy };
					GlassScene::UpdateGlass(this, m_data->GetHWND(), glass, LiquidGlassBackdrop::AllEdges, m_activate,
						m_spriteVisual, material, POINT{ 0, 0 });
					return;
				}
			}
		}
		// Not visible, or not liquid glass: no pinch for this window.
		GlassScene::RemoveGlass(this);
	}

	void CCompositedBackdropVisual::UpdateNCBackground()
	{
		if (GetSrcWindow()->IsTrullyMinimized())
		{
			return;
		}

		RECT borderRect{};
		THROW_HR_IF_NULL(E_INVALIDARG, GetSrcWindow()->GetActualWindowRect(&borderRect, true, true, false));

		if (!EqualRect(&m_windowRect, &borderRect))
		{
			m_windowRect = borderRect;
			m_backdropDataChanged = true;
		}
	}

	HRESULT CClonedPeekingBackdropVisual::InitializeVisual()
	{
		RETURN_IF_FAILED(CBackdropVisual::InitializeVisual());

		auto compositor{ m_dcompDevice.as<wuc::Compositor>() };
		m_reflectionVisual->InitializeVisual(compositor);
		m_containerVisual = compositor.CreateContainerVisual();
		m_roundedGeometry = compositor.CreateRoundedRectangleGeometry();
		m_roundedClip = compositor.CreateGeometricClip(m_roundedGeometry);
		{
			HWND hwnd{ m_data->GetHWND() };
			RECT windowRect{}, borderRect{};
			auto window{ m_data->GetWindow() };
			window->GetActualWindowRect(&windowRect, false, true, true);
			window->GetActualWindowRect(&borderRect, false, true, false);
			MARGINS margins{};
			//window->GetBorderMargins(&margins);
			winrt::Windows::Foundation::Numerics::float3 offset
			{
				static_cast<float>(0.f),
				static_cast<float>(!IsMaximized(hwnd) ? 0.f : margins.cyTopHeight),
				1.f
			};
			winrt::Windows::Foundation::Numerics::float2 size
			{
				static_cast<float>(wil::rect_width(borderRect) + (IsMaximized(hwnd) ? margins.cxRightWidth + margins.cxLeftWidth : 0)),
				static_cast<float>(wil::rect_height(borderRect))
			};
			m_containerVisual.Offset(offset);
			m_containerVisual.Size(size);
			m_roundedGeometry.Size(size);
			m_roundedGeometry.CornerRadius(wfn::float2{ (float)g_configData.extendRound, (float)g_configData.extendRound });
			m_reflectionVisual->NotifyOffsetToWindow(offset);
		}
		m_containerVisual.Clip(m_roundedClip);
		m_containerVisual.Children().InsertAtTop(m_reflectionVisual->GetVisual());
		m_reflectionVisual->ValidateVisual();
		m_visualCollection.InsertAtBottom(m_containerVisual);

		return S_OK;
	}

	void CClonedPeekingBackdropVisual::UninitializeVisual()
	{
		m_roundedClip = nullptr;
		m_roundedGeometry = nullptr;
		m_containerVisual = nullptr;

		m_reflectionVisual->UninitializeVisual();
		CBackdropVisual::UninitializeVisual();
	}

	HRESULT CClonedCompositedBackdropVisual::InitializeVisual()
	{
		RETURN_IF_FAILED(CBackdropVisual::InitializeVisual());

		auto compositor{ m_dcompDevice.as<wuc::Compositor>() };
		m_reflectionVisual->InitializeVisual(compositor);
		m_containerVisual = compositor.CreateContainerVisual();
		m_spriteVisual = compositor.CreateSpriteVisual();
		m_spriteVisual.RelativeSizeAdjustment({ 1.f, 1.f });

		m_reflectionVisual->NotifyOffsetToWindow(m_source->m_reflectionVisual->GetOffset());
		m_reflectionVisual->ValidateVisual();

		RECT regionBox{};
		GetRgnBox(m_source->m_compositedRgn.get(), &regionBox);

		wfn::float3 offset{ static_cast<float>(regionBox.left), static_cast<float>(regionBox.top), 0.f };
		m_containerVisual.Offset(offset);
		m_containerVisual.Size({ static_cast<float>(max(wil::rect_width(regionBox), 0)), static_cast<float>(max(wil::rect_height(regionBox), 0)) });

		m_containerVisual.Clip(compositor.CreateGeometricClip(m_source->m_pathGeometry));
		m_containerVisual.Children().InsertAtTop(m_reflectionVisual->GetVisual());

		m_spriteVisual.Brush(m_source->m_spriteVisual.Brush());
		m_containerVisual.Children().InsertAtBottom(m_spriteVisual);

		// Carry the titlebar gradient across too; the brush can be shared.
		if (m_source->m_gradientVisual && m_source->m_gradientVisual.IsVisible())
		{
			auto gradientVisual{ compositor.CreateSpriteVisual() };
			gradientVisual.Brush(m_source->m_gradientBrush);
			gradientVisual.Offset(m_source->m_gradientVisual.Offset());
			gradientVisual.Size(m_source->m_gradientVisual.Size());
			m_containerVisual.Children().InsertAbove(gradientVisual, m_spriteVisual);
		}

		m_visualCollection.InsertAtBottom(m_containerVisual);

		return S_OK;
	}

	void CClonedCompositedBackdropVisual::UninitializeVisual()
	{
		m_spriteVisual = nullptr;
		m_containerVisual = nullptr;
		m_source = nullptr;
		m_reflectionVisual->UninitializeVisual();
		CBackdropVisual::UninitializeVisual();
	}
}
