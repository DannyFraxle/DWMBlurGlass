/**
 * FileName: D3DPresentHook.h
 *
 * Where the Liquid Glass edge pinch looks at the screen. Composition cannot
 * remap what is behind a window at all (see Backdrops/LiquidGlassBackdrop.hpp),
 * so the render thread reads the finished frame instead - inside dwmcore's
 * CDDisplayRenderTarget::Present, when the flip asks
 * CDDisplaySwapChainBuffer::GetD3D11Resource for the buffer it is about to put
 * on screen - and renders each titlebar's band of compacted surroundings from
 * it (LiquidGlassPass). The frame itself is never written; the bands go back
 * into Composition, under the glass tint (GlassScene).
 *
 * Two dwmcore functions are hooked, both resolved by symbol like every other
 * hook in this project, and each checked to have an address of its own
 * (dwmcore folds identical one-line functions together):
 *  - CDDisplayRenderTarget::Present             brackets the flip
 *  - CDDisplaySwapChainBuffer::GetD3D11Resource the buffer being flipped
 *
 * Everything here runs on DWM's render thread. A fault there crash-loops the
 * desktop, so: no allocation and no file I/O on the hot path, SEH around every
 * entry point, and an arm-counter fuse that refuses to re-arm after repeated
 * crashes (C:\ProgramData\DWMBlurGlassRecon\arm.count).
*/
#pragma once
#include <Windows.h>

namespace MDWMBlurGlassExt::D3DPresentHook
{
	void Attach();
	void Detach();
	// Arms or disarms to match GlassScene::IsEnabled().
	void Refresh();
}
