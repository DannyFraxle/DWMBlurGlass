/**
 * FileName: GlassScene.cpp
 *
 * See GlassScene.h.
*/
#include "GlassScene.h"
#include "GlassRectRegistry.h"
#include "PinchBands.h"
#include "DWMStruct.h"
#include "CommonDef.h"
#include "wil.h"
#include "../Helper/ReconLog.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <mutex>

namespace MDWMBlurGlassExt::GlassScene
{
	using namespace MDWMBlurGlass;
	using namespace CommonDef;
	using namespace DWM;
	namespace wuc = winrt::Windows::UI::Composition;

	namespace
	{
		// The band is part of the window, so it can never be drawn in the wrong
		// place. While it moves, the band is re-rendered every frame and handed
		// over on the GPU, so what it shows is at most a frame old; out-of-date
		// readback bands are skipped (see Matches). (`changedAt` is still published, as `settledAt`: the render
		// thread renders every frame for a while after a change, and only
		// slows down once the window has been still.)
		constexpr ULONGLONG kQuietMs = 0;
		// How long DWM is kept presenting after a change, so the render thread
		// gets frames to finish the band from once the window stops.
		constexpr ULONGLONG kKickMs = 700;

		constexpr wchar_t kNoPinchMarker[] = L"C:\\ProgramData\\DWMBlurGlassRecon\\no_pinch";
		constexpr UINT kUploadMessage = WM_APP + 0x4C47;   // "LG"

		struct Glass
		{
			const void* owner;
			HWND hwnd;
			RECT rect;
			uint8_t edges;
			bool active;
			ULONGLONG changedAt;
			wuc::Visual kick{ nullptr };
			wuc::CompositionSurfaceBrush pinch{ nullptr };
			POINT bandOffset;
			wuc::CompositionDrawingSurface surface{ nullptr };
			SIZE surfaceSize;
			uint64_t uploaded;
			// Where the band now in `surface` was rendered for, and whether it
			// is showing. A band only fits the window position it was rendered
			// at: once the window moves on, it would carry its old surroundings
			// along with it.
			RECT shownFor;
			bool shown;
			bool retry;   // a readback band was skipped as out of date; look again

			// The render thread's shared band textures, opened on our device.
			// It alternates between two, so two are kept.
			struct SharedView
			{
				HANDLE handle{ nullptr };
				winrt::com_ptr<ID3D11Texture2D> texture;
				winrt::com_ptr<IDXGIKeyedMutex> mutex;
				winrt::com_ptr<ID2D1Bitmap1> bitmap;
				uint64_t lastUse{ 0 };
			};
			SharedView views[2];
		};

		// Keyed-mutex keys of a shared band texture (as in LiquidGlassPass).
		constexpr UINT64 kKeyFree = 0;
		constexpr UINT64 kKeyRendered = 1;

		// The D3D device behind our Composition graphics device, which shared
		// textures are opened on. Found from the first surface drawn into.
		winrt::com_ptr<ID3D11Device> g_uiDevice;
		winrt::com_ptr<ID2D1DeviceContext> g_checkDc;
		uint64_t g_viewClock{ 0 };

		// How long bands take from being rendered to being drawn into their
		// window, per path - for the log.
		struct Latency
		{
			std::atomic<uint64_t> count{ 0 };
			std::atomic<uint64_t> sumUs{ 0 };
			std::atomic<uint64_t> maxUs{ 0 };
		};
		Latency g_latencyGpu;
		Latency g_latencyReadback;

		// What the UI thread does with each upload notification, step by step -
		// for the log, so a band that never shows says where it stopped.
		// UI thread only.
		struct Diag
		{
			uint64_t calls{ 0 };        // UploadPending ran
			uint64_t lockMiss{ 0 };     // our lock was busy
			uint64_t noBrush{ 0 };      // a glass without a pinch brush
			uint64_t taken{ 0 };        // a newer band was taken
			uint64_t staleSkip{ 0 };    // readback band for an old position
			uint64_t acquireFail{ 0 };  // shared texture held by the render thread
			uint64_t versionSkip{ 0 };  // a newer band appeared meanwhile
			uint64_t sharedDrawn{ 0 };
			uint64_t pixelsDrawn{ 0 };
			uint64_t failed{ 0 };       // threw
			uint64_t commits{ 0 };      // interop device commits after drawing
			uint64_t commitFailed{ 0 };
			ULONGLONG loggedAt{ 0 };
		};
		Diag g_diag;
		const char* g_step{ "start" };   // last step a band draw reached, for the log
		int g_sharedFailStreak{ 0 };

		void LogDiag(size_t glassCount)
		{
			const ULONGLONG now{ GetTickCount64() };
			if (now - g_diag.loggedAt < 2000)
				return;
			g_diag.loggedAt = now;
			ReconLog::Write("[scene] uploads: calls=%llu lockMiss=%llu noBrush=%llu taken=%llu stale=%llu acquireFail=%llu versionSkip=%llu drawn gpu=%llu pixels=%llu failed=%llu commits=%llu/%llu glass=%zu",
				g_diag.calls, g_diag.lockMiss, g_diag.noBrush, g_diag.taken, g_diag.staleSkip, g_diag.acquireFail,
				g_diag.versionSkip, g_diag.sharedDrawn, g_diag.pixelsDrawn, g_diag.failed, g_diag.commits, g_diag.commitFailed, glassCount);
		}

		uint64_t Now()
		{
			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);
			return static_cast<uint64_t>(counter.QuadPart);
		}

		void Record(Latency& latency, uint64_t renderedAt)
		{
			if (!renderedAt)
				return;
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			const uint64_t now{ Now() };
			if (now <= renderedAt || !frequency.QuadPart)
				return;
			const uint64_t us{ (now - renderedAt) * 1000000ull / static_cast<uint64_t>(frequency.QuadPart) };
			latency.count.fetch_add(1);
			latency.sumUs.fetch_add(us);
			uint64_t max{ latency.maxUs.load() };
			while (us > max && !latency.maxUs.compare_exchange_weak(max, us)) {}
		}

		// How far (px) a band read back to the CPU may be from where its window
		// now is and still be drawn. Readback bands are a frame or two old, so
		// one rendered before a drag must not replace the live GPU band. (GPU
		// bands are drawn within a frame of being rendered and always shown -
		// the band on show is never taken down, since hiding and re-showing it
		// as the window moves is what made it flicker.)
		constexpr LONG kStaleSlackPx = 3;

		bool Matches(const RECT& rendered, const RECT& now)
		{
			return std::abs(rendered.left - now.left) <= kStaleSlackPx &&
				std::abs(rendered.top - now.top) <= kStaleSlackPx;
		}

		std::mutex g_lock;
		std::vector<Glass> g_glass;
		bool g_enabled{ false };

		wuc::CompositionGraphicsDevice g_graphicsDevice{ nullptr };

		// Set while an upload message is in flight, so the render thread posts
		// at most one at a time.
		std::atomic_bool g_uploadPosted{ false };

		// Set by the render thread (Nudge): kick every glass briefly so DWM
		// presents a few more frames and a waiting readback gets collected.
		std::atomic_bool g_nudgeRequested{ false };
		constexpr ULONGLONG kNudgeMs = 60;

		bool SameRect(const RECT& a, const RECT& b)
		{
			return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
		}

		// Keeps DWM producing frames while a band settles. Without it a still
		// desktop presents nothing and the render thread never sees the
		// settled frame. The opacity change is far below anything visible.
		void Kick(const wuc::Visual& visual, ULONGLONG ms = kKickMs) try
		{
			if (!visual)
				return;
			auto compositor{ visual.Compositor() };
			auto animation{ compositor.CreateScalarKeyFrameAnimation() };
			animation.InsertKeyFrame(0.f, 0.998f);
			animation.InsertKeyFrame(1.f, 1.f);
			animation.Duration(std::chrono::milliseconds{ ms });
			visual.StartAnimation(L"Opacity", animation);
		}
		catch (...)
		{
		}

		void EnsureGraphicsDevice(const wuc::Compositor& compositor)
		{
			if (g_graphicsDevice)
				return;
			wuc::CompositionGraphicsDevice device{ nullptr };
			THROW_IF_FAILED(
				compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>()->CreateGraphicsDevice(
					CDesktopManager::s_pDesktopManagerInstance->GetD2DDevice(),
					reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(device))
				)
			);
			g_graphicsDevice = device;
		}

		// The surface covers the backdrop visual from its origin to the far
		// corner of the titlebar, so with the brush aligned top-left the band
		// lands exactly on the titlebar - no brush transform needed.
		void EnsureSurface(Glass& glass, UINT width, UINT height)
		{
			g_step = "graphics device";
			EnsureGraphicsDevice(glass.pinch.Compositor());
			const SIZE size{ glass.bandOffset.x + static_cast<LONG>(width), glass.bandOffset.y + static_cast<LONG>(height) };
			if (!glass.surface)
			{
				g_step = "create surface";
				glass.surface = g_graphicsDevice.CreateDrawingSurface(
					{ static_cast<float>(size.cx), static_cast<float>(size.cy) },
					winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
					winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
				glass.surfaceSize = size;
			}
			else if (glass.surfaceSize.cx != size.cx || glass.surfaceSize.cy != size.cy)
			{
				g_step = "resize surface";
				glass.surface.Resize({ size.cx, size.cy });
				glass.surfaceSize = size;
			}
		}

		winrt::com_ptr<ID3D11Device> DeviceOf(ID2D1DeviceContext* dc)
		{
			winrt::com_ptr<ID2D1Image> target;
			dc->GetTarget(target.put());
			auto bitmap{ target.try_as<ID2D1Bitmap1>() };
			if (!bitmap)
				return nullptr;
			winrt::com_ptr<IDXGISurface> surface;
			if (FAILED(bitmap->GetSurface(surface.put())) || !surface)
				return nullptr;
			winrt::com_ptr<ID3D11Device> device;
			if (FAILED(surface->GetDevice(IID_PPV_ARGS(device.put()))))
				return nullptr;
			return device;
		}

		// Finds our D3D device the first time it is needed (once per load), by
		// opening the whole surface and clearing it; the band is drawn into it
		// straight after. Not a single pixel: on a surface that is new or just
		// resized, Composition rejects any BeginDraw that does not cover all of
		// it (E_INVALIDARG) - which, with the GPU path drawing a window's first
		// band, left every band undrawn.
		bool EnsureUiDevice(Glass& glass)
		{
			if (g_uiDevice)
				return true;
			auto interop{ glass.surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>() };
			winrt::com_ptr<ID2D1DeviceContext> dc;
			POINT offset{};
			g_step = "BeginDraw (find device)";
			THROW_IF_FAILED(interop->BeginDraw(nullptr, IID_PPV_ARGS(dc.put()), &offset));
			auto endDraw{ wil::scope_exit([&] { interop->EndDraw(); }) };
			dc->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
			g_uiDevice = DeviceOf(dc.get());
			// A D2D context of our own on the same device, for making bitmaps
			// of the shared textures and checking them outside any BeginDraw.
			winrt::com_ptr<ID2D1Device> d2d;
			dc->GetDevice(d2d.put());
			if (!d2d || FAILED(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, g_checkDc.put())))
				g_uiDevice = nullptr;
			return g_uiDevice != nullptr;
		}

		Glass::SharedView* OpenShared(Glass& glass, HANDLE handle)
		{
			++g_viewClock;
			for (auto& view : glass.views)
			{
				if (view.handle == handle && view.texture)
				{
					view.lastUse = g_viewClock;
					return &view;
				}
			}
			auto* view{ &glass.views[0] };
			if (glass.views[1].lastUse < view->lastUse)
				view = &glass.views[1];
			*view = Glass::SharedView{};

			winrt::com_ptr<ID3D11Texture2D> texture;
			if (FAILED(g_uiDevice->OpenSharedResource(handle, IID_PPV_ARGS(texture.put()))) || !texture)
				return nullptr;
			auto mutex{ texture.try_as<IDXGIKeyedMutex>() };
			if (!mutex)
				return nullptr;
			view->handle = handle;
			view->texture = texture;
			view->mutex = mutex;
			view->lastUse = g_viewClock;
			return view;
		}

		void DrawInto(Glass& glass, UINT width, UINT height, const std::function<void(ID2D1DeviceContext*, const D2D1_RECT_F&)>& draw)
		{
			auto interop{ glass.surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>() };
			winrt::com_ptr<ID2D1DeviceContext> dc;
			POINT offset{};
			g_step = "BeginDraw";
			THROW_IF_FAILED(interop->BeginDraw(nullptr, IID_PPV_ARGS(dc.put()), &offset));
			bool ended{ false };
			auto endDraw{ wil::scope_exit([&] { if (!ended) interop->EndDraw(); }) };

			dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
			dc->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
			const auto left{ static_cast<float>(glass.bandOffset.x) };
			const auto top{ static_cast<float>(glass.bandOffset.y) };
			draw(dc.get(), D2D1::RectF(left, top, left + width, top + height));

			ended = true;
			const HRESULT hr{ interop->EndDraw() };
			if (FAILED(hr))
			{
				static bool logged{ false };
				if (!logged)
				{
					logged = true;
					ReconLog::Write("[scene] EndDraw failed: 0x%08X", static_cast<unsigned>(hr));
				}
				THROW_HR(hr);
			}
		}

		// Highest alpha in a band bitmap as the UI thread's device sees it, or
		// -1 if it cannot be read. Used on the first few GPU bands only, to
		// catch a shared texture that arrives empty.
		int MaxAlpha(ID2D1DeviceContext* dc, ID2D1Bitmap1* source, UINT width, UINT height)
		{
			winrt::com_ptr<ID2D1Bitmap1> readable;
			HRESULT hr{ dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
				D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
					D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
				readable.put()) };
			if (SUCCEEDED(hr))
				hr = readable->CopyFromBitmap(nullptr, source, nullptr);
			D2D1_MAPPED_RECT mapped{};
			if (SUCCEEDED(hr))
				hr = readable->Map(D2D1_MAP_OPTIONS_READ, &mapped);
			if (FAILED(hr))
			{
				ReconLog::Write("[scene] gpu band check could not read it: 0x%08X", static_cast<unsigned>(hr));
				return -1;
			}
			int max{ 0 };
			for (UINT y = 0; y < height; ++y)
			{
				const auto* row{ mapped.bits + static_cast<size_t>(y) * mapped.pitch };
				for (UINT x = 0; x < width; ++x)
					max = (std::max)(max, static_cast<int>(row[x * 4 + 3]));
			}
			readable->Unmap();
			return max;
		}

		// The first GPU bands are checked; if they all arrive empty, the
		// shared textures do not work in this DWM and every band is read back.
		constexpr int kChecks = 6;
		int g_checksLeft{ kChecks };
		int g_checksBlank{ 0 };

		void LogDevice(const char* who, ID3D11Device* device)
		{
			LUID luid{};
			winrt::com_ptr<IDXGIDevice> dxgi;
			if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgi.put()))))
			{
				winrt::com_ptr<IDXGIAdapter> adapter;
				if (SUCCEEDED(dxgi->GetAdapter(adapter.put())))
				{
					DXGI_ADAPTER_DESC desc{};
					adapter->GetDesc(&desc);
					luid = desc.AdapterLuid;
				}
			}
			ReconLog::Write("[scene] %s device %p adapter %08lX:%08lX flags 0x%X fl 0x%X", who, device,
				static_cast<unsigned long>(luid.HighPart), luid.LowPart, device->GetCreationFlags(),
				static_cast<unsigned>(device->GetFeatureLevel()));
		}

		void Show(Glass& glass)
		{
			if (glass.pinch.Surface() != glass.surface)
				glass.pinch.Surface(glass.surface);
			glass.shown = true;
		}

		// Draws a band that came back as pixels, the same way the material
		// texture is uploaded (BeginDraw / D2D bitmap / EndDraw).
		void UploadPixels(Glass& glass, const PinchBands::Band& band)
		{
			EnsureSurface(glass, band.width, band.height);
			DrawInto(glass, band.width, band.height, [&](ID2D1DeviceContext* dc, const D2D1_RECT_F& rect)
			{
				winrt::com_ptr<ID2D1Bitmap1> bitmap;
				g_step = "bitmap from pixels";
				THROW_IF_FAILED(dc->CreateBitmap(
					D2D1::SizeU(band.width, band.height),
					band.pixels.data(),
					band.width * 4,
					D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
						D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
					bitmap.put()));
				dc->DrawBitmap(bitmap.get(), rect);
			});
			Show(glass);
			Record(g_latencyReadback, band.renderedAt);
		}

		// Draws a band straight from the render thread's shared texture: no
		// readback, no CPU copy. Never waits - if the render thread holds the
		// texture right now, the band is skipped and a newer one follows.
		// Returns false when it could not be drawn.
		bool UploadShared(Glass& glass, const PinchBands::Band& band)
		{
			EnsureSurface(glass, band.width, band.height);
			if (!EnsureUiDevice(glass))
			{
				ReconLog::Write("[scene] no D3D device behind the surface; reading bands back instead");
				PinchBands::DisableShared();
				return false;
			}
			auto* view{ OpenShared(glass, band.shared) };
			if (!view)
			{
				ReconLog::Write("[scene] could not open a shared band texture; reading bands back instead");
				PinchBands::DisableShared();
				return false;
			}

			// Key 1: a band waiting for us. Key 0: one we drew already - still
			// what the slot describes, since the render thread only changes the
			// texture together with the description.
			if (view->mutex->AcquireSync(kKeyRendered, 0) != S_OK &&
				view->mutex->AcquireSync(kKeyFree, 0) != S_OK)
			{
				g_diag.acquireFail++;
				return false;
			}
			// Declared before the draw, so it is released after EndDraw has
			// submitted the work that reads it.
			auto release{ wil::scope_exit([&] { view->mutex->ReleaseSync(kKeyFree); }) };

			// A newer band has been published since we took this one: the
			// texture may hold it rather than the one described. Skip; the
			// newer one is on its way.
			if (PinchBands::Version(glass.owner) != glass.uploaded)
			{
				g_diag.versionSkip++;
				return false;
			}

			if (!view->bitmap)
			{
				auto surface{ view->texture.as<IDXGISurface>() };
				g_step = "bitmap from shared texture";
				THROW_IF_FAILED(g_checkDc->CreateBitmapFromDxgiSurface(surface.get(),
					D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
						D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
					view->bitmap.put()));
			}
			// Checked before drawing: BeginDraw clears the surface, so an empty
			// band would wipe the one on show.
			if (g_checksLeft > 0)
			{
				if (g_checksLeft == kChecks)
					LogDevice("ui", g_uiDevice.get());
				--g_checksLeft;
				const int alpha{ MaxAlpha(g_checkDc.get(), view->bitmap.get(), band.width, band.height) };
				ReconLog::Write("[scene] gpu band check: %ux%u max alpha %d", band.width, band.height, alpha);
				if (alpha == 0)
					++g_checksBlank;
				if (g_checksLeft == 0 && g_checksBlank == kChecks)
				{
					ReconLog::Write("[scene] gpu bands arrive empty; reading bands back instead");
					PinchBands::DisableShared();
				}
				if (alpha == 0)
					return false;
			}

			// Copied into a bitmap of our own while we hold the texture, and that
			// copy is what gets drawn. Drawn from the shared texture itself the
			// band never showed: DWM evidently carries out surface drawing later
			// than EndDraw, by when the texture is no longer ours to read (its
			// contents checked out fine here, yet nothing reached the screen).
			// A plain bitmap, made fresh each time, is what the readback path
			// draws too, and that has always shown.
			winrt::com_ptr<ID2D1Bitmap1> copy;
			g_step = "create copy bitmap";
			THROW_IF_FAILED(g_checkDc->CreateBitmap(D2D1::SizeU(band.width, band.height), nullptr, 0,
				D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
					D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
				copy.put()));
			g_step = "copy from shared bitmap";
			THROW_IF_FAILED(copy->CopyFromBitmap(nullptr, view->bitmap.get(), nullptr));

			DrawInto(glass, band.width, band.height, [&](ID2D1DeviceContext* dc, const D2D1_RECT_F& rect)
			{
				dc->DrawBitmap(copy.get(), rect);
			});
			Show(glass);
			Record(g_latencyGpu, band.renderedAt);
			return true;
		}

		void LogLatency()
		{
			const uint64_t total{ g_latencyGpu.count.load() + g_latencyReadback.count.load() };
			if (total && total % 300 == 0)
				ReconLog::Write("[scene] %s", LatencySummary().c_str());
		}

		thread_local bool t_uploading{ false };

		void UploadPending() try
		{
			// Never re-entered, and never waits on our own lock: if it is busy
			// (this thread or another is mid-update), the band stays pending and
			// the next notification or validation picks it up.
			if (t_uploading)
				return;
			t_uploading = true;
			auto done{ wil::scope_exit([] { t_uploading = false; }) };
			g_diag.calls++;

			// Visuals to nudge, kicked once both locks are let go: starting an
			// animation may call back into DWM, which may call back into us.
			std::vector<wuc::Visual> nudge;
			bool drew{ false };
			size_t glassCount{ 0 };
			{
				auto dwmLock{ wil::EnterCriticalSection(CDesktopManager::s_csDwmInstance) };
				std::unique_lock guard{ g_lock, std::try_to_lock };
				if (!guard.owns_lock())
				{
					g_diag.lockMiss++;
					return;
				}
				glassCount = g_glass.size();
				static PinchBands::Band band;   // UI thread only; keeps its pixel buffer
				for (auto& glass : g_glass)
				{
					if (!glass.pinch)
					{
						g_diag.noBrush++;
						continue;
					}
					if (!PinchBands::Take(glass.owner, glass.uploaded, band))
						continue;
					g_diag.taken++;
					// No position check here any more: a band's rect now follows
					// the window's live position (the render thread tracks it),
					// so it may differ from glass.rect, which only updates when
					// DWM validates the window. A window that moved is live, and
					// its readbacks are never published, so a readback band is
					// always from where the window still is.
					glass.retry = false;
					try
					{
						g_step = "start";
						if (band.shared)
						{
							if (UploadShared(glass, band))
							{
								g_diag.sharedDrawn++;
								drew = true;
								g_sharedFailStreak = 0;
								glass.shownFor = band.rect;
							}
						}
						else
						{
							UploadPixels(glass, band);
							g_diag.pixelsDrawn++;
							drew = true;
							glass.shownFor = band.rect;
						}
						LogLatency();
					}
					catch (...)
					{
						// Device lost or similar; the next band will try again.
						g_diag.failed++;
						const HRESULT hr{ wil::ResultFromCaughtException() };
						if (g_diag.failed <= 3)
						{
							ReconLog::Write("[scene] drawing a %s band failed at '%s': 0x%08X",
								band.shared ? "gpu" : "readback", g_step, static_cast<unsigned>(hr));
						}
						// The GPU path keeps failing: read every band back instead,
						// so the reflection at least shows.
						if (band.shared && ++g_sharedFailStreak >= 30 && PinchBands::SharedEnabled())
						{
							ReconLog::Write("[scene] gpu bands keep failing; reading bands back instead");
							PinchBands::DisableShared();
						}
					}
				}

				// The render thread left a readback waiting and wants a few more
				// presents to collect it.
				if (g_nudgeRequested.exchange(false))
				{
					for (auto& glass : g_glass)
					{
						if (glass.kick)
							nudge.push_back(glass.kick);
					}
				}
			}
			for (const auto& visual : nudge)
				Kick(visual, kNudgeMs);

			// Commit what we drew. The material lives on DWM's interop
			// DirectComposition device, and changes to it only reach the screen
			// when that device commits - which DWM does while validating a
			// window. A still window is not validated, so without this the band
			// drawn here sat waiting until something else (the mouse over the
			// window's edge, a move) made DWM validate it.
			if (drew || !nudge.empty())
			{
				auto dwmLock{ wil::EnterCriticalSection(CDesktopManager::s_csDwmInstance) };
				if (auto* device{ CDesktopManager::s_pDesktopManagerInstance->GetDCompositionInteropDevice() })
				{
					const HRESULT hr{ device->Commit() };
					if (FAILED(hr))
					{
						g_diag.commitFailed++;
						if (g_diag.commitFailed <= 3)
							ReconLog::Write("[scene] commit failed: 0x%08X", static_cast<unsigned>(hr));
					}
					else
						g_diag.commits++;
				}
			}
			LogDiag(glassCount);
		}
		catch (...)
		{
		}

		// The render thread wakes the UI thread through a hidden window of our
		// own, created on DWM's UI thread (the thread that pumps DWM's "Dwm"
		// window), so its messages are handled there. It only ever forwards to
		// DefWindowProcW - it is never part of anyone else's window procedure
		// chain. (Subclassing DWM's own window instead crash-looped dwm.exe: a
		// stale procedure left in its chain by a previous load, at the same
		// address, had the new one forwarding to itself until the stack ran out.)
		std::atomic<HWND> g_notifyWindow{ nullptr };
		bool g_loggedThread{ false };

		LRESULT CALLBACK NotifyWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
		{
			if (message == kUploadMessage)
			{
				g_uploadPosted.store(false);
				if (g_enabled)
					UploadPending();
				return 0;
			}
			return DefWindowProcW(hwnd, message, wParam, lParam);
		}

		// Only ever creates the window on DWM's UI thread; anywhere else it
		// would die with the thread, or never get its messages pumped.
		void EnsureNotifyWindow()
		{
			if (g_notifyWindow.load())
				return;
			const HWND dwmWindow{ FindWindowW(L"Dwm", nullptr) };
			const DWORD uiThread{ dwmWindow ? GetWindowThreadProcessId(dwmWindow, nullptr) : 0 };
			if (!uiThread || uiThread != GetCurrentThreadId())
			{
				if (!g_loggedThread)
				{
					g_loggedThread = true;
					ReconLog::Write("[scene] waiting for DWM's UI thread (ui=%lu here=%lu)", uiThread, GetCurrentThreadId());
				}
				return;
			}

			// A class name of its own per load: a class registered by an earlier
			// load of this DLL may still exist, pointing at its unloaded code.
			HMODULE module{ nullptr };
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&NotifyWindowProc), &module);
			wchar_t className[96]{};
			swprintf_s(className, L"DWMBlurGlass.LiquidGlass.%llx", static_cast<unsigned long long>(GetTickCount64()));
			WNDCLASSEXW wc{ sizeof(wc) };
			wc.lpfnWndProc = NotifyWindowProc;
			wc.hInstance = module;
			wc.lpszClassName = className;
			if (!RegisterClassExW(&wc))
				return;
			const HWND hwnd{ CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr) };
			if (hwnd)
			{
				g_notifyWindow.store(hwnd);
				ReconLog::Write("[scene] notify window ready on UI thread %lu", uiThread);
			}
		}

		// From any thread. DestroyWindow only works on the owning thread, so the
		// window is neutralised instead - its procedure swapped for
		// DefWindowProcW, which then closes it on its own thread - and nothing
		// is left pointing into this DLL once it unloads.
		void RetireNotifyWindow()
		{
			const HWND hwnd{ g_notifyWindow.exchange(nullptr) };
			if (!hwnd)
				return;
			SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DefWindowProcW));
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
			// A message already being dispatched to us gets a moment to return.
			Sleep(20);
		}

		void Publish(const std::vector<GlassRects::Entry>& entries)
		{
			GlassRects::Display display{};
			// One monitor only: every monitor has its own swapchain, and nothing
			// available here says which swapchain shows which part of the
			// virtual screen.
			display.valid = GetSystemMetrics(SM_CMONITORS) == 1;
			display.width = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
			display.height = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
			GlassRects::Publish(entries.data(), static_cast<int>(entries.size()), display);
		}
	}

	bool IsEnabled()
	{
		return g_enabled;
	}

	void Refresh()
	{
		const bool enabled =
			os::buildNumber >= 22000 &&
			g_configData.effectType == effectType::LiquidGlass &&
			g_configData.glassPinch &&
			GetFileAttributesW(kNoPinchMarker) == INVALID_FILE_ATTRIBUTES;

		{
			std::lock_guard guard{ g_lock };
			g_enabled = enabled;
			if (!enabled)
				g_glass.clear();
		}
		// The notify window is created lazily, from the UI thread (Flush).
		if (!enabled)
		{
			RetireNotifyWindow();
			GlassRects::Clear();
			PinchBands::Clear();
		}
	}

	void UpdateGlass(const void* owner, HWND hwnd, const RECT& screenRect, uint8_t edges, bool active,
		const wuc::Visual& kickVisual, const wuc::CompositionSurfaceBrush& pinchBrush, POINT bandOffset)
	{
		if (!g_enabled || !owner)
			return;

		bool kick{ false };
		{
			std::lock_guard guard{ g_lock };
			auto it{ std::find_if(g_glass.begin(), g_glass.end(), [&](const Glass& g) { return g.owner == owner; }) };
			if (it == g_glass.end())
			{
				Glass glass{};
				glass.owner = owner;
				glass.hwnd = hwnd;
				glass.rect = screenRect;
				glass.edges = edges;
				glass.active = active;
				glass.changedAt = GetTickCount64();
				glass.kick = kickVisual;
				glass.pinch = pinchBrush;
				glass.bandOffset = bandOffset;
				g_glass.push_back(std::move(glass));
				kick = true;
			}
			else
			{
				const bool resized
				{
					(it->rect.right - it->rect.left) != (screenRect.right - screenRect.left) ||
					(it->rect.bottom - it->rect.top) != (screenRect.bottom - screenRect.top) ||
					it->bandOffset.x != bandOffset.x || it->bandOffset.y != bandOffset.y ||
					it->edges != edges
				};
				if (resized || !SameRect(it->rect, screenRect) || it->hwnd != hwnd)
				{
					it->changedAt = GetTickCount64();
					kick = true;
				}
				// On a resize the old band stays up until the new one (a frame or
				// two later) replaces it - blanking it would flicker all through a
				// live resize.
				// The material is rebuilt on activation and colour changes; give
				// the new one the band straight away.
				if (it->pinch != pinchBrush)
				{
					it->pinch = pinchBrush;
					if (pinchBrush && it->surface && it->shown)
						pinchBrush.Surface(it->surface);
				}
				it->hwnd = hwnd;
				it->rect = screenRect;
				it->edges = edges;
				it->active = active;
				it->kick = kickVisual;
				it->bandOffset = bandOffset;
			}
		}
		if (kick)
			Kick(kickVisual);
	}

	void RemoveGlass(const void* owner)
	{
		bool removed{ false };
		{
			std::lock_guard guard{ g_lock };
			const auto before{ g_glass.size() };
			std::erase_if(g_glass, [&](const Glass& g) { return g.owner == owner; });
			removed = g_glass.size() != before;
		}
		if (removed)
		{
			PinchBands::Forget(owner);
			// Stop the render thread rendering it straight away.
			Flush();
		}
	}

	void Flush() try
	{
		if (!g_enabled)
			return;

		// Validations run on DWM's UI thread, so this is where the notify
		// window gets made (a no-op on any other thread, or once it exists).
		EnsureNotifyWindow();

		std::vector<GlassRects::Entry> entries;
		bool anyHidden{ false };
		{
			// Lock order is always DWM's lock first, then ours: the backdrop
			// refresh calls into UpdateGlass while already holding DWM's.
			auto dwmLock{ wil::EnterCriticalSection(CDesktopManager::s_csDwmInstance) };
			std::lock_guard guard{ g_lock };

			const LONG screenW{ GetSystemMetrics(SM_CXSCREEN) };
			const LONG screenH{ GetSystemMetrics(SM_CYSCREEN) };

			for (const auto& glass : g_glass)
			{
				if (!glass.pinch)
					continue;
				if (glass.retry)
					anyHidden = true;
				// An edge near the screen border keeps its rim: where the strip it
				// gathers runs off the screen, the sampler clamps, repeating the
				// last row of pixels there is. Only an edge AT the border goes -
				// the last row there would be the window itself.
				uint8_t edges{ glass.edges };
				if (glass.rect.left <= 0) edges &= ~GlassRects::LeftEdge;
				if (glass.rect.top <= 0) edges &= ~GlassRects::TopEdge;
				if (glass.rect.right >= screenW) edges &= ~GlassRects::RightEdge;
				if (glass.rect.bottom >= screenH) edges &= ~GlassRects::BottomEdge;
				if (!edges)
					continue;

				GlassRects::Entry entry{};
				entry.owner = glass.owner;
				entry.hwnd = glass.hwnd;
				entry.rect = glass.rect;
				entry.edges = edges;
				// Windows 11 rounds window corners by 8 DIPs, except when
				// maximised (and then every edge is at the screen border anyway).
				const UINT dpi{ GetDpiForWindow(glass.hwnd) };
				entry.cornerRadius = IsZoomed(glass.hwnd) ? 0.f : 8.f * static_cast<float>(dpi ? dpi : 96) / 96.f;
				entry.active = glass.active;
				entry.settledAt = glass.changedAt + kQuietMs;
				if (RECT windowRect{}; GetWindowRect(glass.hwnd, &windowRect))
				{
					entry.windowOrigin = { windowRect.left, windowRect.top };
					entry.hasWindowOrigin = true;
				}
				entries.push_back(entry);
			}
		}
		Publish(entries);
		LogDiag(entries.size());

		// A band hidden mid-drag may already fit the window's new position
		// without anything new arriving to say so.
		if (anyHidden)
			UploadPending();
	}
	catch (...)
	{
		// Never let this take the UI thread down; the pinch simply stops.
		GlassRects::Clear();
	}

	void Nudge()
	{
		g_nudgeRequested.store(true);
		NotifyBandsChanged();
	}

	void NotifyBandsChanged()
	{
		// One message in flight at a time; the handler takes every band that
		// is new by then.
		if (g_uploadPosted.exchange(true))
			return;
		const HWND hwnd{ g_notifyWindow.load() };
		if (!hwnd || !PostMessageW(hwnd, kUploadMessage, 0, 0))
			g_uploadPosted.store(false);
	}

	void Reset()
	{
		{
			std::lock_guard guard{ g_lock };
			g_glass.clear();
			g_enabled = false;
			g_graphicsDevice = nullptr;
			g_uiDevice = nullptr;
			g_checkDc = nullptr;
			g_checksLeft = kChecks;
			g_checksBlank = 0;
		}
		RetireNotifyWindow();
		GlassRects::Clear();
		PinchBands::Clear();
	}

	std::string LatencySummary()
	{
		auto line{ [](const char* name, const Latency& latency)
		{
			const uint64_t count{ latency.count.load() };
			char buffer[96]{};
			if (!count)
				sprintf_s(buffer, "%s=none", name);
			else
				sprintf_s(buffer, "%s=%llu avg %.1fms max %.1fms", name,
					static_cast<unsigned long long>(count),
					static_cast<double>(latency.sumUs.load()) / static_cast<double>(count) / 1000.0,
					static_cast<double>(latency.maxUs.load()) / 1000.0);
			return std::string{ buffer };
		} };
		return "band latency " + line("gpu", g_latencyGpu) + ", " + line("readback", g_latencyReadback);
	}
}
