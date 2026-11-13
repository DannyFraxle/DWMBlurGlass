#pragma once
#include "DWMStruct.h"
#include "../Backdrops/GlassReflectionBackdrop.hpp"

namespace MDWMBlurGlassExt
{
	enum class CompositedBackdropKind : CHAR
	{
		None = -1,
		Legacy,
		Accent,
		SystemBackdrop
	};
	enum class CompositedBackdropType : CHAR
	{
		None = -1,
		Aero,
		Acrylic,
		Mica,
		Blur
	};

	class CGlassReflectionVisual;

	class CCompositedBackdropVisual : public implements<CCompositedBackdropVisual, DCompPrivate::ICompositedBackdropVisual>, CBackdropVisual
	{
		bool m_backdropDataChanged{ false };
		bool m_backdropBrushChanged{ false };
		bool m_activate{ false };
		bool m_visible{ true };
		bool m_clipApplied{ false };
		bool m_clipApplied2{ true };
		DWORD m_state{};
		DWORD m_color{};
		DWORD m_accentFlags{};
		std::chrono::steady_clock::time_point m_backdropTimeStamp{};

		CompositedBackdropKind m_kind{ CompositedBackdropKind::None };
		DWM::CTopLevelWindow* m_window{ nullptr };
		DWM::CTopLevelWindow* m_srcWindow{ nullptr };
		DWM::CWindowData* m_data{ nullptr };

		wuc::CompositionBrush m_backdropBrush{ nullptr };
		wuc::CompositionBrush m_previousBackdropBrush{ nullptr };
		wuc::ContainerVisual m_wucRootVisual{ nullptr };
		wuc::ContainerVisual m_containerVisual{ nullptr };
		wuc::SpriteVisual m_spriteVisual{ nullptr };

		bool m_isWaitingForAnimationComplete{ false };
		bool m_shouldPlayCrossFadeAnimation{ false };
		wuc::CompositionScopedBatch m_waitingForAnimationCompleteBatch{ nullptr };
		winrt::event_token m_waitingForAnimationCompleteToken{};

		com_ptr<CGlassReflectionVisual> m_reflectionVisual{ nullptr };

		wil::unique_hrgn m_gdiWindowRgn{ nullptr };
		wil::unique_hrgn m_clientBlurRgn{ nullptr };
		wil::unique_hrgn m_borderRgn{ nullptr };
		RECT m_captionRect{};
		RECT m_windowRect{};
		std::optional<RECT> m_accentRect{ std::nullopt };
		wil::unique_hrgn m_compositedRgn{ nullptr };
		wuc::CompositionPathGeometry m_pathGeometry{ nullptr };

		// LiquidGlass: what the lip was last laid out for, so it is only redone
		// when the brush, the size or the set of real window edges changes.
		wuc::CompositionBrush m_glassLaidOutBrush{ nullptr };
		wfn::float2 m_glassLaidOutSize{};
		uint8_t m_glassLaidOutEdges{ 0 };

		// Titlebar gradient: a second sprite above the backdrop, covering the
		// caption only. Created the first time it is needed, so it costs
		// nothing while the option is off.
		wuc::SpriteVisual m_gradientVisual{ nullptr };
		wuc::CompositionLinearGradientBrush m_gradientBrush{ nullptr };
		// start, middle, end - the middle stop is only in the brush's stop
		// collection while the middle point is on.
		std::array<wuc::CompositionColorGradientStop, 3> m_gradientStops{ nullptr, nullptr, nullptr };
		std::array<winrt::Windows::UI::Color, 3> m_gradientStopColors{};
		MDWMBlurGlass::gradientDirection m_gradientDirection{ MDWMBlurGlass::gradientDirection::None };
		bool m_gradientMid{ false };
		float m_gradientMidPos{ -1.f };
		RECT m_gradientRect{};

		HRESULT InitializeVisual() override;
		void UninitializeVisual() override;

		void OnDeviceLost();
		void HandleChanges();
		void OnBackdropKindUpdated(CompositedBackdropKind kind);
		void OnBackdropRegionChanged(wil::unique_hrgn& newBackdropRegion);
		void OnBackdropBrushUpdated();
		void OnBackdropBrushChanged();
		void UpdateTitleGradient();
		void CancelAnimationCompleteWait();

		POINT GetClientAreaOffset() const;
		bool DoesBorderParticipateInBackdropRegion() const;
		wil::unique_hrgn CompositeNewBackdropRegion() const;

		DWM::CTopLevelWindow* GetSrcWindow() const
		{
			return m_srcWindow ? m_srcWindow : m_window;
		}

	public:
		CCompositedBackdropVisual(DWM::CTopLevelWindow* window);
		~CCompositedBackdropVisual() override;

		void SetClientBlurRegion(HRGN region) override;
		void SetCaptionRegion(HRGN region) override;
		void SetBorderRegion(HRGN region) override;
		void SetAccentRect(LPCRECT lprc) override;
		void SetGdiWindowRegion(HRGN region) override;
		void ValidateVisual() override;
		void UpdateNCBackground() override;
		auto GetuDwmVisual() const
		{
			return m_udwmVisual.get();
		}
		auto GetDCompVisual() const
		{
			return m_rootVisual;
		}
		bool CanBeTrimmed() override
		{
			if (m_kind != CompositedBackdropKind::Accent && (!m_visible || GetSrcWindow()->IsTrullyMinimized()))
			{
				return true;
			}
			if (m_accentRect.has_value())
			{
				return false;
			}
			if (m_gdiWindowRgn && m_data->GetAccentPolicy()->IsGdiRegionRespected())
			{
				return false;
			}

			return true;
		}

		friend class CClonedCompositedBackdropVisual;
	};

	// temporary workaround for aero peek/live preview
	class CClonedPeekingBackdropVisual : public implements<CCompositedBackdropVisual, DCompPrivate::ICompositedBackdropVisual>, CBackdropVisual
	{
		DWM::CTopLevelWindow* m_window{ nullptr };
		DWM::CWindowData* m_data{ nullptr };
		wuc::ContainerVisual m_containerVisual{ nullptr };
		wuc::CompositionGeometricClip m_roundedClip{ nullptr };
		wuc::CompositionRoundedRectangleGeometry m_roundedGeometry{ nullptr };
		com_ptr<CGlassReflectionVisual> m_reflectionVisual{ nullptr };

		HRESULT InitializeVisual() override;

		void UninitializeVisual() override;

		void OnDeviceLost()
		{
			UninitializeVisual();
			InitializeInteropDevice(DWM::CDesktopManager::s_pDesktopManagerInstance->GetDCompositionInteropDevice());
			InitializeVisual();
		}
	public:
		CClonedPeekingBackdropVisual(DWM::CTopLevelWindow* source, DWM::CTopLevelWindow* target) :
			m_window{ target },
			m_data{ source->GetData() },
			m_reflectionVisual{ winrt::make_self<CGlassReflectionVisual>(source, m_data, true) },
			CBackdropVisual{ target->GetVisual() }
		{
			OnDeviceLost();
		}

		~CClonedPeekingBackdropVisual() override
		{
			CClonedPeekingBackdropVisual::UninitializeVisual();
		}

		void SetClientBlurRegion(HRGN /*region*/) override {}
		void SetCaptionRegion(HRGN /*region*/) override {}
		void SetBorderRegion(HRGN /*region*/) override {}
		void SetAccentRect(LPCRECT /*lprc*/) override {}
		void SetGdiWindowRegion(HRGN /*region*/) override {}
		void ValidateVisual() override {}
		void UpdateNCBackground() override {}
		bool CanBeTrimmed() override
		{
			return false;
		}
	};

	class CClonedCompositedBackdropVisual : public implements<CCompositedBackdropVisual, DCompPrivate::ICompositedBackdropVisual>, CBackdropVisual
	{
		DWM::CTopLevelWindow* m_window{ nullptr };
		com_ptr<CCompositedBackdropVisual> m_source{ nullptr };
		wuc::ContainerVisual m_containerVisual{ nullptr };
		wuc::SpriteVisual m_spriteVisual{ nullptr };
		com_ptr<CGlassReflectionVisual> m_reflectionVisual{ nullptr };
		HRESULT InitializeVisual() override;

		void UninitializeVisual() override;

		void OnDeviceLost()
		{
			UninitializeVisual();
			InitializeInteropDevice(DWM::CDesktopManager::s_pDesktopManagerInstance->GetDCompositionInteropDevice());
			InitializeVisual();
		}
	public:
		CClonedCompositedBackdropVisual(CCompositedBackdropVisual* source, DWM::CTopLevelWindow* target) :
			m_window{ target },
			m_reflectionVisual{ winrt::make_self<CGlassReflectionVisual>(source->m_window, source->m_data, true) },
			CBackdropVisual{ target->GetVisual() }
		{
			copy_from_abi(m_source, source);
			InitializeInteropDevice(DWM::CDesktopManager::s_pDesktopManagerInstance->GetDCompositionInteropDevice());
			CClonedCompositedBackdropVisual::InitializeVisual();
		}

		~CClonedCompositedBackdropVisual() override
		{
			CClonedCompositedBackdropVisual::UninitializeVisual();
		}

		void SetClientBlurRegion(HRGN /*region*/) override {}
		void SetCaptionRegion(HRGN /*region*/) override {}
		void SetBorderRegion(HRGN /*region*/) override {}
		void SetAccentRect(LPCRECT /*lprc*/) override {}
		void SetGdiWindowRegion(HRGN /*region*/) override {}
		void ValidateVisual() override {}
		void UpdateNCBackground() override {}
		bool CanBeTrimmed() override
		{
			return false;
		}
	};
}