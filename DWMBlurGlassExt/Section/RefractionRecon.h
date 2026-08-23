/**
 * FileName: RefractionRecon.h
 *
 * Reconnaissance-only instrumentation. Hooks candidate DWM render-path
 * functions, logs what COM objects they carry, and ALWAYS calls the original
 * unchanged. It must never alter rendering.
 *
 * Enabled only while effectType == LiquidGlass. Output goes to
 * C:\ProgramData\DWMBlurGlassRecon\recon.log
*/
#pragma once
#include "DWMStruct.h"

namespace MDWMBlurGlassExt::RefractionRecon
{
	void Attach();
	void Detach();
	void Refresh();

	DWORD64 STDMETHODCALLTYPE CRenderingTechnique_ExecuteBlur(
		DWM::CRenderingTechnique* This,
		DWM::CDrawingContext* drawingContext,
		const DWM::EffectInput* input,
		const D2D_VECTOR_2F* blurAmount,
		DWM::EffectInput* output
	);

	DWORD64 STDMETHODCALLTYPE CRenderingTechnique_ExecuteBlur24h2(
		DWM::CRenderingTechnique* This,
		DWM::CDrawingContext* drawingContext,
		const DWM::EffectInput* input,
		const D2D_VECTOR_2F* blurAmount,
		const D2D_SIZE_F* size,
		DWM::EffectInput* output
	);

	HRESULT STDMETHODCALLTYPE CCustomBlur_Draw(
		DWM::Core::CCustomBlur* This,
		DWM::Core::CDrawingContext* drawingContext,
		const D2D1_RECT_F& destinationRect,
		const D2D1_POINT_2F* point,
		D2D1_INTERPOLATION_MODE interpolationMode,
		D2D1_COMPOSITE_MODE compositeMode
	);
}
