/**
 * FileName: GlassScene.h
 *
 * The UI-thread half of the Liquid Glass edge pinch.
 *
 * It tells the render thread where each titlebar is on screen (through
 * GlassRectRegistry), and when the render thread has rendered a titlebar's
 * band of compacted surroundings (PinchBands), it draws that band into the
 * window's own Composition surface - which the glass material composites
 * between its blur and its tint. So the pinch sits UNDER the titlebar shading,
 * title and buttons, moves with the window and is covered by whatever is
 * stacked above it, all by DWM itself.
*/
#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include "winrt.h"
#include <winrt/Windows.UI.Composition.h>

namespace MDWMBlurGlassExt::GlassScene
{
	// Whether the pinch is wanted at all: Windows 11, LiquidGlass selected,
	// glassPinch on in config.ini and no no_pinch marker file. Re-read by
	// Refresh(), which also hooks DWM's message window for band uploads.
	bool IsEnabled();
	void Refresh();

	// From a glass window's backdrop visual whenever it validates.
	//  screenRect  - the titlebar, screen px
	//  edges       - which sides of it are real window edges (GlassRects::EdgeBits)
	//  kickVisual  - kept animating while the band settles, so DWM keeps
	//                presenting frames for the render thread to look at
	//  pinchBrush  - the glass material's band input (see LiquidGlassBackdrop)
	//  bandOffset  - where the titlebar starts inside the backdrop visual
	void UpdateGlass(const void* owner, HWND hwnd, const RECT& screenRect, uint8_t edges, bool active,
		const winrt::Windows::UI::Composition::Visual& kickVisual,
		const winrt::Windows::UI::Composition::CompositionSurfaceBrush& pinchBrush,
		POINT bandOffset);
	void RemoveGlass(const void* owner);

	// After any top-level window validates: re-publish what is on screen.
	void Flush();

	// Render thread: a band changed, the UI thread should upload it. Never blocks.
	void NotifyBandsChanged();

	// Render thread: a readback is waiting and nothing may present to collect
	// it; have the UI thread make DWM present a few more frames. Never blocks.
	void Nudge();

	// For the log: how long bands take from being rendered to being drawn into
	// their window, on the GPU path and the readback path.
	std::string LatencySummary();

	// Forget everything, publish an empty scene and unhook DWM's window.
	void Reset();
}
