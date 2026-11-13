/**
 * FileName: GlassScene.cpp
 *
 * See GlassScene.h.
*/
#include "GlassScene.h"
#include "GlassRectRegistry.h"
#include "DWMStruct.h"
#include "CommonDef.h"
#include "wil.h"
#include "../Helper/ReconLog.h"
#include <vector>
#include <algorithm>
#include <mutex>

namespace MDWMBlurGlassExt::GlassScene
{
	using namespace MDWMBlurGlass;
	using namespace CommonDef;
	using namespace DWM;

	namespace
	{
		// After the glass, or anything over it, moves, the rim stays off this
		// long: the frame on screen may not match the geometry published here
		// yet (DWM animates opening, restoring and maximising on its own), and
		// a rim drawn at last frame's position reads as a ghost outline.
		constexpr ULONGLONG kQuietMs = 350;
		// ...then fades in over this long (in LiquidGlassPass). The kick
		// animation below spans both, plus a margin.
		constexpr ULONGLONG kFadeMs = 250;

		constexpr wchar_t kNoPinchMarker[] = L"C:\\ProgramData\\DWMBlurGlassRecon\\no_pinch";

		struct Glass
		{
			const void* owner;
			HWND hwnd;
			RECT rect;
			uint8_t edges;
			bool active;
			ULONGLONG changedAt;
			RECT occluders[GlassRects::kMaxOccluders];
			int occluderCount;
			winrt::Windows::UI::Composition::Visual kick{ nullptr };
		};

		std::mutex g_lock;
		std::vector<Glass> g_glass;
		bool g_enabled{ false };

		// Which way DWM's window list runs. Settled once from where the shell's
		// taskbar (always on top) and the desktop (always at the bottom) sit.
		enum class Direction { Unknown, TopFirst, BottomFirst };
		Direction g_direction{ Direction::Unknown };
		bool g_loggedDirection{ false };

		bool SameRect(const RECT& a, const RECT& b)
		{
			return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
		}

		bool Intersects(const RECT& a, const RECT& b)
		{
			return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
		}

		// Keeps DWM producing frames while a rim settles and fades in. Without
		// it a still desktop presents nothing, PrePresent never runs, and the
		// rim would only appear on the next unrelated repaint. The opacity
		// change is far below anything visible.
		void Kick(const winrt::Windows::UI::Composition::Visual& visual) try
		{
			if (!visual)
				return;
			auto compositor{ visual.Compositor() };
			auto animation{ compositor.CreateScalarKeyFrameAnimation() };
			animation.InsertKeyFrame(0.f, 0.998f);
			animation.InsertKeyFrame(1.f, 1.f);
			animation.Duration(std::chrono::milliseconds{ kQuietMs + kFadeMs + 120 });
			visual.StartAnimation(L"Opacity", animation);
		}
		catch (...)
		{
		}

		// Shell surfaces that float over normal windows from their own bands
		// (Start, search, notification centre, task view, menus, tooltips).
		// Wherever they appear in the list, they always count as covering.
		bool IsOverlay(HWND hwnd)
		{
			if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
				return true;

			wchar_t name[64]{};
			if (!GetClassNameW(hwnd, name, static_cast<int>(std::size(name))))
				return false;
			static constexpr const wchar_t* overlays[]
			{
				L"Windows.UI.Core.CoreWindow",
				L"Shell_TrayWnd",
				L"Shell_SecondaryTrayWnd",
				L"XamlExplorerHostIslandWindow",
				L"MultitaskingViewFrame",
				L"ForegroundStaging",
				L"Xaml_WindowedPopupClass",
				L"TopLevelWindowForOverflowXamlIsland",
				L"#32768",
				L"tooltips_class32",
			};
			for (auto overlay : overlays)
			{
				if (!_wcsicmp(name, overlay))
					return true;
			}
			return false;
		}

		struct Window
		{
			HWND hwnd;
			RECT rect;
			bool overlay;
		};

		// The visible, uncloaked, unminimised windows of the current desktop,
		// in DWM's list order, plus where the taskbar and desktop sit in it.
		std::vector<Window> WalkWindows(int* trayIndex, int* desktopIndex)
		{
			std::vector<Window> windows;
			*trayIndex = -1;
			*desktopIndex = -1;

			// The caller holds CDesktopManager::s_csDwmInstance.
			ULONG_PTR desktopID{ 0 };
			GetDesktopID(1, &desktopID);
			auto list{ CDesktopManager::s_pDesktopManagerInstance->GetWindowList()->GetWindowListForDesktop(desktopID) };
			if (!list)
				return windows;

			int guard{ 0 };
			for (auto i{ list->Flink }; i != list && guard < 4096; i = i->Flink, ++guard)
			{
				auto data{ reinterpret_cast<CWindowData*>(i) };
				if (!data->IsWindowVisibleAndUncloaked())
					continue;
				const HWND hwnd{ data->GetHWND() };
				if (!hwnd || !IsWindow(hwnd))
					continue;

				wchar_t name[32]{};
				GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
				if (!_wcsicmp(name, L"Shell_TrayWnd"))
					*trayIndex = static_cast<int>(windows.size());
				else if (!_wcsicmp(name, L"Progman") || !_wcsicmp(name, L"WorkerW"))
				{
					if (*desktopIndex < 0)
						*desktopIndex = static_cast<int>(windows.size());
				}

				RECT rect{};
				auto window{ data->GetWindow() };
				if (window)
				{
					if (window->IsTrullyMinimized())
						continue;
					if (!window->GetActualWindowRect(&rect, false, true, true))
						continue;
				}
				else if (!GetWindowRect(hwnd, &rect))
				{
					continue;
				}
				if (IsRectEmpty(&rect))
					continue;

				windows.push_back({ hwnd, rect, IsOverlay(hwnd) });
			}
			return windows;
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
		if (!enabled)
			GlassRects::Clear();
	}

	void UpdateGlass(const void* owner, HWND hwnd, const RECT& screenRect, uint8_t edges,
		bool active, const winrt::Windows::UI::Composition::Visual& kickVisual)
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
				g_glass.push_back(std::move(glass));
				kick = true;
			}
			else
			{
				if (it->hwnd != hwnd || !SameRect(it->rect, screenRect) || it->edges != edges)
				{
					it->changedAt = GetTickCount64();
					kick = true;
				}
				it->hwnd = hwnd;
				it->rect = screenRect;
				it->edges = edges;
				it->active = active;
				it->kick = kickVisual;
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
		// Withdraw it from the render side straight away rather than on the
		// next validation, so no rim outlives its window.
		if (removed)
			Flush();
	}

	void Flush() try
	{
		if (!g_enabled)
			return;

		std::vector<GlassRects::Entry> entries;
		std::vector<winrt::Windows::UI::Composition::Visual> kicks;
		{
			// Lock order is always DWM's lock first, then ours: the backdrop
			// refresh calls into UpdateGlass while already holding DWM's, so
			// taking them the other way round here could deadlock.
			auto dwmLock{ wil::EnterCriticalSection(CDesktopManager::s_csDwmInstance) };
			std::lock_guard guard{ g_lock };
			if (g_glass.empty())
			{
				GlassRects::Clear();
				return;
			}

			int trayIndex{ -1 }, desktopIndex{ -1 };
			const auto windows{ WalkWindows(&trayIndex, &desktopIndex) };

			if (g_direction == Direction::Unknown && trayIndex >= 0 && desktopIndex >= 0)
			{
				g_direction = trayIndex < desktopIndex ? Direction::TopFirst : Direction::BottomFirst;
			}
			if (!g_loggedDirection)
			{
				g_loggedDirection = true;
				ReconLog::Write("[scene] window list: %zu windows, tray@%d desktop@%d => %s",
					windows.size(), trayIndex, desktopIndex,
					g_direction == Direction::TopFirst ? "top-first" :
					g_direction == Direction::BottomFirst ? "bottom-first" : "UNKNOWN (pinch off)");
			}
			// Without knowing which way is up there is no telling what covers
			// what, and a rim on top of the wrong window is worse than none.
			if (g_direction == Direction::Unknown)
			{
				GlassRects::Clear();
				return;
			}

			const UINT screenW{ static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN)) };
			const UINT screenH{ static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN)) };
			// How far outside an edge the pass samples; an edge closer than this
			// to the screen border has nothing to gather, so it gets no rim.
			const LONG reach{ static_cast<LONG>(g_configData.glassRefractionAmount * 1.25f) + 4 };

			for (auto& glass : g_glass)
			{
				// Where this window sits in the list; not there means not on
				// screen on this desktop, and so no rim.
				int self{ -1 };
				for (int i = 0; i < static_cast<int>(windows.size()); ++i)
				{
					if (windows[i].hwnd == glass.hwnd)
					{
						self = i;
						break;
					}
				}
				if (self < 0)
					continue;

				RECT occluders[GlassRects::kMaxOccluders]{};
				int occluderCount{ 0 };
				RECT spill{};   // everything past the cap, merged
				bool spilled{ false };
				for (int i = 0; i < static_cast<int>(windows.size()); ++i)
				{
					if (i == self)
						continue;
					const bool above{ g_direction == Direction::TopFirst ? i < self : i > self };
					if (!above && !windows[i].overlay)
						continue;
					RECT r{ windows[i].rect };
					InflateRect(&r, 2, 2);
					if (!Intersects(r, glass.rect))
						continue;
					if (occluderCount < GlassRects::kMaxOccluders - 1)
					{
						occluders[occluderCount++] = r;
					}
					else if (!spilled)
					{
						spill = r;
						spilled = true;
					}
					else
					{
						UnionRect(&spill, &spill, &r);
					}
				}
				if (spilled)
					occluders[occluderCount++] = spill;

				bool occludersChanged{ occluderCount != glass.occluderCount };
				for (int i = 0; !occludersChanged && i < occluderCount; ++i)
					occludersChanged = !SameRect(occluders[i], glass.occluders[i]);
				if (occludersChanged)
				{
					for (int i = 0; i < occluderCount; ++i)
						glass.occluders[i] = occluders[i];
					glass.occluderCount = occluderCount;
					glass.changedAt = GetTickCount64();
					kicks.push_back(glass.kick);
				}

				uint8_t edges{ glass.edges };
				if (glass.rect.left - reach < 0) edges &= ~GlassRects::LeftEdge;
				if (glass.rect.top - reach < 0) edges &= ~GlassRects::TopEdge;
				if (glass.rect.right + reach > static_cast<LONG>(screenW)) edges &= ~GlassRects::RightEdge;
				if (glass.rect.bottom + reach > static_cast<LONG>(screenH)) edges &= ~GlassRects::BottomEdge;
				if (!edges)
					continue;

				GlassRects::Entry entry{};
				entry.hwnd = glass.hwnd;
				entry.rect = glass.rect;
				entry.edges = edges;
				// Windows 11 rounds window corners by 8 DIPs, except when
				// maximised (and then every edge is at the screen border anyway).
				const UINT dpi{ GetDpiForWindow(glass.hwnd) };
				entry.cornerRadius = IsZoomed(glass.hwnd) ? 0.f : 8.f * static_cast<float>(dpi ? dpi : 96) / 96.f;
				entry.active = glass.active;
				entry.settledAt = glass.changedAt + kQuietMs;
				for (int i = 0; i < occluderCount; ++i)
					entry.occluders[i] = occluders[i];
				entry.occluderCount = occluderCount;
				entries.push_back(entry);
			}
		}

		Publish(entries);
		for (auto& visual : kicks)
			Kick(visual);
	}
	catch (...)
	{
		// Never let this take the UI thread down; the rim simply stops.
		GlassRects::Clear();
	}

	void Reset()
	{
		{
			std::lock_guard guard{ g_lock };
			g_glass.clear();
			g_enabled = false;
		}
		GlassRects::Clear();
	}
}
