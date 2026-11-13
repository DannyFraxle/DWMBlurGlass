#pragma once
#include "CanvasEffect.hpp"

namespace MDWMBlurGlassExt
{
	// The curved reflective edge of the glass.
	//
	// D2D lighting effects read their input's ALPHA channel as a height map and
	// derive surface normals from it themselves, so a rim whose alpha ramps up
	// at the border reads as a raised, rounded lip and lights accordingly. That
	// is the whole trick: no refraction, no displacement, just a shaped highlight
	// running around the edge - which is the cue that actually reads as "glass".
	//
	// Unlike DisplacementMap, the lighting effects are on the subset of D2D
	// effects Windows.UI.Composition will accept, so this works through the
	// ordinary backdrop path with no hooking of any kind.
	class DistantSpecularEffect : public CanvasEffect
	{
	public:
		DistantSpecularEffect() : CanvasEffect{ CLSID_D2D1DistantSpecular }
		{
			SetAzimuth();
			SetElevation();
			SetSpecularExponent();
			SetSpecularConstant();
			SetSurfaceScale();
			SetColor();
		}
		virtual ~DistantSpecularEffect() = default;

		// Direction the light comes from, in degrees around the surface.
		void SetAzimuth(float degrees = 315.f)
		{
			SetProperty(D2D1_DISTANTSPECULAR_PROP_AZIMUTH, BoxValue(degrees));
		}
		// Height of the light above the surface, in degrees.
		void SetElevation(float degrees = 55.f)
		{
			SetProperty(D2D1_DISTANTSPECULAR_PROP_ELEVATION, BoxValue(degrees));
		}
		// Tightness of the highlight; higher is a sharper, glassier glint.
		void SetSpecularExponent(float exponent = 16.f)
		{
			SetProperty(D2D1_DISTANTSPECULAR_PROP_SPECULAR_EXPONENT, BoxValue(exponent));
		}
		void SetSpecularConstant(float constant = 1.f)
		{
			SetProperty(D2D1_DISTANTSPECULAR_PROP_SPECULAR_CONSTANT, BoxValue(constant));
		}
		// How much the alpha ramp counts as height: this is what decides whether
		// the edge reads as a gentle bevel or a pronounced bead of glass.
		void SetSurfaceScale(float scale = 6.f)
		{
			SetProperty(D2D1_DISTANTSPECULAR_PROP_SURFACE_SCALE, BoxValue(scale));
		}
		void SetColor(const winrt::Windows::UI::Color& color = { 255, 255, 255, 255 })
		{
			float rgb[3]
			{
				static_cast<float>(color.R) / 255.f,
				static_cast<float>(color.G) / 255.f,
				static_cast<float>(color.B) / 255.f
			};
			SetProperty(D2D1_DISTANTSPECULAR_PROP_COLOR, BoxValue(rgb));
		}
		// CanvasEffect::GetPropertyCount reports m_effectSources.size(), not the
		// property count - an upstream bug that goes unnoticed for effects whose
		// two counts happen to be compatible. This effect has one source and six
		// properties, so without this override Composition would read only the
		// first property and never see the rest of the lighting setup.
		IFACEMETHOD(GetPropertyCount)(UINT* count) override
		{
			if (!count)
				return E_POINTER;
			*count = 6;
			return S_OK;
		}

		// SetInput comes from CanvasEffect.
	};
}
