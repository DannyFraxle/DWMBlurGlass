#pragma once
#include "DCompBackdrop.hpp"
#include "../Effects/GaussianBlurEffect.hpp"
#include "../Effects/SaturationEffect.hpp"
#include "../Effects/ColorSourceEffect.hpp"
#include "../Effects/OpacityEffect.hpp"
#include "../Effects/CompositeEffect.hpp"
#include "../Effects/BlendEffect.hpp"
#include "../Effects/ChannelMatrixEffect.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace MDWMBlurGlassExt::LiquidGlassBackdrop
{
	// A pane of thick glass: the backdrop blurred and made a little more vivid,
	// tinted, and finished with a lit lip around the window edge - a crisp
	// specular line where the edge catches the light, a soft frosted glow
	// across the thickness of the edge, and a faint shadow just inside the
	// highlight that makes the edge read as raised rather than painted on.
	//
	// What this material deliberately does NOT do is bend the backdrop. That
	// was tried, and Windows.UI.Composition rules it out at every turn (all
	// verified against a standalone compositor, not guessed):
	//  - DisplacementMap, Scale, DirectionalBlur and friends are refused
	//    outright ("Unsupported effect type");
	//  - 2DAffineTransform is accepted, but only on a SURFACE brush: on a
	//    backdrop brush it fails with "Backdrop brushes cannot be transformed",
	//    and on a nested effect brush "Effect brushes cannot be transformed";
	//  - a backdrop brush always samples in screen space, so neither scaling
	//    its visual nor nine-gridding it moves what shows through;
	//  - a graph takes at most four source parameters, and one that feeds a
	//    transform may be used only once.
	// So the edge is drawn with light, not with refraction.
	//
	// The lip has to stay the same width in pixels at any window size, which
	// rules out a stretched texture (and a nine-grid brush is not a legal effect
	// input). It is two linear gradients instead, one across each axis, whose
	// stops are laid out per window by UpdateLayout. Each gradient packs three
	// signals into one parameter: red is the specular line, green the inner
	// shadow and blue the frosted glow, all premultiplied under alpha.
	constexpr wchar_t c_lipAcrossParameter[]{ L"LipAcross" };
	constexpr wchar_t c_lipDownParameter[]{ L"LipDown" };
	// The edge pinch band (see GlassScene). Four parameters in all - Backdrop,
	// Pinch and the two lips - which is Composition's limit for one graph.
	constexpr wchar_t c_pinchParameter[]{ L"Pinch" };

	// Edge bits for UpdateLayout: which sides of the backdrop are real window
	// edges. A caption-only backdrop has no lip along its bottom, where it meets
	// the client area.
	enum EdgeBits : uint8_t
	{
		LeftEdge = 1 << 0,
		TopEdge = 1 << 1,
		RightEdge = 1 << 2,
		BottomEdge = 1 << 3,
		AllEdges = LeftEdge | TopEdge | RightEdge | BottomEdge
	};

	// Gradient brushes arrived in 1809; before that there is no lip to draw.
	inline bool IsSupported()
	{
		return os::buildNumber >= 17763;
	}

	// glassIntensity runs 0..1 in the GUI. Even at 0 the edge should still be
	// visible - that is what makes this material differ from the plain blur.
	inline float Strength(float glassIntensity)
	{
		return 0.35f + 0.65f * std::clamp(glassIntensity, 0.f, 1.f);
	}

	inline wuc::CompositionBrush CreateBrush(
		const wuc::Compositor& compositor,
		const wu::Color& tintColor,
		float tintOpacity,
		float blurAmount,
		float glassIntensity
	)
	{
		// Fully opaque tint => nothing to see through, just a solid brush.
		if (static_cast<float>(tintColor.A) * tintOpacity == 255.f)
		{
			return compositor.CreateColorBrush(tintColor);
		}

		const float strength{ Strength(glassIntensity) };

		auto blurEffect{ winrt::make_self<GaussianBlurEffect>() };
		blurEffect->SetName(L"Blur");
		blurEffect->SetBorderMode(D2D1_BORDER_MODE_HARD);
		blurEffect->SetBlurAmount(blurAmount > 0.f ? blurAmount : 1.f);
		blurEffect->SetOptimizationMode(
			CommonDef::g_configData.blurQuality == blurQuality::Speed ?
			D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED :
			D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY
		);
		blurEffect->SetInput(wuc::CompositionEffectSourceParameter{ L"Backdrop" });

		// Glass lets colour through richer than frosted plastic does.
		auto vibrancyEffect{ winrt::make_self<SaturationEffect>() };
		vibrancyEffect->SetName(L"Vibrancy");
		vibrancyEffect->SetSaturation(1.f + 0.35f * strength);
		vibrancyEffect->SetInput(*blurEffect);

		// The edge pinch (Windows 11): a band of compacted surroundings, drawn
		// into a surface by GlassScene, blended into the blurred pane and UNDER
		// the tint - so the titlebar's shading, gradient, title and buttons all
		// sit on top of it. Transparent until a band has been rendered.
		// Screen, not over: the reflection only ever adds light, the way a
		// reflection on glass does - bright surroundings glint on the pane,
		// dark ones leave the blur showing. Laid over, it covered the blur at
		// the rim like a sharp picture pasted on.
		auto pinchedEffect{ winrt::make_self<BlendEffect>() };
		pinchedEffect->SetName(L"Pinched");
		pinchedEffect->SetBlendMode(D2D1_BLEND_MODE_SCREEN);
		pinchedEffect->SetBackground(*vibrancyEffect);
		pinchedEffect->SetForeground(wuc::CompositionEffectSourceParameter{ c_pinchParameter });

		winrt::Windows::Graphics::Effects::IGraphicsEffect paneEffect{ *pinchedEffect };
		if (static_cast<float>(tintColor.A) * tintOpacity != 0.f)
		{
			auto tintColorEffect{ winrt::make_self<ColorSourceEffect>() };
			tintColorEffect->SetName(L"TintColor");
			tintColorEffect->SetColor(tintColor);

			auto tintOpacityEffect{ winrt::make_self<OpacityEffect>() };
			tintOpacityEffect->SetName(L"TintOpacity");
			tintOpacityEffect->SetOpacity(tintOpacity);
			tintOpacityEffect->SetInput(*tintColorEffect);

			auto tintedEffect{ winrt::make_self<CompositeStepEffect>() };
			tintedEffect->SetName(L"Tinted");
			tintedEffect->SetDestination(*pinchedEffect);
			tintedEffect->SetSource(*tintOpacityEffect);
			paneEffect = *tintedEffect;
		}

		// The two lip gradients, united. Needed twice - once for the shadow and
		// once for the light - and the graph must be a tree, so it is built
		// twice; the source parameters themselves may be shared because nothing
		// transforms them.
		auto makeLip = [](const wchar_t* name)
		{
			auto lip{ winrt::make_self<CompositeStepEffect>() };
			lip->SetName(name);
			lip->SetDestination(wuc::CompositionEffectSourceParameter{ c_lipAcrossParameter });
			lip->SetSource(wuc::CompositionEffectSourceParameter{ c_lipDownParameter });
			return lip;
		};

		// Shadow: black, as opaque as the green channel.
		float shadeMatrix[20]{};
		shadeMatrix[1 * 4 + 3] = 1.f;
		auto shadeEffect{ winrt::make_self<ChannelMatrixEffect>() };
		shadeEffect->SetName(L"Shade");
		shadeEffect->SetMatrix(shadeMatrix);
		shadeEffect->SetInput(*makeLip(L"ShadeLip"));

		auto shadedEffect{ winrt::make_self<CompositeStepEffect>() };
		shadedEffect->SetName(L"Shaded");
		shadedEffect->SetDestination(paneEffect);
		shadedEffect->SetSource(*shadeEffect);

		// Light: white, as opaque as specular (red) plus glow (blue).
		float lightMatrix[20]{};
		for (int column = 0; column < 4; ++column)
		{
			lightMatrix[0 * 4 + column] = 1.f;
			lightMatrix[2 * 4 + column] = 1.f;
		}
		auto lightEffect{ winrt::make_self<ChannelMatrixEffect>() };
		lightEffect->SetName(L"Light");
		lightEffect->SetMatrix(lightMatrix);
		lightEffect->SetInput(*makeLip(L"LightLip"));

		auto litEffect{ winrt::make_self<CompositeStepEffect>() };
		litEffect->SetName(L"Lit");
		litEffect->SetDestination(*shadedEffect);
		litEffect->SetSource(*lightEffect);

		auto effectBrush{ compositor.CreateEffectFactory(*litEffect).CreateBrush() };
		effectBrush.SetSourceParameter(L"Backdrop", compositor.CreateBackdropBrush());

		// Transparent until UpdateLayout knows the window size, so a window that
		// is never laid out simply shows the pane without a lip.
		auto makeGradient = [&](wfn::float2 endPoint)
		{
			auto gradient{ compositor.CreateLinearGradientBrush() };
			gradient.StartPoint({ 0.f, 0.f });
			gradient.EndPoint(endPoint);
			gradient.ColorStops().Append(compositor.CreateColorGradientStop(0.f, { 0, 0, 0, 0 }));
			gradient.ColorStops().Append(compositor.CreateColorGradientStop(1.f, { 0, 0, 0, 0 }));
			return gradient;
		};
		effectBrush.SetSourceParameter(c_lipAcrossParameter, makeGradient({ 1.f, 0.f }));
		effectBrush.SetSourceParameter(c_lipDownParameter, makeGradient({ 0.f, 1.f }));

		// No surface yet - creating one here, on DWM's render path, is exactly
		// what crashed dwm.exe before. GlassScene attaches it later, from DWM's
		// own message loop. Aligned top-left at natural size, so the band lands
		// where it is drawn in the surface.
		auto pinchBrush{ compositor.CreateSurfaceBrush() };
		pinchBrush.Stretch(wuc::CompositionStretch::None);
		pinchBrush.HorizontalAlignmentRatio(0.f);
		pinchBrush.VerticalAlignmentRatio(0.f);
		effectBrush.SetSourceParameter(c_pinchParameter, pinchBrush);
		return effectBrush;
	}

	// The band input of a liquid-glass brush, or null for any other brush.
	inline wuc::CompositionSurfaceBrush GetPinchBrush(const wuc::CompositionBrush& brush) try
	{
		auto effectBrush{ brush ? brush.try_as<wuc::CompositionEffectBrush>() : nullptr };
		if (!effectBrush)
			return nullptr;
		auto source{ effectBrush.GetSourceParameter(c_pinchParameter) };
		return source ? source.try_as<wuc::CompositionSurfaceBrush>() : nullptr;
	}
	catch (...)
	{
		return nullptr;
	}

	namespace Detail
	{
		// The lip at one depth (px in from an edge).
		struct LipSample
		{
			float depth;
			float glow;
			float light;
			float shade;
		};

		inline float Sample(const std::pair<float, float>* curve, size_t count, float x)
		{
			if (x <= curve[0].first)
				return curve[0].second;
			for (size_t i = 1; i < count; ++i)
			{
				if (x <= curve[i].first)
				{
					const float span{ (std::max)(curve[i].first - curve[i - 1].first, 1e-4f) };
					const float t{ (x - curve[i - 1].first) / span };
					return curve[i - 1].second + t * (curve[i].second - curve[i - 1].second);
				}
			}
			return curve[count - 1].second;
		}

		// rim <= 0 means "no lip on this edge".
		inline std::vector<LipSample> Profile(float rim, float light, float glow, float shade)
		{
			std::vector<LipSample> samples;
			if (rim < 1.f)
			{
				samples.push_back({ 0.f, 0.f, 0.f, 0.f });
				return samples;
			}

			// The specular line is a fixed couple of pixels whatever the rim
			// width: it is the edge catching the light, not the edge's thickness.
			static constexpr std::pair<float, float> lightCurve[]
			{
				{ 0.f, 0.6f }, { 0.75f, 1.f }, { 1.75f, 0.55f }, { 3.5f, 0.15f }, { 6.f, 0.f }
			};
			// The shadow sits just inside the highlight.
			static constexpr std::pair<float, float> shadeCurve[]
			{
				{ 0.f, 0.f }, { 2.f, 0.f }, { 3.5f, 1.f }, { 6.f, 0.35f }, { 10.f, 0.f }
			};

			float depths[]{ 0.f, 0.75f, 1.75f, 3.5f, 6.f, 10.f, rim * 0.25f, rim * 0.5f, rim * 0.75f, rim };
			std::sort(std::begin(depths), std::end(depths));
			for (float depth : depths)
			{
				if (depth > rim || (!samples.empty() && depth - samples.back().depth < 0.2f))
					continue;
				// The glow is the edge's thickness: strongest at the very edge,
				// curving off to nothing at the inner side of the lip.
				const float inward{ 1.f - depth / rim };
				samples.push_back({
					depth,
					glow * inward * inward,
					light * Sample(lightCurve, std::size(lightCurve), depth),
					shade * Sample(shadeCurve, std::size(shadeCurve), depth)
				});
			}
			if (samples.back().depth < rim)
				samples.push_back({ rim, 0.f, 0.f, 0.f });
			return samples;
		}

		// Gradient stops take straight colours, but the effect reads the
		// premultiplied result, so alpha is the largest of the three signals
		// and each channel is that signal divided by it.
		inline wu::Color Encode(const LipSample& sample)
		{
			const float alpha{ (std::max)({ sample.glow, sample.light, sample.shade }) };
			if (alpha <= 0.f)
				return { 0, 0, 0, 0 };
			auto toByte = [](float v)
			{
				return static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(v * 255.f)), 0, 255));
			};
			return { toByte(alpha), toByte(sample.light / alpha), toByte(sample.shade / alpha), toByte(sample.glow / alpha) };
		}

		// Lays the near edge's profile from offset 0 inward and the far edge's
		// from offset 1 inward, along an axis 'length' px long.
		inline void Lay(
			const wuc::CompositionLinearGradientBrush& gradient,
			float length,
			const std::vector<LipSample>& nearEdge,
			const std::vector<LipSample>& farEdge
		)
		{
			const float half{ length * 0.5f };
			std::vector<std::pair<float, wu::Color>> stops;
			stops.reserve(nearEdge.size() + farEdge.size());
			for (const auto& sample : nearEdge)
				stops.emplace_back((std::min)(sample.depth, half) / length, Encode(sample));
			for (auto it = farEdge.rbegin(); it != farEdge.rend(); ++it)
				stops.emplace_back(1.f - (std::min)(it->depth, half) / length, Encode(*it));

			// During a resize this runs every frame, so reuse the stops when the
			// shape of the profile has not changed and only move them.
			auto collection{ gradient.ColorStops() };
			if (collection.Size() == stops.size())
			{
				for (uint32_t i = 0; i < collection.Size(); ++i)
				{
					auto stop{ collection.GetAt(i) };
					stop.Offset(stops[i].first);
					stop.Color(stops[i].second);
				}
				return;
			}

			auto compositor{ gradient.Compositor() };
			collection.Clear();
			for (const auto& [offset, color] : stops)
				collection.Append(compositor.CreateColorGradientStop(offset, color));
		}
	}

	// Called by the backdrop visual whenever its size, its edges or its brush
	// change: places the lip in pixels.
	//
	// This runs on DWM's render path, so nothing may escape it. It is routinely
	// handed a brush that is not a liquid-glass brush (the solid brush used for
	// an opaque tint, or the plain blur fallback); those simply have no lip
	// parameters and are left alone.
	inline void UpdateLayout(
		const wuc::CompositionBrush& brush,
		float width,
		float height,
		uint8_t edges,
		int rimThickness,
		float glassIntensity
	) try
	{
		if (!brush || width < 1.f || height < 1.f)
			return;

		auto effectBrush{ brush.try_as<wuc::CompositionEffectBrush>() };
		if (!effectBrush)
			return;
		auto across{ effectBrush.GetSourceParameter(c_lipAcrossParameter).try_as<wuc::CompositionLinearGradientBrush>() };
		auto down{ effectBrush.GetSourceParameter(c_lipDownParameter).try_as<wuc::CompositionLinearGradientBrush>() };
		if (!across || !down)
			return;

		// A thin specular line and a faint shadow inside it - no frosted glow:
		// spread across the whole lip, the glow read as a white frame round
		// the window rather than as the edge of a pane of glass.
		const float strength{ Strength(glassIntensity) };
		const float light{ 0.6f * strength };
		const float glow{ 0.f };
		const float shade{ 0.18f * strength };

		const float rim{ static_cast<float>(std::clamp(rimThickness, 1, 64)) };
		const float rimAcross{ (std::min)(rim, width * 0.5f) };
		const float rimDown{ (std::min)(rim, height * 0.5f) };
		auto profile = [&](uint8_t edge, float edgeRim, float edgeLight)
		{
			return Detail::Profile((edges & edge) ? edgeRim : 0.f, edgeLight, glow, shade);
		};

		// Light from above and a little to the left. The far edges still pick
		// up some of it, reflected back through the thickness of the glass.
		Detail::Lay(across, width, profile(LeftEdge, rimAcross, light * 0.7f), profile(RightEdge, rimAcross, light * 0.4f));
		Detail::Lay(down, height, profile(TopEdge, rimDown, light), profile(BottomEdge, rimDown, light * 0.5f));
	}
	catch (...)
	{
		// Device lost, or a brush we did not expect. Leaving the lip as it was
		// is always safe; taking dwm.exe down is not.
	}
}
