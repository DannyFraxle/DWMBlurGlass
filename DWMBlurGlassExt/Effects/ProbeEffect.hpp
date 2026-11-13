#pragma once
#include "CanvasEffect.hpp"

namespace MDWMBlurGlassExt
{
	// A bare CanvasEffect wrapping any CLSID, with no properties set, used only
	// to ask Composition "would you build this?".
	//
	// Worth having because the failure messages are misleading: a rejected
	// effect can report "Invalid effect input" rather than "Unsupported effect
	// type", so the only trustworthy test is to actually construct a factory for
	// a self-contained graph and see whether it throws.
	class ProbeEffect : public CanvasEffect
	{
	public:
		explicit ProbeEffect(REFCLSID effectId) : CanvasEffect{ effectId } {}
		virtual ~ProbeEffect() = default;

		// No properties are set, so report none rather than inheriting the base
		// class's source-count-as-property-count bug.
		IFACEMETHOD(GetPropertyCount)(UINT* count) override
		{
			if (!count)
				return E_POINTER;
			*count = 0;
			return S_OK;
		}
	};
}
