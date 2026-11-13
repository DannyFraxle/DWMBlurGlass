/**
 * FileName: GlassRectRegistry.h
 *
 * Where the glass is, published by DWM's UI thread (GlassScene) and read once
 * per frame by the pinch pass on DWM's render thread (LiquidGlassPass).
 *
 * The UI side always publishes the whole scene at once, so the render side
 * never sees half of one update and half of another. It is a seqlock: the
 * writer bumps a version either side of the copy; the reader copies and
 * retries if the version moved under it. The render thread never blocks,
 * never allocates and never takes a lock.
*/
#pragma once
#include <Windows.h>
#include <cstdint>

namespace MDWMBlurGlassExt::GlassRects
{
	// Windows stacked above a glass window, so the pass does not paint across
	// whatever covers it. Past this many, the rest are merged into one bounding
	// rectangle - a little rim lost, never a rim drawn on the wrong window.
	constexpr int kMaxOccluders = 8;
	constexpr int kMaxEntries = 32;

	enum EdgeBits : uint8_t
	{
		LeftEdge = 1 << 0,
		TopEdge = 1 << 1,
		RightEdge = 1 << 2,
		BottomEdge = 1 << 3
	};

	struct Entry
	{
		HWND hwnd;
		RECT rect;                 // glass region, screen px
		uint8_t edges;             // which sides get a rim
		float cornerRadius;        // px
		bool active;
		// GetTickCount64() before which the rim is not drawn: the window, or
		// something over it, has just moved, and the composed frame may not
		// match this geometry yet.
		ULONGLONG settledAt;
		RECT occluders[kMaxOccluders];  // screen px
		int occluderCount;
	};

	// The one display the pass draws on. Only a single-monitor desktop is
	// supported: each monitor has its own swapchain, and nothing ties a
	// swapchain to a monitor origin.
	struct Display
	{
		UINT width;
		UINT height;
		bool valid;
	};

	// UI side.
	void Publish(const Entry* entries, int count, const Display& display);
	void Clear();

	// Render side. Copies into `out`, returns how many were written.
	int Snapshot(Entry* out, int maxOut, Display* display);
}
