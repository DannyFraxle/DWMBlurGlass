/**
 * FileName: RefractionRecon.cpp
 *
 * Reconnaissance-only. See RefractionRecon.h. Every detour here calls the
 * original unchanged; the probes are read-only and fault-guarded.
*/
#include "RefractionRecon.h"
#include "HookDef.h"
#include "CommonDef.h"
#include <cstdio>
#include <cstdarg>
#include <atomic>

namespace MDWMBlurGlassExt::RefractionRecon
{
	using namespace CommonDef;
	using namespace MDWMBlurGlass;

	std::atomic_bool g_startup = false;
	std::atomic_bool g_hookedExecuteBlur = false;
	std::atomic_bool g_hookedCustomBlurDraw = false;

	// Probe only the first few calls of each site, then go quiet: these run
	// once per window per frame and must not become a perf or disk problem.
	constexpr int kProbeCalls = 2;
	std::atomic_int g_nExecuteBlur{ 0 };
	std::atomic_int g_nCustomBlurDraw{ 0 };

	static constexpr wchar_t kLogDir[] = L"C:\\ProgramData\\DWMBlurGlassRecon";
	static constexpr wchar_t kLogPath[] = L"C:\\ProgramData\\DWMBlurGlassRecon\\recon.log";

	static void LogLine(const char* fmt, ...)
	{
		CreateDirectoryW(kLogDir, nullptr);
		FILE* f = nullptr;
		if (_wfopen_s(&f, kLogPath, L"a+") != 0 || !f)
			return;
		va_list ap;
		va_start(ap, fmt);
		vfprintf(f, fmt, ap);
		va_end(ap);
		fputc('\n', f);
		fclose(f);
	}

	// ---- read-only memory probes -------------------------------------------

	static bool IsReadable(const void* p, size_t cb)
	{
		if (!p) return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
		if (mbi.State != MEM_COMMIT) return false;
		if (mbi.Protect & PAGE_GUARD) return false;
		const DWORD prot = mbi.Protect & 0xFF;
		if (prot == PAGE_NOACCESS) return false;
		const BYTE* end = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
		return (static_cast<const BYTE*>(p) + cb) <= end;
	}

	// A plausible vtable pointer lives inside a mapped image (dwmcore/d2d1/...).
	static bool IsImagePtr(const void* p)
	{
		if (!p) return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
		return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE;
	}

	// POD-only bodies so SEH is legal here; never let a bad QI take down dwm.exe.
	static const char* ProbeIface(void* candidate)
	{
		IUnknown* unk = static_cast<IUnknown*>(candidate);
		struct Entry { const IID* iid; const char* name; };
		const Entry table[] = {
			{ &__uuidof(ID2D1Effect),        "ID2D1Effect" },
			{ &__uuidof(ID2D1Bitmap),        "ID2D1Bitmap" },
			{ &__uuidof(ID2D1CommandList),   "ID2D1CommandList" },
			{ &__uuidof(ID2D1Image),         "ID2D1Image" },
			{ &__uuidof(ID2D1DeviceContext), "ID2D1DeviceContext" },
		};
		for (int i = 0; i < ARRAYSIZE(table); ++i)
		{
			void* out = nullptr;
			HRESULT hr = E_FAIL;
			__try { hr = unk->QueryInterface(*table[i].iid, &out); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return "(QI faulted)"; }
			if (SUCCEEDED(hr) && out)
			{
				__try { static_cast<IUnknown*>(out)->Release(); }
				__except (EXCEPTION_EXECUTE_HANDLER) {}
				return table[i].name;
			}
		}
		return nullptr;
	}

	// Name the module owning an address (which DLL does this vtable live in?).
	static void ModuleOf(const void* addr, char* out, size_t cb, uintptr_t* offset)
	{
		HMODULE h{};
		*offset = 0;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(addr), &h) && h)
		{
			wchar_t path[MAX_PATH]{};
			if (GetModuleFileNameW(h, path, MAX_PATH))
			{
				const wchar_t* name = wcsrchr(path, L'\\');
				name = name ? name + 1 : path;
				WideCharToMultiByte(CP_UTF8, 0, name, -1, out, static_cast<int>(cb), nullptr, nullptr);
				*offset = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(h);
				return;
			}
		}
		strcpy_s(out, cb, "?");
	}

	// Walk the first N pointer slots of an opaque DWM struct looking for
	// anything that behaves like a D2D COM object. depth>0 recurses into
	// COM-like children, since the real ID2D1Image may be a member.
	static void ProbeObject(const char* label, const void* obj, int slots = 24, int depth = 1)
	{
		if (!IsReadable(obj, sizeof(void*) * static_cast<size_t>(slots)))
		{
			LogLine("    %s @ %p : not readable", label, obj);
			return;
		}
		LogLine("    %s @ %p", label, obj);
		void* const* p = static_cast<void* const*>(obj);
		for (int i = 0; i < slots; ++i)
		{
			void* v = p[i];
			if (!v || !IsReadable(v, sizeof(void*)))
				continue;
			void* vtable = *static_cast<void**>(v);
			if (!IsImagePtr(vtable))
				continue;
			const char* iface = ProbeIface(v);
			char mod[64]{}; uintptr_t off = 0;
			ModuleOf(vtable, mod, sizeof(mod), &off);
			LogLine("      [%2d] %p  vt=%s+0x%llX  %s", i, v, mod, (unsigned long long)off,
				iface ? iface : "(com-like, no known D2D iface)");
			if (depth > 0)
			{
				char childLabel[96]{};
				sprintf_s(childLabel, "child of %s[%d]", label, i);
				ProbeObject(childLabel, v, 24, depth - 1);
			}
		}
	}

	// ---- hooks --------------------------------------------------------------

	MinHook g_funExecuteBlur{ "CRenderingTechnique::ExecuteBlur", CRenderingTechnique_ExecuteBlur };
	MinHook g_funExecuteBlur24h2{ "CRenderingTechnique::ExecuteBlur", CRenderingTechnique_ExecuteBlur24h2 };
	MinHook g_funCCustomBlur_Draw{ "CCustomBlur::Draw", CCustomBlur_Draw };

	static void ProbeExecuteBlur(const char* which, const DWM::EffectInput* input,
		const D2D_VECTOR_2F* blurAmount, const D2D_SIZE_F* size, DWM::EffectInput* output)
	{
		const int n = g_nExecuteBlur.fetch_add(1);
		if (n >= kProbeCalls) return;
		LogLine("[%s] call #%d", which, n);
		if (blurAmount && IsReadable(blurAmount, sizeof(*blurAmount)))
			LogLine("    blurAmount = (%.3f, %.3f)", blurAmount->x, blurAmount->y);
		if (size && IsReadable(size, sizeof(*size)))
			LogLine("    size       = (%.1f x %.1f)", size->width, size->height);
		ProbeObject("input  EffectInput", input);
		ProbeObject("output EffectInput", output);
		if (n == kProbeCalls - 1)
			LogLine("[%s] probe budget spent; going quiet", which);
	}

	DWORD64 STDMETHODCALLTYPE CRenderingTechnique_ExecuteBlur(DWM::CRenderingTechnique* This,
		DWM::CDrawingContext* drawingContext, const DWM::EffectInput* input,
		const D2D_VECTOR_2F* blurAmount, DWM::EffectInput* output)
	{
		try { ProbeExecuteBlur("ExecuteBlur", input, blurAmount, nullptr, output); }
		catch (...) {}
		return g_funExecuteBlur.call_org(This, drawingContext, input, blurAmount, output);
	}

	DWORD64 STDMETHODCALLTYPE CRenderingTechnique_ExecuteBlur24h2(DWM::CRenderingTechnique* This,
		DWM::CDrawingContext* drawingContext, const DWM::EffectInput* input,
		const D2D_VECTOR_2F* blurAmount, const D2D_SIZE_F* size, DWM::EffectInput* output)
	{
		try { ProbeExecuteBlur("ExecuteBlur24h2", input, blurAmount, size, output); }
		catch (...) {}
		return g_funExecuteBlur24h2.call_org(This, drawingContext, input, blurAmount, size, output);
	}

	HRESULT STDMETHODCALLTYPE CCustomBlur_Draw(DWM::Core::CCustomBlur* This,
		DWM::Core::CDrawingContext* drawingContext, const D2D1_RECT_F& destinationRect,
		const D2D1_POINT_2F* point, D2D1_INTERPOLATION_MODE interpolationMode,
		D2D1_COMPOSITE_MODE compositeMode)
	{
		try
		{
			const int n = g_nCustomBlurDraw.fetch_add(1);
			if (n < kProbeCalls)
			{
				LogLine("[CCustomBlur::Draw] call #%d  dest=(%.1f,%.1f)-(%.1f,%.1f)", n,
					destinationRect.left, destinationRect.top,
					destinationRect.right, destinationRect.bottom);
				LogLine("    deviceContext    = %p", This ? This->GetDeviceContext() : nullptr);
				LogLine("    directionalBlurY = %p", This ? This->GetDirectionalBlurYEffect() : nullptr);
				LogLine("    >>> RAW D2D PATH IS LIVE ON THIS BUILD <<<");
			}
		}
		catch (...) {}
		return g_funCCustomBlur_Draw.call_org(This, drawingContext, destinationRect, point,
			interpolationMode, compositeMode);
	}

	// ---- lifecycle ----------------------------------------------------------

	void Attach()
	{
		if (g_startup) return;
		g_startup = true;

		g_nExecuteBlur = 0;
		g_nCustomBlurDraw = 0;

		LogLine("================================================================");
		LogLine("recon attach  build=%u  effectType=%d  blurMethod=%d  customAmount=%d  scaleOptimizer=%d",
			os::buildNumber, (int)g_configData.effectType, (int)g_configData.blurmethod,
			(int)g_configData.customAmount, (int)g_configData.scaleOptimizer);

		// Do not double-hook a target another section already owns.
		if (g_configData.customAmount)
		{
			LogLine("SKIP ExecuteBlur: BlurRadiusTweaker owns it (turn OFF custom blur amount to probe it)");
		}
		else if (os::buildNumber >= 26100)
		{
			g_funExecuteBlur24h2.Attach();
			g_hookedExecuteBlur = true;
			LogLine("hooked ExecuteBlur (24h2 signature)");
		}
		else if (os::buildNumber >= 22000)
		{
			g_funExecuteBlur.Attach();
			g_hookedExecuteBlur = true;
			LogLine("hooked ExecuteBlur");
		}
		else
		{
			LogLine("ExecuteBlur not used on this build (<22000)");
		}

		if (g_configData.scaleOptimizer)
		{
			LogLine("SKIP CCustomBlur::Draw: ScaleOptimizer owns it (turn OFF blur optimization to probe it)");
		}
		else
		{
			g_funCCustomBlur_Draw.Attach();
			g_hookedCustomBlurDraw = true;
			LogLine("hooked CCustomBlur::Draw (expected SILENT on Win11 - that is itself the finding)");
		}
	}

	void Detach()
	{
		if (!g_startup) return;

		if (g_hookedExecuteBlur)
		{
			if (os::buildNumber >= 26100) g_funExecuteBlur24h2.Detach();
			else g_funExecuteBlur.Detach();
			g_hookedExecuteBlur = false;
		}
		if (g_hookedCustomBlurDraw)
		{
			g_funCCustomBlur_Draw.Detach();
			g_hookedCustomBlurDraw = false;
		}

		LogLine("recon detach: ExecuteBlur calls=%d  CCustomBlur::Draw calls=%d",
			g_nExecuteBlur.load(), g_nCustomBlurDraw.load());
		g_startup = false;
	}

	void Refresh()
	{
		const bool want = (g_configData.effectType == effectType::LiquidGlass);
		if (want && !g_startup) Attach();
		else if (!want && g_startup) Detach();
	}
}
