/**
 * FileName: PinchBands.cpp
 *
 * See PinchBands.h.
*/
#include "PinchBands.h"
#include <atomic>
#include <cstring>

namespace MDWMBlurGlassExt::PinchBands
{
	namespace
	{
		constexpr int kMaxBands = 32;

		struct Slot
		{
			const void* owner{ nullptr };
			UINT width{ 0 };
			UINT height{ 0 };
			uint64_t version{ 0 };
			uint64_t hash{ 0 };
			RECT rect{};
			HANDLE shared{ nullptr };
			uint64_t renderedAt{ 0 };
			std::vector<uint32_t> pixels;
		};

		Slot g_slots[kMaxBands]{};
		SRWLOCK g_lock = SRWLOCK_INIT;
		uint64_t g_nextVersion{ 1 };
		std::atomic_bool g_sharedDisabled{ false };

		Slot* Find(const void* owner)
		{
			for (auto& slot : g_slots)
			{
				if (slot.owner == owner)
					return &slot;
			}
			return nullptr;
		}

		Slot* FindOrFree(const void* owner)
		{
			Slot* slot{ Find(owner) };
			return slot ? slot : Find(nullptr);
		}
	}

	bool Publish(const void* owner, const RECT& rect, UINT width, UINT height,
		const void* pixels, UINT rowPitch, uint64_t renderedAt)
	{
		if (!owner || !pixels || !width || !height)
			return false;
		// Never wait on the UI thread from the render thread.
		if (!TryAcquireSRWLockExclusive(&g_lock))
			return false;

		bool changed{ false };
		if (Slot* slot{ FindOrFree(owner) })
		{
			// FNV-1a over the rows, so an unchanged band - the common case once a
			// window has settled - costs no upload and no recomposition.
			uint64_t hash{ 1469598103934665603ull };
			const auto* src{ static_cast<const uint8_t*>(pixels) };
			for (UINT y = 0; y < height; ++y)
			{
				const auto* row{ src + static_cast<size_t>(y) * rowPitch };
				for (UINT x = 0; x < width * 4; ++x)
					hash = (hash ^ row[x]) * 1099511628211ull;
			}

			if (slot->owner != owner || slot->shared || slot->width != width || slot->height != height ||
				slot->hash != hash || slot->rect.left != rect.left || slot->rect.top != rect.top)
			{
				slot->owner = owner;
				slot->width = width;
				slot->height = height;
				slot->hash = hash;
				slot->rect = rect;
				slot->shared = nullptr;
				slot->renderedAt = renderedAt;
				slot->pixels.resize(static_cast<size_t>(width) * height);
				for (UINT y = 0; y < height; ++y)
				{
					memcpy(slot->pixels.data() + static_cast<size_t>(y) * width,
						src + static_cast<size_t>(y) * rowPitch, static_cast<size_t>(width) * 4);
				}
				slot->version = g_nextVersion++;
				changed = true;
			}
		}

		ReleaseSRWLockExclusive(&g_lock);
		return changed;
	}

	bool PublishShared(const void* owner, const RECT& rect, UINT width, UINT height,
		HANDLE shared, uint64_t renderedAt)
	{
		if (!owner || !shared || !width || !height)
			return false;
		if (!TryAcquireSRWLockExclusive(&g_lock))
			return false;

		bool published{ false };
		if (Slot* slot{ FindOrFree(owner) })
		{
			slot->owner = owner;
			slot->width = width;
			slot->height = height;
			// Whatever is read back next is compared against this, not against
			// the last band that went by readback.
			slot->hash = 0;
			slot->rect = rect;
			slot->shared = shared;
			slot->renderedAt = renderedAt;
			slot->version = g_nextVersion++;
			published = true;
		}

		ReleaseSRWLockExclusive(&g_lock);
		return published;
	}

	bool Take(const void* owner, uint64_t& seenVersion, Band& band)
	{
		AcquireSRWLockShared(&g_lock);
		bool taken{ false };
		if (const Slot* slot{ owner ? Find(owner) : nullptr }; slot && slot->version > seenVersion)
		{
			band.rect = slot->rect;
			band.width = slot->width;
			band.height = slot->height;
			band.shared = slot->shared;
			band.renderedAt = slot->renderedAt;
			if (slot->shared)
				band.pixels.clear();
			else
				band.pixels = slot->pixels;
			seenVersion = slot->version;
			taken = true;
		}
		ReleaseSRWLockShared(&g_lock);
		return taken;
	}

	uint64_t Version(const void* owner)
	{
		AcquireSRWLockShared(&g_lock);
		const Slot* slot{ owner ? Find(owner) : nullptr };
		const uint64_t version{ slot ? slot->version : 0 };
		ReleaseSRWLockShared(&g_lock);
		return version;
	}

	void DisableShared()
	{
		g_sharedDisabled.store(true);
	}

	bool SharedEnabled()
	{
		return !g_sharedDisabled.load();
	}

	void Forget(const void* owner)
	{
		if (!owner)
			return;
		AcquireSRWLockExclusive(&g_lock);
		if (Slot* slot{ Find(owner) })
			*slot = Slot{};
		ReleaseSRWLockExclusive(&g_lock);
	}

	void Clear()
	{
		AcquireSRWLockExclusive(&g_lock);
		for (auto& slot : g_slots)
			slot = Slot{};
		ReleaseSRWLockExclusive(&g_lock);
		g_sharedDisabled.store(false);
	}
}
