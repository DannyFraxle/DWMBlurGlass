#pragma once
#include "CanvasEffect.hpp"

namespace MDWMBlurGlassExt
{
	// Wraps the Direct2D displacement-map effect (CLSID_D2D1DisplacementMap).
	// Input 0 = image to displace, Input 1 = displacement map (a normal map).
	// Each output pixel is sampled from the source at an offset driven by the
	// selected colour channels of the map, scaled by Scale. This is what bends
	// the background near the window edge like a curved glass rim.
	class DisplacementMapEffect : public CanvasEffect
	{
	public:
		DisplacementMapEffect() : CanvasEffect{ CLSID_D2D1DisplacementMap }
		{
			SetScale();
			SetXChannelSelect();
			SetYChannelSelect();
		}
		virtual ~DisplacementMapEffect() = default;

		void SetScale(float scale = 0.f)
		{
			SetProperty(D2D1_DISPLACEMENTMAP_PROP_SCALE, BoxValue(scale));
		}
		void SetXChannelSelect(D2D1_CHANNEL_SELECTOR selector = D2D1_CHANNEL_SELECTOR_R)
		{
			SetProperty(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT, BoxValue(selector));
		}
		void SetYChannelSelect(D2D1_CHANNEL_SELECTOR selector = D2D1_CHANNEL_SELECTOR_G)
		{
			SetProperty(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT, BoxValue(selector));
		}

		// The image that gets distorted.
		void SetSource(const winrt::Windows::Graphics::Effects::IGraphicsEffectSource& source)
		{
			SetInput(0, source);
		}
		void SetSource(const winrt::Windows::UI::Composition::CompositionEffectSourceParameter& source)
		{
			SetInput(0, source);
		}
		// The normal/displacement map that drives the distortion.
		void SetDisplacement(const winrt::Windows::Graphics::Effects::IGraphicsEffectSource& source)
		{
			SetInput(1, source);
		}
		void SetDisplacement(const winrt::Windows::UI::Composition::CompositionEffectSourceParameter& source)
		{
			SetInput(1, source);
		}
	};
}
