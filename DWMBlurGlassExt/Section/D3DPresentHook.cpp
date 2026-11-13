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

		std::atomic_int g_prePresentCalls{ 0 };
		std::atomic_int g_pinchFrames{ 0 };

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
					ReconLog::Write("[fuse] clean run: counter cleared  prePresent=%d pinchFrames=%d",
						g_prePresentCalls.load(), g_pinchFrames.load());
					ReconLog::Flush();
				}
			}
			return 0;
		}

		// ---- which texture is this frame's? ----------------------------------
		// dwmcore is linked with COMDAT folding, so a one-line getter can share
		// its address with an unrelated getter of the same shape - which makes
		// "is this function in the object's vtable" useless as a type check, and
		// calling one on a guessed `this` a good way to crash-loop the desktop.
		// So these accessors are HOOKED: DWM calls them, and we keep what they
		// return.

		ID3D11RenderTargetView* STDMETHODCALLTYPE CDeviceTextureTarget_GetRenderTargetView(void* This);
		ID3D11Resource* STDMETHODCALLTYPE CDDisplaySwapChainBuffer_GetD3D11Resource(void* This);
		HRESULT STDMETHODCALLTYPE CDDisplaySwapChain_PrePresent(void* This, bool a2);
		HRESULT STDMETHODCALLTYPE CWindowNode_RenderContent(DWM::CWindowNode* This, void* drawingContext, bool* a3);

		MinHook g_funGetRenderTargetView{ "CDeviceTextureTarget::GetRenderTargetView", CDeviceTextureTarget_GetRenderTargetView };
		MinHook g_funGetD3D11Resource{ "CDDisplaySwapChainBuffer::GetD3D11Resource", CDDisplaySwapChainBuffer_GetD3D11Resource };
		MinHook g_funDDisplayPrePresent{ "CDDisplaySwapChain::PrePresent", CDDisplaySwapChain_PrePresent };
		MinHook g_funRenderContent{ "CWindowNode::RenderContent", CWindowNode_RenderContent };

		// Everything below is touched only on DWM's render thread.

		// Render targets seen this session, each classified once (classifying
		// means touching COM). DWM cycles through a handful.
		struct RTVInfo
		{
			ID3D11RenderTargetView* rtv;
			ID3D11Texture2D* texture;   // not addref'd; identity only
			UINT width;
			UINT height;
		};
		constexpr int kMaxRTVInfo = 64;
		RTVInfo g_rtvInfo[kMaxRTVInfo]{};
		int g_rtvInfoCount{ 0 };

		// The textures DWM actually hands to the display - the swapchain
		// buffers. The pass only ever draws into one of these, so it can never
		// end up writing into some large intermediate that happens to be
		// display-sized.
		constexpr int kMaxBuffers = 8;
		ID3D11Texture2D* g_buffers[kMaxBuffers]{};
		int g_bufferCount{ 0 };

		// The display-sized target DWM most recently composed into this frame.
		ID3D11Texture2D* g_frameTarget{ nullptr };
		UINT g_frameWidth{ 0 };
		UINT g_frameHeight{ 0 };

		// Windows DWM drew this frame.
		constexpr int kMaxRendered = 128;
		HWND g_rendered[kMaxRendered]{};
		int g_renderedCount{ 0 };

		bool IsSwapchainBuffer(ID3D11Texture2D* texture)
		{
			for (int i = 0; i < g_bufferCount; ++i)
			{
				if (g_buffers[i] == texture)
					return true;
			}
			return false;
		}

		void NoteRenderTarget(ID3D11RenderTargetView* rtv)
		{
			for (int i = 0; i < g_rtvInfoCount; ++i)
			{
				if (g_rtvInfo[i].rtv != rtv)
					continue;
				if (g_rtvInfo[i].texture && g_rtvInfo[i].width >= 640 && g_rtvInfo[i].height >= 480)
				{
					g_frameTarget = g_rtvInfo[i].texture;
					g_frameWidth = g_rtvInfo[i].width;
					g_frameHeight = g_rtvInfo[i].height;
				}
				return;
			}

			RTVInfo info{ rtv, nullptr, 0, 0 };
			ID3D11Resource* resource = nullptr;
			rtv->GetResource(&resource);
			if (resource)
			{
				ID3D11Texture2D* texture = nullptr;
				if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture)
				{
					D3D11_TEXTURE2D_DESC td{};
					texture->GetDesc(&td);
					info.texture = texture;
					info.width = td.Width;
					info.height = td.Height;
					texture->Release();   // DWM owns it; we only note the pointer
				}
				resource->Release();
			}

			// DWM recreates targets now and then; when the table fills, start
			// over rather than stop recognising new ones.
			if (g_rtvInfoCount >= kMaxRTVInfo)
				g_rtvInfoCount = 0;
			g_rtvInfo[g_rtvInfoCount++] = info;
			if (info.texture && info.width >= 640 && info.height >= 480)
			{
				g_frameTarget = info.texture;
				g_frameWidth = info.width;
				g_frameHeight = info.height;
			}
		}

		void GuardedNoteRenderTarget(ID3D11RenderTargetView* rtv)
		{
			__try
			{
				NoteRenderTarget(rtv);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_disabled.store(true, std::memory_order_relaxed);
			}
		}

		ID3D11RenderTargetView* STDMETHODCALLTYPE CDeviceTextureTarget_GetRenderTargetView(void* This)
		{
			auto* result = g_funGetRenderTargetView.call_org(This);
			if (result && !g_disabled.load(std::memory_order_relaxed))
				GuardedNoteRenderTarget(result);
			return result;
		}

		void NoteSwapchainBuffer(ID3D11Resource* resource)
		{
			ID3D11Texture2D* texture = nullptr;
			if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) || !texture)
				return;
			texture->Release();   // identity only
			if (IsSwapchainBuffer(texture))
				return;
			if (g_bufferCount >= kMaxBuffers)
				g_bufferCount = 0;
			g_buffers[g_bufferCount++] = texture;

			D3D11_TEXTURE2D_DESC td{};
			texture->GetDesc(&td);
			ReconLog::Write("[swapbuf] tex=%p %ux%u fmt=%d bind=0x%X", texture, td.Width, td.Height,
				static_cast<int>(td.Format), td.BindFlags);
		}

		void GuardedNoteSwapchainBuffer(ID3D11Resource* resource)
		{
			__try
			{
				NoteSwapchainBuffer(resource);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_disabled.store(true, std::memory_order_relaxed);
			}
		}

		// Asked for during the present submission - after PrePresent - so it
		// only ever teaches us which textures ARE swapchain buffers; the
		// render-target hook says which one this frame is going into.
		ID3D11Resource* STDMETHODCALLTYPE CDDisplaySwapChainBuffer_GetD3D11Resource(void* This)
		{
			auto* result = g_funGetD3D11Resource.call_org(This);
			if (result && !g_disabled.load(std::memory_order_relaxed))
				GuardedNoteSwapchainBuffer(result);
			return result;
		}

		void NoteRendered(DWM::CWindowNode* node)
		{
			const HWND hwnd{ node->GetHwnd() };
			if (!hwnd)
				return;
			for (int i = 0; i < g_renderedCount; ++i)
			{
				if (g_rendered[i] == hwnd)
					return;
			}
			if (g_renderedCount < kMaxRendered)
				g_rendered[g_renderedCount++] = hwnd;
		}

		void GuardedNoteRendered(DWM::CWindowNode* node)
		{
			__try
			{
				NoteRendered(node);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_disabled.store(true, std::memory_order_relaxed);
			}
		}

		HRESULT STDMETHODCALLTYPE CWindowNode_RenderContent(DWM::CWindowNode* This, void* drawingContext, bool* a3)
		{
			if (!g_disabled.load(std::memory_order_relaxed))
				GuardedNoteRendered(This);
			return g_funRenderContent.call_org(This, drawingContext, a3);
		}

		void Pinch()
		{
			ID3D11Texture2D* target{ g_frameTarget };
			if (target && IsSwapchainBuffer(target))
			{
				LiquidGlassPass::RenderPinch(target, g_frameWidth, g_frameHeight, g_rendered, g_renderedCount);
				g_pinchFrames.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void GuardedPinch()
		{
			__try
			{
				Pinch();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_disabled.store(true, std::memory_order_relaxed);
				ReconLog::Write("[pass] FAULTED - pinch disabled until DWMBlurGlass is reloaded");
			}
		}

		HRESULT STDMETHODCALLTYPE CDDisplaySwapChain_PrePresent(void* This, bool a2)
		{
			g_prePresentCalls.fetch_add(1, std::memory_order_relaxed);
			// Composition is finished and the present has not gone out: the
			// one moment the whole frame is final and still writable.
			if (!g_disabled.load(std::memory_order_relaxed) && GlassScene::IsEnabled())
				GuardedPinch();

			g_renderedCount = 0;
			g_frameTarget = nullptr;
			return g_funDDisplayPrePresent.call_org(This, a2);
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

		const auto st1 = g_funGetRenderTargetView.Attach();
		const auto st2 = g_funGetD3D11Resource.Attach();
		const auto st3 = g_funRenderContent.Attach();
		const auto st4 = g_funDDisplayPrePresent.Attach();
		ReconLog::Write("[attach] GetRTV=%d GetD3D11Resource=%d RenderContent=%d PrePresent=%d (0=MH_OK)",
			(int)st1, (int)st2, (int)st3, (int)st4);
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

		// PrePresent first, so no new pass starts while the rest come off.
		g_funDDisplayPrePresent.Detach();
		g_funRenderContent.Detach();
		g_funGetD3D11Resource.Detach();
		g_funGetRenderTargetView.Detach();
		// Give a detour already in flight on the render thread a moment to
		// return before its state is torn down.
		Sleep(50);

		g_rtvInfoCount = 0;
		g_bufferCount = 0;
		g_renderedCount = 0;
		g_frameTarget = nullptr;
		LiquidGlassPass::Shutdown();
		GlassRects::Clear();

		const bool clean = !g_disabled.load();
		ReconLog::Write("[detach] disabled=%d prePresent=%d pinchFrames=%d",
			g_disabled.load() ? 1 : 0, g_prePresentCalls.load(), g_pinchFrames.load());
		ReconLog::Stop();

		// A clean detach is a clean run.
		if (clean)
			WriteFuse(0);

		g_disabled.store(false);
		g_prePresentCalls.store(0);
		g_pinchFrames.store(0);
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
