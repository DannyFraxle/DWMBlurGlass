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
		float quad[4];          // area to rasterise, in band-texture px
		float rect[4];          // the titlebar, SCREEN px (disabled edges pushed out)

		float invTarget[2];     // 1 / band texture size
		float rim;              // band width, px
		float cornerRadius;     // px

		float reach;            // outside strip compacted into the band, px
		float gamma;
		float chromatic;
		float highlight;

		float lightDir[2];      // unit vector toward the light
		float encodeSrgb;       // 1 when the frame is linear FP16 and the band 8-bit sRGB
		float debug;

		float origin[2];        // screen px of the band texture's (0,0)
		float invFrame[2];      // 1 / composed frame size

		float occluderCount;
		float pad0[3];

		float occluders[kMaxShaderOccluders][4];
	};
	static_assert(sizeof(ShaderParams) == 16 * 15, "ShaderParams must match the HLSL cbuffer layout");
}
