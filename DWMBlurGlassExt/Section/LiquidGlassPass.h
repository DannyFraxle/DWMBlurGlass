/**
 * FileName: LiquidGlassPass.h
 *
 * The Liquid Glass edge pinch, drawn on the finished frame. Called from
 * D3DPresentHook once per frame on DWM's render thread, just before present,
 * with the swapchain buffer DWM has just composed into.
 *
 * Device objects are created lazily on DWM's own device and cached, so a
 * steady frame costs a few state changes, four small copies and one quad per
 * glass window.
*/
#pragma once
#include <d3d11.h>
#include <Windows.h>

namespace MDWMBlurGlassExt::LiquidGlassPass
{
	// Draws the pinch for every published glass window whose HWND is in
	// `rendered` (the windows DWM drew this frame). Leaves the frame exactly as
	// DWM composed it if anything is missing or wrong.
	void RenderPinch(ID3D11Texture2D* target, UINT width, UINT height, const HWND* rendered, int renderedCount);

	// Drops every cached device resource. Safe to call when nothing is cached.
	void Shutdown();
}
