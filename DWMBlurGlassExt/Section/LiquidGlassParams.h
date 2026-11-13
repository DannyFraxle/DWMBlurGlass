/**
 * FileName: LiquidGlassParams.h
 *
 * The constant buffer of Shaders/LiquidGlass.hlsl, as C++. Kept on its own so
 * the pass and any offline test of the shader share one definition.
*/
#pragma once

namespace MDWMBlurGlassExt::LiquidGlassPass
{
	constexpr int kMaxShaderOccluders = 8;

	// Matches cbuffer Params in Shaders/LiquidGlass.hlsl, member for member.
	// HLSL packs into float4 registers, so every group below fills exactly one.
	struct ShaderParams
	{
		float quad[4];          // area to rasterise, target px
		float rect[4];          // glass rect, target px (disabled edges pushed out)

		float invTarget[2];
		float rim;              // band width, px
		float cornerRadius;     // px

		float reach;            // outside strip compacted into the band, px
		float gamma;
		float chromatic;
		float highlight;

		float lightDir[2];      // unit vector toward the light
		float fade;             // 0..1
		float debug;

		float occluderCount;
		float pad0[3];

		float occluders[kMaxShaderOccluders][4];
	};
	static_assert(sizeof(ShaderParams) == 16 * 14, "ShaderParams must match the HLSL cbuffer layout");
}
