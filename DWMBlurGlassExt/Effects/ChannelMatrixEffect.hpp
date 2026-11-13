#pragma once
#include "CanvasEffect.hpp"

namespace MDWMBlurGlassExt
{
	// A D2D ColorMatrix that works on the raw, premultiplied channels and clamps
	// its output - for routing one channel of an input into another, e.g. turning
	// a value carried in the red channel into a white layer of that opacity.
	//
	// All three of the effect's properties are set up front. Composition asks for
	// EVERY property a D2D effect defines, whatever GetPropertyCount returns, and
	// fails the whole graph if one is missing - which is why the older
	// ColorMatrixEffect wrapper, which leaves CLAMP_OUTPUT unset, cannot be used.
	class ChannelMatrixEffect : public CanvasEffect
	{
	public:
		ChannelMatrixEffect() : CanvasEffect{ CLSID_D2D1ColorMatrix }
		{
			float zero[20]{};
			SetMatrix(zero);
			SetProperty(D2D1_COLORMATRIX_PROP_ALPHA_MODE, BoxValue(D2D1_COLORMATRIX_ALPHA_MODE_STRAIGHT));
			SetProperty(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, BoxValue(true));
		}
		virtual ~ChannelMatrixEffect() = default;

		// Row-major 5x4, as D2D lays it out: rows are the input channels
		// (r, g, b, a, then a constant offset), columns the output channels.
		void SetMatrix(float(&matrix)[20])
		{
			SetProperty(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, BoxValue(matrix));
		}
	};
}
