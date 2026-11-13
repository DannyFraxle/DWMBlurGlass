/**
 * FileName: D3DPresentHook.h
 *
 * Where the Liquid Glass edge pinch runs. Composition cannot remap the
 * backdrop of a window at all (see Backdrops/LiquidGlassBackdrop.hpp), so the
 * pinch is drawn onto the finished frame instead: dwmcore's
 * CDDisplaySwapChain::PrePresent runs once per frame with composition done and
 * the present not yet sent, and the swapchain buffer is still writable.
 *
 * Four dwmcore functions are hooked, all resolved by symbol like every other
 * hook in this project:
 *  - CDeviceTextureTarget::GetRenderTargetView  which target this frame used
 *  - CDDisplaySwapChainBuffer::GetD3D11Resource which textures are buffers
 *  - CWindowNode::RenderContent                 which windows were drawn
 *  - CDDisplaySwapChain::PrePresent             where the pass runs
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
