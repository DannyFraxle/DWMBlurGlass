/**
 * FileName: ReconLog.cpp
 *
 * See ReconLog.h.
*/
#include "ReconLog.h"
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace MDWMBlurGlassExt::ReconLog
{
	namespace
	{
		// One page-aligned arena. Writes past the end are dropped rather than
		// wrapped: for recon, the first N lines are the interesting ones, and a
		// ring buffer would risk tearing a line across the flush boundary.
		constexpr size_t kArenaSize = 512 * 1024;
		constexpr size_t kLineMax = 1024;

		char g_arena[kArenaSize];
		std::atomic_size_t g_head{ 0 };   // next free byte
		std::atomic_size_t g_flushed{ 0 };// bytes already written to disk
		std::atomic_bool g_running{ false };
		std::atomic_bool g_overflowed{ false };

		wchar_t g_path[MAX_PATH]{};
		HANDLE g_thread{ nullptr };
		HANDLE g_stopEvent{ nullptr };

		// Only bytes below this are known to be fully memcpy'd. A writer reserves
		// its range with a CAS and fills it immediately afterwards, so a range
		// that was already reserved one tick ago is certainly complete by now.
		std::atomic_size_t g_settled{ 0 };

		void AppendToFile()
		{
			const size_t head = g_settled.exchange(g_head.load(std::memory_order_acquire),
				std::memory_order_acq_rel);
			size_t flushed = g_flushed.load(std::memory_order_relaxed);
			if (head <= flushed || !g_path[0])
				return;

			HANDLE file = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ,
				nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return;

			DWORD written = 0;
			if (WriteFile(file, g_arena + flushed, static_cast<DWORD>(head - flushed), &written, nullptr))
				g_flushed.store(flushed + written, std::memory_order_relaxed);
			CloseHandle(file);
		}

		DWORD WINAPI FlusherProc(LPVOID)
		{
			for (;;)
			{
				const DWORD r = WaitForSingleObject(g_stopEvent, 1000);
				AppendToFile();
				if (r == WAIT_OBJECT_0)
					break;
			}
			return 0;
		}
	}

	void Start(const wchar_t* path)
	{
		if (g_running.exchange(true))
			return;

		wcscpy_s(g_path, path);

		// Make sure the directory exists, then truncate any previous run.
		wchar_t dir[MAX_PATH]{};
		wcscpy_s(dir, path);
		if (wchar_t* slash = wcsrchr(dir, L'\\'))
		{
			*slash = L'\0';
			CreateDirectoryW(dir, nullptr);
		}
		HANDLE truncate = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (truncate != INVALID_HANDLE_VALUE)
			CloseHandle(truncate);

		g_head.store(0, std::memory_order_relaxed);
		g_settled.store(0, std::memory_order_relaxed);
		g_flushed.store(0, std::memory_order_relaxed);
		g_overflowed.store(false, std::memory_order_relaxed);

		g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_thread = CreateThread(nullptr, 0, FlusherProc, nullptr, 0, nullptr);
	}

	void Stop()
	{
		if (!g_running.exchange(false))
			return;

		if (g_stopEvent)
			SetEvent(g_stopEvent);
		if (g_thread)
		{
			WaitForSingleObject(g_thread, 3000);
			CloseHandle(g_thread);
			g_thread = nullptr;
		}
		if (g_stopEvent)
		{
			CloseHandle(g_stopEvent);
			g_stopEvent = nullptr;
		}
		AppendToFile();
		AppendToFile();
	}

	void Write(const char* fmt, ...)
	{
		if (!g_running.load(std::memory_order_relaxed))
			return;

		char line[kLineMax];
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
		va_end(ap);
		if (n < 0)
			return;
		if (static_cast<size_t>(n) > sizeof(line) - 2)
			n = static_cast<int>(sizeof(line) - 2);
		line[n++] = '\r';
		line[n++] = '\n';

		// Reserve with CAS rather than fetch_add: the flusher reads everything
		// below g_head, so g_head must never point past the arena, even briefly.
		size_t offset = g_head.load(std::memory_order_relaxed);
		for (;;)
		{
			if (offset + static_cast<size_t>(n) > kArenaSize)
			{
				g_overflowed.store(true, std::memory_order_relaxed);
				return;
			}
			if (g_head.compare_exchange_weak(offset, offset + static_cast<size_t>(n),
				std::memory_order_acq_rel, std::memory_order_relaxed))
				break;
		}
		memcpy(g_arena + offset, line, static_cast<size_t>(n));
	}

	void Flush()
	{
		AppendToFile();
		AppendToFile();
	}
}
