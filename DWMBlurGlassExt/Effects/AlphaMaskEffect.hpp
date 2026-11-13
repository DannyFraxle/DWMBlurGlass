#pragma once
#include "CanvasEffect.hpp"

namespace MDWMBlurGlassExt
{
	// Keeps the magnified copy of the backdrop only where the mask is opaque,
	// which is what confines the gather to a band around the window edge instead
	// of zooming the whole pane.
	//
	// AlphaMask is the one effect that passes Composition's check even with no
	// properties set at all, so it is the safest building block available here.
	class AlphaMaskEffect : public CanvasEffect
	{
	public:
		AlphaMaskEffect() : CanvasEffect{ CLSID_D2D1AlphaMask } {}
		virtual ~AlphaMaskEffect() = default;

		void SetSource(const winrt::Windows::Graphics::Effects::IGraphicsEffectSource& source)
		{
			SetInput(0, source);
		}
		void SetMask(const winrt::Windows::Graphics::Effects::IGraphicsEffectSource& mask)
		{
			SetInput(1, mask);
		}
		void SetSource(const winrt::Windows::UI::Composition::CompositionEffectSourceParameter& source)
		{
			SetInput(0, source);
		}
		void SetMask(const winrt::Windows::UI::Composition::CompositionEffectSourceParameter& mask)
		{
			SetInput(1, mask);
		}

		// No properties at all - say so, rather than inheriting the base class's
		// source-count-as-property-count bug, which would have Composition ask
		// for a property that does not exist.
		IFACEMETHOD(GetPropertyCount)(UINT* count) override
		{
			if (!count)
				return E_POINTER;
			*count = 0;
			return S_OK;
		}
	};
}
