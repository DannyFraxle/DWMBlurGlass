/**
 * FileName: LiquidGlassPass.h
 *
 * The render-thread half of the Liquid Glass edge pinch. Called from
 * D3DPresentHook once per present on DWM's render thread, with the finished
 * frame. It only READS that frame: for each titlebar it renders the band of
 * compacted surroundings into a small texture of its own and reads it back
 * without waiting, handing the pixels to the UI thread (PinchBands), which
 * puts them in the window's Composition surface, under the glass tint.
*/
#pragma once
#include <d3d11.h>
#include <Windows.h>

namespace MDWMBlurGlassExt::LiquidGlassPass
{
	// Returns true when a band changed and the UI thread should upload it.
	bool RenderBands(ID3D11Texture2D* frame, UINT width, UINT height);

	// Drops every cached device resource. Safe to call when nothing is cached.
	void Shutdown();

	// Running totals for the log.
	struct Stats
	{
		long rendered;      // bands rendered from a frame
		long published;     // bands that came back changed, for upload
		long throttled;     // rendered for change detection only (window still, nothing changing)
		long shared;        // handed to the UI thread on the GPU (window moving)
		long drained;       // readbacks collected
		long hashChanges;   // readbacks that differed from the one before
		long shifted;       // renders that followed a window DWM had moved since the scene was published
	};
	Stats GetStats();

	// Whether the last frame rendered had any band live (moving, or just
	// changed). For the log.
	bool AnyLive();
}
