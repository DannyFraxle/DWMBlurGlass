#pragma once
#include "DCompBackdrop.hpp"
#include "../Effects/GaussianBlurEffect.hpp"
#include "../Effects/DisplacementMapEffect.hpp"
#include "../Effects/ColorSourceEffect.hpp"
#include "../Effects/OpacityEffect.hpp"
#include "../Effects/CompositeEffect.hpp"

namespace MDWMBlurGlassExt::LiquidGlassBackdrop
{
	// macOS-26 style "liquid glass": the blurred backdrop is refracted along the
	// window edge by a rim normal-map, so straight lines behind the frame bow and
	// magnify like the rim of a curved lens. The interior stays a flat blur.
	//
	// rimBrush is a nine-grid brush wrapping the generated rim normal-map, so the
	// refracting border keeps a constant pixel width regardless of window size
	// (see CreateRimNormalMapBrush in BackdropFactory.cpp).
	wuc::CompositionBrush CreateBrush(
		const wuc::Compositor& compositor,
		const wuc::CompositionBrush& rimBrush,
		const wu::Color& tintColor,
		float tintOpacity,
		float blurAmount,
		float refractionScale
	)
	{
		// Fully opaque tint => nothing to see through, just a solid brush.
		if (static_cast<float>(tintColor.A) * tintOpacity == 255.f)
		{
			return compositor.CreateColorBrush(tintColor);
		}

		// Blur the live backdrop (reuses the shared helper / L"Backdrop" param).
		auto gaussianBlurEffect{ winrt::make_self<GaussianBlurEffect>() };
		gaussianBlurEffect->SetName(L"Blur");
		gaussianBlurEffect->SetBorderMode(D2D1_BORDER_MODE_HARD);
		gaussianBlurEffect->SetBlurAmount(blurAmount ? blurAmount : 1.f);
		gaussianBlurEffect->SetOptimizationMode(D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
		gaussianBlurEffect->SetInput(wuc::CompositionEffectSourceParameter{ L"Backdrop" });

		// Refract the blurred backdrop using the rim normal-map.
		auto displacementEffect{ winrt::make_self<DisplacementMapEffect>() };
		displacementEffect->SetName(L"Refraction");
		displacementEffect->SetScale(refractionScale);
		displacementEffect->SetSource(*gaussianBlurEffect);
		displacementEffect->SetDisplacement(wuc::CompositionEffectSourceParameter{ L"RimMap" });

		winrt::Windows::Graphics::Effects::IGraphicsEffect finalEffect{ *displacementEffect };

		// Optional glass tint composited over the refracted result.
		if (static_cast<float>(tintColor.A) * tintOpacity != 0.f)
		{
			auto tintColorEffect{ winrt::make_self<ColorSourceEffect>() };
			tintColorEffect->SetName(L"TintColor");
			tintColorEffect->SetColor(tintColor);

			auto tintOpacityEffect{ winrt::make_self<OpacityEffect>() };
			tintOpacityEffect->SetName(L"TintOpacity");
			tintOpacityEffect->SetOpacity(tintOpacity);
			tintOpacityEffect->SetInput(*tintColorEffect);

			auto compositeStepEffect{ winrt::make_self<CompositeStepEffect>() };
			compositeStepEffect->SetDestination(*displacementEffect);
			compositeStepEffect->SetSource(*tintOpacityEffect);

			finalEffect = *compositeStepEffect;
		}

		auto effectBrush{ compositor.CreateEffectFactory(finalEffect).CreateBrush() };
		effectBrush.SetSourceParameter(L"Backdrop", compositor.CreateBackdropBrush());
		effectBrush.SetSourceParameter(L"RimMap", rimBrush);
		return effectBrush;
	}
}
