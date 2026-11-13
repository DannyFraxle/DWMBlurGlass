/**
 * FileName: GlassScene.h
 *
 * The UI-thread half of the Liquid Glass edge pinch. It knows where every glass
 * region is on screen and which windows are stacked above it, and publishes
 * that to the render thread through GlassRectRegistry, where LiquidGlassPass
 * draws the pinch at the end of each frame.
 *
 * Everything here runs on DWM's UI thread (udwm), where the window list can be
 * walked safely.
*/
#pragma once
#include <Windows.h>
#include <cstdint>
#include "winrt.h"
#include <winrt/Windows.UI.Composition.h>

namespace MDWMBlurGlassExt::GlassScene
{
	// Whether the pinch is wanted at all: Windows 11, LiquidGlass selected,
	// glassPinch on in config.ini and no no_pinch marker file. Re-read by
	// Refresh().
	bool IsEnabled();
	void Refresh();

	// From a glass window's backdrop visual whenever it validates: its glass
	// region in screen px, which sides of it are real window edges (see
	// GlassRects::EdgeBits), and a visual to keep animating while the rim
	// settles, so DWM keeps producing frames for the pass to fade it in on.
	void UpdateGlass(const void* owner, HWND hwnd, const RECT& screenRect, uint8_t edges,
		bool active, const winrt::Windows::UI::Composition::Visual& kickVisual);
	void RemoveGlass(const void* owner);

	// After any top-level window validates: recompute what covers each glass
	// region and publish. Cheap enough to run on every validation.
	void Flush();

	// Forget everything and publish an empty scene.
	void Reset();
}
