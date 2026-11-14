/**
 * FileName: D3DPresentHook.cpp
 *
 * See D3DPresentHook.h.
*/
#include "D3DPresentHook.h"
#include "CommonDef.h"
#include "HookDef.h"
#include "LiquidGlassPass.h"
#include "GlassRectRegistry.h"
#include "GlassScene.h"
#include "../Helper/ReconLog.h"

#include <d3d11.h>
#include <atomic>
#include <utility>

namespace MDWMBlurGlassExt::D3DPresentHook
{
	using namespace MDWMBlurGlass;
	using namespace CommonDef;

	namespace
	{
		constexpr wchar_t kReconDir[] = L"C:\\ProgramData\\DWMBlurGlassRecon";
		constexpr wchar_t kReconLog[] = L"C:\\ProgramData\\DWMBlurGlassRecon\\present.log";
		constexpr wchar_t kFusePath[] = L"C:\\ProgramData\\DWMBlurGlassRecon\\arm.count";

		// How many arms without a clean run before we give up. Two means: one
		// crash is a glitch, a second is a pattern.
		constexpr DWORD kFuseLimit = 2;
		// A run that survives this long is considered clean and clears the fuse.
		constexpr DWORD kCleanRunMs = 5000;

		std::atomic_bool g_startup{ false };
		// Set by the SEH handler on any fault: from then on every detour is a
		// straight pass-through and we never touch DWM resources again.
		std::atomic_bool g_disabled{ false };

		HANDLE g_fuseThread{ nullptr };
		HANDLE g_fuseStop{ nullptr };

		std::atomic_int g_presentCalls{ 0 };
		std::atomic_bool g_loggedBuffer{ false };
		std::atomic_int g_pinchFrames{ 0 };
		// How many times each present asked for its buffer: 0 means DWM flipped
		// (or skipped the flip) without asking, so the pass could not draw.
		std::atomic_int g_presentsAsked0{ 0 };
		std::atomic_int g_presentsAsked1{ 0 };
		std::atomic_int g_presentsAskedMore{ 0 };

		void LogSummary(const char* when)
		{
			const auto stats{ LiquidGlassPass::GetStats() };
			ReconLog::Write("[stats] %s: presents=%d asked(0/1/2+)=%d/%d/%d passFrames=%d  bands rendered=%ld changed=%ld throttled=%ld gpu=%ld shifted=%ld %s",
				when, g_presentCalls.load(), g_presentsAsked0.load(), g_presentsAsked1.load(), g_presentsAskedMore.load(),
				g_pinchFrames.load(), stats.rendered, stats.published, stats.throttled, stats.shared, stats.shifted, GlassScene::LatencySummary().c_str());
		}

		// While no band is live (every window still, nothing around them
		// changing): how many presents there are, how many of them hand us the
		// frame, and what the pass makes of them. Logged every 2 s while there
		// are any. Render thread only.
		struct StillCounts
		{
			long presents{ 0 };
			long asked0{ 0 };
			LiquidGlassPass::Stats base{};
			ULONGLONG since{ 0 };
		};
		StillCounts g_still;

		void LogStill(int asked)
		{
			if (g_disabled.load(std::memory_order_relaxed) || !GlassScene::IsEnabled())
				return;
			if (!LiquidGlassPass::AnyLive())
			{
				++g_still.presents;
				if (asked == 0)
					++g_still.asked0;
			}
			const ULONGLONG now{ GetTickCount64() };
			if (!g_still.since)
			{
				g_still.since = now;
				g_still.base = LiquidGlassPass::GetStats();
				return;
			}
			if (now - g_still.since < 2000)
				return;
			const auto stats{ LiquidGlassPass::GetStats() };
			if (g_still.presents)
			{
				ReconLog::Write("[still] 2s: presents=%ld asked0=%ld checked=%ld readbacks=%ld changes=%ld shown=%ld live=%ld",
					g_still.presents, g_still.asked0,
					stats.throttled - g_still.base.throttled, stats.drained - g_still.base.drained,
					stats.hashChanges - g_still.base.hashChanges, stats.published - g_still.base.published,
					stats.shared - g_still.base.shared);
			}
			g_still = StillCounts{};
			g_still.since = now;
			g_still.base = stats;
		}

		// ---- fuse -----------------------------------------------------------
		// File-based rather than registry-based: dwm.exe runs as the DWM-N
		// virtual account, so its HKCU is not the user hive and is easy to get
		// wrong. ProgramData is already proven writable from this process.

		DWORD ReadFuse()
		{
			HANDLE f = CreateFileW(kFusePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE)
				return 0;
			DWORD value = 0, read = 0;
			ReadFile(f, &value, sizeof(value), &read, nullptr);
			CloseHandle(f);
			return read == sizeof(value) ? value : 0;
		}

		void WriteFuse(DWORD value)
		{
			CreateDirectoryW(kReconDir, nullptr);
			HANDLE f = CreateFileW(kFusePath, GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE)
				return;
			DWORD written = 0;
			WriteFile(f, &value, sizeof(value), &written, nullptr);
			CloseHandle(f);
		}

		DWORD WINAPI FuseProc(LPVOID)
		{
			// Still alive after a few seconds: this arm was clean, so clear the
			// counter and give a future crash its own budget.
			if (WaitForSingleObject(g_fuseStop, kCleanRunMs) == WAIT_TIMEOUT)
			{
				if (!g_disabled.load())
				{
					WriteFuse(0);
					ReconLog::Write("[fuse] clean run: counter cleared");
					LogSummary("5s");
					ReconLog::Flush();
				}
			}
			// A few more snapshots over the first minute, for diagnosing what
			// the pass did while someone was actually using the desktop.
			for (const char* when : { "30s", "60s" })
			{
				if (WaitForSingleObject(g_fuseStop, 27500) != WAIT_TIMEOUT)
					break;
				LogSummary(when);
				ReconLog::Flush();
			}
			return 0;
		}


		// ---- the hooks ------------------------------------------------------
		// Only functions with a unique address are hooked. dwmcore is linked
		// with COMDAT folding: CDeviceTextureTarget::GetRenderTargetView, for
		// instance, is one `mov rax,[rcx+10h]; ret` shared with
		// CD2DSharedBuffer::GetBufferPointer and CKernelTransport::GetWorkEvent,
		// so its "result" is not always a render target view. The two below
		// were each checked to stand alone.

		HRESULT STDMETHODCALLTYPE CDDisplayRenderTarget_Present(void* This);
		ID3D11Resource* STDMETHODCALLTYPE CDDisplaySwapChainBuffer_GetD3D11Resource(void* This);

		MinHook g_funPresent{ "CDDisplayRenderTarget::Present", CDDisplayRenderTarget_Present };
		MinHook g_funGetD3D11Resource{ "CDDisplaySwapChainBuffer::GetD3D11Resource", CDDisplaySwapChainBuffer_GetD3D11Resource };

		// Set on the render thread for the duration of CDDisplayRenderTarget::
		// Present. thread_local, so a GetD3D11Resource call from any other
		// thread can never start the pass on DWM's device context.
		thread_local bool t_inPresent{ false };
		thread_local int t_askedThisPresent{ 0 };
		// The first buffer a present asks for is the frame being presented.
		// DWM only redraws what changed, though, and copies the rest over from
		// the previous buffer (the second one it asks for) further into the
		// present - so at the first ask, anything that did not change THIS
		// frame is still three frames old in it (the swapchain has three).
		// Reading it there showed a still window's surroundings stale and
		// flickering between buffers. So the buffer is only noted here, and
		// read once the present has returned and the frame is complete.
		thread_local ID3D11Resource* t_frame{ nullptr };

		// Reads the presented frame - the bands go to Composition, never into
		// this buffer.
		bool RenderFrom(ID3D11Resource* resource)
		{
			ID3D11Texture2D* texture = nullptr;
			if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) || !texture)
				return false;

			D3D11_TEXTURE2D_DESC td{};
			texture->GetDesc(&td);
			if (!g_loggedBuffer.exchange(true))
			{
				ReconLog::Write("[present] frame %ux%u fmt=%d bind=0x%X (read after the present)",
					td.Width, td.Height, static_cast<int>(td.Format), td.BindFlags);
			}
			bool changed{ false };
			if (td.Width >= 640 && td.Height >= 480)
			{
				changed = LiquidGlassPass::RenderBands(texture, td.Width, td.Height);
				g_pinchFrames.fetch_add(1, std::memory_order_relaxed);
			}
			texture->Release();
			return changed;
		}

		bool GuardedRenderFrom(ID3D11Resource* resource)
		{
			__try
			{
				return RenderFrom(resource);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_disabled.store(true, std::memory_order_relaxed);
				ReconLog::Write("[pass] FAULTED - pinch disabled until DWMBlurGlass is reloaded");
				return false;
			}
		}

		ID3D11Resource* STDMETHODCALLTYPE CDDisplaySwapChainBuffer_GetD3D11Resource(void* This)
		{
			auto* result = g_funGetD3D11Resource.call_org(This);
			if (t_inPresent)
			{
				if (t_askedThisPresent == 0 && result && !t_frame)
				{
					// Held until the present returns.
					result->AddRef();
					t_frame = result;
				}
				++t_askedThisPresent;
			}
			return result;
		}

		HRESULT STDMETHODCALLTYPE CDDisplayRenderTarget_Present(void* This)
		{
			g_presentCalls.fetch_add(1, std::memory_order_relaxed);
			t_inPresent = true;
			t_askedThisPresent = 0;
			t_frame = nullptr;
			const HRESULT hr{ g_funPresent.call_org(This) };
			t_inPresent = false;
			if (t_askedThisPresent == 0)
				g_presentsAsked0.fetch_add(1, std::memory_order_relaxed);
			else if (t_askedThisPresent == 1)
				g_presentsAsked1.fetch_add(1, std::memory_order_relaxed);
			else
				g_presentsAskedMore.fetch_add(1, std::memory_order_relaxed);

			if (ID3D11Resource* frame{ std::exchange(t_frame, nullptr) })
			{
				// The frame is complete now. Hand any changed band to the UI
				// thread, which draws it into the window's Composition surface.
				// Posting never blocks.
				if (SUCCEEDED(hr) && !g_disabled.load(std::memory_order_relaxed) && GlassScene::IsEnabled() &&
					GuardedRenderFrom(frame))
					GlassScene::NotifyBandsChanged();
				frame->Release();
			}
			LogStill(t_askedThisPresent);
			return hr;
		}
	}

	void Attach()
	{
		if (g_startup.exchange(true))
			return;

		ReconLog::Start(kReconLog);
		ReconLog::Write("================================================================");
		ReconLog::Write("[attach] build=%u  rim=%d reach=%.1f", os::buildNumber,
			g_configData.glassRimThickness, g_configData.glassRefractionAmount);

		const DWORD fuse = ReadFuse();
		if (fuse >= kFuseLimit)
		{
			ReconLog::Write("[fuse] armCount=%u >= %u - refusing to arm. Delete %ls to reset.",
				fuse, kFuseLimit, kFusePath);
			ReconLog::Flush();
			g_disabled.store(true);
			return;
		}
		WriteFuse(fuse + 1);

		// Leaves first, the per-frame anchor last, so the anchor never runs
		// against half-attached state.
		const auto st1 = g_funGetD3D11Resource.Attach();
		const auto st2 = g_funPresent.Attach();
		ReconLog::Write("[attach] GetD3D11Resource=%d CDDisplayRenderTarget::Present=%d (0=MH_OK)",
			(int)st1, (int)st2);
		if (st1 != MH_OK || st2 != MH_OK)
			ReconLog::Write("[attach] a hook failed: the pinch cannot run on this build (symbols missing or changed)");
		ReconLog::Flush();

		g_fuseStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_fuseThread = CreateThread(nullptr, 0, FuseProc, nullptr, 0, nullptr);
	}

	void Detach()
	{
		if (!g_startup.exchange(false))
			return;

		if (g_fuseStop)
			SetEvent(g_fuseStop);
		if (g_fuseThread)
		{
			WaitForSingleObject(g_fuseThread, 3000);
			CloseHandle(g_fuseThread);
			g_fuseThread = nullptr;
		}
		if (g_fuseStop)
		{
			CloseHandle(g_fuseStop);
			g_fuseStop = nullptr;
		}

		// The anchor first, so no new pass starts while the rest come off.
		g_funPresent.Detach();
		g_funGetD3D11Resource.Detach();
		// Give a detour already in flight on the render thread a moment to
		// return before its state is torn down.
		Sleep(50);

		const bool clean = !g_disabled.load();
		ReconLog::Write("[detach] disabled=%d", g_disabled.load() ? 1 : 0);
		LogSummary("detach");
		LiquidGlassPass::Shutdown();
		GlassRects::Clear();
		ReconLog::Stop();

		// A clean detach is a clean run.
		if (clean)
			WriteFuse(0);

		g_disabled.store(false);
		g_presentCalls.store(0);
		g_pinchFrames.store(0);
		g_presentsAsked0.store(0);
		g_presentsAsked1.store(0);
		g_presentsAskedMore.store(0);
		g_loggedBuffer.store(false);
	}

	void Refresh()
	{
		GlassScene::Refresh();
		const bool want = GlassScene::IsEnabled();
		if (want && !g_startup)
			Attach();
		else if (!want && g_startup)
			Detach();
	}
}
