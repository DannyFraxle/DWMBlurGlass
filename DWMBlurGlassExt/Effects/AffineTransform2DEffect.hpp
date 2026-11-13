#pragma once
#include "CanvasEffect.hpp"

namespace MDWMBlurGlassExt
{
	// D2D 2DAffineTransform, with its matrix exposed as an animatable property.
	//
	// Composition accepts this effect, but ONLY directly on a surface-brush
	// source parameter: on a backdrop brush it fails with "Backdrop brushes
	// cannot be transformed", on a nested effect brush with "Effect brushes
	// cannot be transformed", and a transformed parameter may be used only once
	// per graph. So it cannot magnify the live backdrop - see
	// LiquidGlassBackdrop.hpp.
	class AffineTransform2DEffect : public CanvasEffect
	{
	public:
		AffineTransform2DEffect() : CanvasEffect{ CLSID_D2D12DAffineTransform }
		{
			m_namedProperties.emplace_back(
				L"TransformMatrix",
				D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
				NamedProperty::GRAPHICS_EFFECT_PROPERTY_MAPPING::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT
			);
			SetTransformMatrix();
			SetInterpolationMode();
			SetBorderMode();
			// Composition requests all four properties; leaving this one out is
			// what made the earlier rim graph fail with E_INVALIDARG.
			SetProperty(D2D1_2DAFFINETRANSFORM_PROP_SHARPNESS, BoxValue(0.f));
		}
		virtual ~AffineTransform2DEffect() = default;

		void SetTransformMatrix(const D2D1_MATRIX_3X2_F& matrix = D2D1::Matrix3x2F::Identity())
		{
			float values[6]{ matrix._11, matrix._12, matrix._21, matrix._22, matrix._31, matrix._32 };
			SetProperty(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, BoxValue(values));
		}
		void SetInterpolationMode(
			D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE mode = D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR)
		{
			SetProperty(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE, BoxValue(mode));
		}
		// Hard, so magnifying near the frame does not drag transparent pixels in
		// from beyond the edge of the backdrop.
		void SetBorderMode(D2D1_BORDER_MODE mode = D2D1_BORDER_MODE_HARD)
		{
			SetProperty(D2D1_2DAFFINETRANSFORM_PROP_BORDER_MODE, BoxValue(mode));
		}

		IFACEMETHOD(GetPropertyCount)(UINT* count) override
		{
			if (!count)
				return E_POINTER;
			*count = 4;
			return S_OK;
		}

		// A scale about a point, as the matrix D2D wants.
		static D2D1_MATRIX_3X2_F ScaleAbout(float scale, float centreX, float centreY)
		{
			D2D1_MATRIX_3X2_F m{};
			m._11 = scale; m._12 = 0.f;
			m._21 = 0.f;   m._22 = scale;
			m._31 = centreX * (1.f - scale);
			m._32 = centreY * (1.f - scale);
			return m;
		}
	};
}
