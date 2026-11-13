/**
 * FileName: GlassRectRegistry.cpp
 *
 * See GlassRectRegistry.h.
*/
#include "GlassRectRegistry.h"
#include <atomic>

namespace MDWMBlurGlassExt::GlassRects
{
	namespace
	{
		Entry g_entries[kMaxEntries]{};
		int g_count{ 0 };
		Display g_display{};

		// Even = stable, odd = a write is in progress.
		std::atomic_uint32_t g_seq{ 0 };

		// Writers only. The render thread never touches this.
		SRWLOCK g_writeLock = SRWLOCK_INIT;

		// A reader that keeps losing the race skips the frame rather than
		// spinning on the render thread. Writes are rare.
		constexpr int kMaxRetries = 8;
	}

	void Publish(const Entry* entries, int count, const Display& display)
	{
		if (count < 0 || !entries)
			count = 0;
		if (count > kMaxEntries)
			count = kMaxEntries;

		AcquireSRWLockExclusive(&g_writeLock);
		g_seq.fetch_add(1, std::memory_order_acq_rel);
		for (int i = 0; i < count; ++i)
			g_entries[i] = entries[i];
		g_count = count;
		g_display = display;
		g_seq.fetch_add(1, std::memory_order_release);
		ReleaseSRWLockExclusive(&g_writeLock);
	}

	void Clear()
	{
		Publish(nullptr, 0, Display{});
	}

	int Snapshot(Entry* out, int maxOut, Display* display)
	{
		if (!out || maxOut <= 0 || !display)
			return 0;

		for (int attempt = 0; attempt < kMaxRetries; ++attempt)
		{
			const uint32_t before = g_seq.load(std::memory_order_acquire);
			if (before & 1u)
				continue;

			const int count = g_count < maxOut ? g_count : maxOut;
			for (int i = 0; i < count; ++i)
				out[i] = g_entries[i];
			*display = g_display;

			std::atomic_thread_fence(std::memory_order_acquire);
			if (g_seq.load(std::memory_order_relaxed) == before)
				return count;
		}
		*display = Display{};
		return 0;
	}
}
