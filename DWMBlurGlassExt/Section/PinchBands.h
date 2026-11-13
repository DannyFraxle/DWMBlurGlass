/**
 * FileName: PinchBands.h
 *
 * The Liquid Glass edge pinch, in flight between DWM's two threads: the render
 * thread (LiquidGlassPass) renders each titlebar's band from the finished
 * frame; the UI thread (GlassScene) draws it into that window's Composition
 * surface, where the glass material composites it under its tint.
 *
 * A band travels one of two ways:
 *  - as pixels, read back to the CPU. A frame or two slower, but it can be
 *    hashed, so a still window whose surroundings have not changed costs no
 *    upload and no recomposition. Used once a window has settled.
 *  - as a shared GPU texture (keyed mutex: key 1 = rendered, waiting for the
 *    UI thread; key 0 = free to render into). No readback, so it reaches the
 *    screen as soon as the UI thread commits. Used while a window moves.
 *
 * The render thread never blocks here: if the UI thread happens to be reading,
 * the publish is skipped and the next frame tries again.
*/
#pragma once
#include <Windows.h>
#include <cstdint>
#include <vector>

namespace MDWMBlurGlassExt::PinchBands
{
	struct Band
	{
		RECT rect{};             // the titlebar's screen rect it was rendered for
		UINT width{ 0 };
		UINT height{ 0 };
		HANDLE shared{ nullptr };   // non-null: on the GPU, in this shared texture
		uint64_t renderedAt{ 0 };   // QueryPerformanceCounter when it was rendered
		std::vector<uint32_t> pixels;   // otherwise: tightly packed, width * 4 bytes a row
	};

	// Render side. `pixels` is premultiplied B8G8R8A8, `rowPitch` bytes apart.
	// Returns true when the band is new or differs from the last one published
	// for this owner - i.e. when the UI thread has something to upload.
	bool Publish(const void* owner, const RECT& rect, UINT width, UINT height,
		const void* pixels, UINT rowPitch, uint64_t renderedAt);

	// Render side, holding `shared`'s keyed mutex: records that it now holds
	// this owner's newest band. False if the UI thread is reading right now -
	// then nothing is recorded and the caller must not hand the texture over.
	bool PublishShared(const void* owner, const RECT& rect, UINT width, UINT height,
		HANDLE shared, uint64_t renderedAt);

	// UI side. Copies the band out if it is newer than `seenVersion` (and
	// updates it).
	bool Take(const void* owner, uint64_t& seenVersion, Band& band);

	// UI side: the newest version published for this owner (0 if none).
	uint64_t Version(const void* owner);

	// The UI thread found it cannot draw shared textures; the render thread
	// goes back to reading every band back.
	void DisableShared();
	bool SharedEnabled();

	// UI side: the window has no glass any more.
	void Forget(const void* owner);
	void Clear();
}
