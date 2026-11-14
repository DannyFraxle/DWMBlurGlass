/**
 * FileName: LiquidGlassPass.cpp
 *
 * See LiquidGlassPass.h.
*/
#include "LiquidGlassPass.h"
#include "LiquidGlassParams.h"
#include "GlassRectRegistry.h"
#include "PinchBands.h"
#include "GlassScene.h"
#include "CommonDef.h"
#include "../Helper/ReconLog.h"

#include "Shaders/LiquidGlassVS.h"
#include "Shaders/LiquidGlassPS.h"

#include <dxgi.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <vector>

namespace MDWMBlurGlassExt::LiquidGlassPass
{
	using namespace MDWMBlurGlass;
	using namespace CommonDef;

	namespace
	{
		constexpr int kMaxBands = GlassRects::kMaxEntries;
		// Readback ring per band: the GPU is a frame or two behind, and Map is
		// only ever attempted with DO_NOT_WAIT, so a few slots keep it flowing
		// without the render thread ever waiting on the GPU.
		constexpr int kRing = 3;
		constexpr int kSharedCount = 2;
		// Keyed-mutex keys of a shared band texture.
		constexpr UINT64 kKeyFree = 0;       // the render thread may render into it
		constexpr UINT64 kKeyRendered = 1;   // holds a band the UI thread has not drawn yet
		// Titlebars are not large; anything bigger than this is not one.
		constexpr UINT kMaxBandSide = 8192;

		struct Cache
		{
			ID3D11Device* device{ nullptr };
			ID3D11DeviceContext* context{ nullptr };
			ID3D11VertexShader* vs{ nullptr };
			ID3D11PixelShader* ps{ nullptr };
			ID3D11SamplerState* sampler{ nullptr };
			ID3D11Buffer* constants{ nullptr };
			ID3D11BlendState* blend{ nullptr };
			ID3D11DepthStencilState* depth{ nullptr };
			ID3D11RasterizerState* raster{ nullptr };

			// A frame-sized copy to sample from. Only the area around each
			// titlebar is refreshed into it, each time that band is rendered.
			ID3D11Texture2D* scratch{ nullptr };
			ID3D11ShaderResourceView* scratchSRV{ nullptr };
			UINT scratchWidth{ 0 };
			UINT scratchHeight{ 0 };
			DXGI_FORMAT scratchFormat{ DXGI_FORMAT_UNKNOWN };
		};

		// One titlebar's band: rendered here, read back, handed to the UI thread.
		struct Band
		{
			const void* owner{ nullptr };
			UINT width{ 0 };
			UINT height{ 0 };
			ID3D11Texture2D* texture{ nullptr };
			ID3D11RenderTargetView* rtv{ nullptr };
			ID3D11Texture2D* staging[kRing]{};
			bool pending[kRing]{};
			RECT renderedFor[kRing]{};   // screen rect each readback was rendered at
			uint64_t renderedAt[kRing]{};   // QueryPerformanceCounter, likewise
			bool handedOver[kRing]{};   // that render already went to the UI thread on the GPU
			int next{ 0 };
			ULONGLONG lastUsed{ 0 };
			ULONGLONG lastRendered{ 0 };

			// Change detection, from the readbacks: when a readback last came
			// back visibly different (see Differs). Anything changing around a
			// still titlebar puts it back on the live path.
			ULONGLONG lastChangeAt{ 0 };
			// The last band read back that counted as a change; what the next
			// is compared against.
			std::vector<uint32_t> reference;
			RECT referenceRect{};
			// Where its window was (GetWindowRect) the last frame it was rendered.
			POINT windowAt{};
			bool windowSeen{ false };

			// While live the band is also copied into one of these and the UI
			// thread draws it straight from the GPU, a frame after it was
			// rendered. Two, so one can be rendered into while the UI thread
			// still holds the other.
			ID3D11Texture2D* shared[kSharedCount]{};
			IDXGIKeyedMutex* sharedMutex[kSharedCount]{};
			HANDLE sharedHandle[kSharedCount]{};
			int sharedNext{ 0 };
			bool sharedFailed{ false };   // this device cannot make them; read back instead
		};

		// A band is rendered on every present. It is LIVE - handed to the UI
		// thread on the GPU each frame - while its window moves (and kLiveMs
		// after), and for kChangeLiveMs after its surroundings last changed.
		// Otherwise it is only read back, to spot the next change: drawing an
		// unchanged band would recompose the window and make DWM present again,
		// forever.
		constexpr ULONGLONG kLiveMs = 1000;
		constexpr ULONGLONG kChangeLiveMs = 500;
		// After a present that left a readback waiting, DWM is nudged into one
		// more burst of presents so it gets collected - a single change (a
		// cursor blink, the last frame of an animation) is otherwise only seen
		// at the next unrelated present. Presents inside the burst ask for no
		// further nudge, so it stops by itself.
		constexpr ULONGLONG kNudgeMs = 250;
		ULONGLONG g_nudgeUntil{ 0 };


		Cache g_cache{};
		Band g_bands[kMaxBands]{};
		std::atomic_bool g_reported{ false };
		std::atomic_bool g_reportedMismatch{ false };
		std::atomic_long g_statRendered{ 0 };
		std::atomic_long g_statPublished{ 0 };
		std::atomic_long g_statNotSettled{ 0 };
		std::atomic_long g_statShared{ 0 };
		std::atomic_long g_statDrained{ 0 };
		std::atomic_long g_statHashChanges{ 0 };
		std::atomic_long g_statShifted{ 0 };
		std::atomic_bool g_anyLive{ false };
		std::atomic_bool g_reportedShared{ false };

		template <typename T>
		void SafeRelease(T*& p)
		{
			if (p)
			{
				p->Release();
				p = nullptr;
			}
		}

		void ReleaseBand(Band& band)
		{
			SafeRelease(band.rtv);
			SafeRelease(band.texture);
			for (auto& staging : band.staging)
				SafeRelease(staging);
			// The UI thread keeps its own reference to any it has opened.
			for (int i = 0; i < kSharedCount; ++i)
			{
				SafeRelease(band.sharedMutex[i]);
				SafeRelease(band.shared[i]);
			}
			band = Band{};
		}

		uint64_t Now()
		{
			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);
			return static_cast<uint64_t>(counter.QuadPart);
		}

		// The two shared textures a moving band is handed over in, made the
		// first time it moves. Any failure and this band reads back instead.
		bool EnsureShared(Band& band)
		{
			if (band.sharedFailed)
				return false;
			if (band.shared[0])
				return true;

			D3D11_TEXTURE2D_DESC td{};
			td.Width = band.width;
			td.Height = band.height;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			// Shader resource so D2D can draw from it on the UI thread.
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
			for (int i = 0; i < kSharedCount; ++i)
			{
				IDXGIResource* resource{ nullptr };
				const bool ok{
					SUCCEEDED(g_cache.device->CreateTexture2D(&td, nullptr, &band.shared[i])) &&
					SUCCEEDED(band.shared[i]->QueryInterface(IID_PPV_ARGS(&band.sharedMutex[i]))) &&
					SUCCEEDED(band.shared[i]->QueryInterface(IID_PPV_ARGS(&resource))) &&
					SUCCEEDED(resource->GetSharedHandle(&band.sharedHandle[i])) && band.sharedHandle[i]
				};
				SafeRelease(resource);
				if (!ok)
				{
					for (int j = 0; j < kSharedCount; ++j)
					{
						SafeRelease(band.sharedMutex[j]);
						SafeRelease(band.shared[j]);
						band.sharedHandle[j] = nullptr;
					}
					band.sharedFailed = true;
					ReconLog::Write("[pass] shared band textures unavailable; reading back instead");
					return false;
				}
			}
			return true;
		}

		// Copies the freshly rendered band into a shared texture and tells the
		// UI thread. Never waits: a texture the UI thread is drawing from right
		// now is skipped, and one it has not got round to is simply re-used for
		// the newer band.
		bool HandOffShared(Band& band, const RECT& rect, uint64_t renderedAt)
		{
			for (int k = 0; k < kSharedCount; ++k)
			{
				const int i{ (band.sharedNext + k) % kSharedCount };
				auto* mutex{ band.sharedMutex[i] };
				UINT64 key{ kKeyFree };
				HRESULT hr{ mutex->AcquireSync(kKeyFree, 0) };
				if (hr != S_OK)
				{
					key = kKeyRendered;
					hr = mutex->AcquireSync(kKeyRendered, 0);
				}
				if (hr != S_OK)
					continue;

				// Recorded while we hold the texture, so by the time the UI thread
				// can take it, it is described by what it now holds.
				if (!PinchBands::PublishShared(band.owner, rect, band.width, band.height, band.sharedHandle[i], renderedAt))
				{
					mutex->ReleaseSync(key);
					return false;
				}
				g_cache.context->CopyResource(band.shared[i], band.texture);
				mutex->ReleaseSync(kKeyRendered);
				band.sharedNext = (i + 1) % kSharedCount;
				return true;
			}
			return false;
		}

		void ReleaseAll()
		{
			for (auto& band : g_bands)
				ReleaseBand(band);
			SafeRelease(g_cache.scratchSRV);
			SafeRelease(g_cache.scratch);
			SafeRelease(g_cache.raster);
			SafeRelease(g_cache.depth);
			SafeRelease(g_cache.blend);
			SafeRelease(g_cache.constants);
			SafeRelease(g_cache.sampler);
			SafeRelease(g_cache.ps);
			SafeRelease(g_cache.vs);
			SafeRelease(g_cache.context);
			SafeRelease(g_cache.device);
			g_cache.scratchWidth = 0;
			g_cache.scratchHeight = 0;
			g_cache.scratchFormat = DXGI_FORMAT_UNKNOWN;
		}

		bool EnsureDevice(ID3D11Texture2D* frame)
		{
			ID3D11Device* device = nullptr;
			frame->GetDevice(&device);
			if (!device)
				return false;

			if (g_cache.device == device)
			{
				device->Release();
				return true;
			}

			// A different device than last time means DWM was reset underneath
			// us; everything cached belongs to the old one.
			ReleaseAll();
			g_cache.device = device;   // keeps the reference GetDevice took
			device->GetImmediateContext(&g_cache.context);
			return g_cache.context != nullptr;
		}

		bool EnsureShaders()
		{
			if (g_cache.vs && g_cache.ps && g_cache.sampler && g_cache.constants &&
				g_cache.blend && g_cache.depth && g_cache.raster)
				return true;

			auto* device = g_cache.device;

			if (!g_cache.vs &&
				FAILED(device->CreateVertexShader(g_liquidGlassVS, sizeof(g_liquidGlassVS), nullptr, &g_cache.vs)))
				return false;
			if (!g_cache.ps &&
				FAILED(device->CreatePixelShader(g_liquidGlassPS, sizeof(g_liquidGlassPS), nullptr, &g_cache.ps)))
				return false;

			if (!g_cache.sampler)
			{
				D3D11_SAMPLER_DESC sd{};
				sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
				sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
				sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
				sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
				sd.MaxLOD = D3D11_FLOAT32_MAX;
				if (FAILED(device->CreateSamplerState(&sd, &g_cache.sampler)))
					return false;
			}

			if (!g_cache.constants)
			{
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = sizeof(ShaderParams);
				bd.Usage = D3D11_USAGE_DYNAMIC;
				bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(device->CreateBuffer(&bd, nullptr, &g_cache.constants)))
					return false;
			}

			// The band is drawn into a texture of its own that is cleared first,
			// so no blending: every pixel is written outright.
			if (!g_cache.blend)
			{
				D3D11_BLEND_DESC bd{};
				bd.RenderTarget[0].BlendEnable = FALSE;
				bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
				if (FAILED(device->CreateBlendState(&bd, &g_cache.blend)))
					return false;
			}

			if (!g_cache.depth)
			{
				D3D11_DEPTH_STENCIL_DESC dd{};
				dd.DepthEnable = FALSE;
				dd.StencilEnable = FALSE;
				if (FAILED(device->CreateDepthStencilState(&dd, &g_cache.depth)))
					return false;
			}

			if (!g_cache.raster)
			{
				D3D11_RASTERIZER_DESC rd{};
				rd.FillMode = D3D11_FILL_SOLID;
				rd.CullMode = D3D11_CULL_NONE;
				rd.DepthClipEnable = TRUE;
				rd.ScissorEnable = FALSE;
				if (FAILED(device->CreateRasterizerState(&rd, &g_cache.raster)))
					return false;
			}

			return true;
		}

		bool EnsureScratch(UINT width, UINT height, DXGI_FORMAT format)
		{
			if (g_cache.scratch && g_cache.scratchWidth == width &&
				g_cache.scratchHeight == height && g_cache.scratchFormat == format)
				return true;

			SafeRelease(g_cache.scratchSRV);
			SafeRelease(g_cache.scratch);

			D3D11_TEXTURE2D_DESC td{};
			td.Width = width;
			td.Height = height;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = format;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(g_cache.device->CreateTexture2D(&td, nullptr, &g_cache.scratch)))
				return false;
			if (FAILED(g_cache.device->CreateShaderResourceView(g_cache.scratch, nullptr, &g_cache.scratchSRV)))
			{
				SafeRelease(g_cache.scratch);
				return false;
			}

			g_cache.scratchWidth = width;
			g_cache.scratchHeight = height;
			g_cache.scratchFormat = format;
			return true;
		}

		Band* EnsureBand(const void* owner, UINT width, UINT height)
		{
			Band* band{ nullptr };
			for (auto& candidate : g_bands)
			{
				if (candidate.owner == owner)
				{
					band = &candidate;
					break;
				}
			}
			if (!band)
			{
				for (auto& candidate : g_bands)
				{
					if (!candidate.owner)
					{
						band = &candidate;
						break;
					}
				}
			}
			if (!band)
				return nullptr;

			if (band->owner == owner && band->width == width && band->height == height && band->texture)
				return band;

			ReleaseBand(*band);

			D3D11_TEXTURE2D_DESC td{};
			td.Width = width;
			td.Height = height;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET;
			if (FAILED(g_cache.device->CreateTexture2D(&td, nullptr, &band->texture)) ||
				FAILED(g_cache.device->CreateRenderTargetView(band->texture, nullptr, &band->rtv)))
			{
				ReleaseBand(*band);
				return nullptr;
			}

			td.Usage = D3D11_USAGE_STAGING;
			td.BindFlags = 0;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (auto& staging : band->staging)
			{
				if (FAILED(g_cache.device->CreateTexture2D(&td, nullptr, &staging)))
				{
					ReleaseBand(*band);
					return nullptr;
				}
			}

			band->owner = owner;
			band->width = width;
			band->height = height;
			return band;
		}

		// Saves and restores everything the pass touches. Leaving any of it
		// changed corrupts DWM's next draw in ways that look like driver bugs,
		// so this is deliberately exhaustive rather than clever.
		struct StateGuard
		{
			ID3D11DeviceContext* ctx;

			ID3D11VertexShader* vs{ nullptr };
			ID3D11PixelShader* ps{ nullptr };
			ID3D11GeometryShader* gs{ nullptr };
			ID3D11HullShader* hs{ nullptr };
			ID3D11DomainShader* ds{ nullptr };
			UINT vsInstances{ 0 }, psInstances{ 0 }, gsInstances{ 0 };
			// DWM does not use dynamic shader linkage; 32 is plenty and keeps this
			// off the render thread's stack.
			ID3D11ClassInstance* vsClass[32]{}, * psClass[32]{}, * gsClass[32]{};

			ID3D11InputLayout* layout{ nullptr };
			D3D11_PRIMITIVE_TOPOLOGY topology{};
			ID3D11Buffer* vertexBuffer{ nullptr };
			UINT vertexStride{ 0 }, vertexOffset{ 0 };
			ID3D11Buffer* indexBuffer{ nullptr };
			DXGI_FORMAT indexFormat{};
			UINT indexOffset{ 0 };

			ID3D11Buffer* psConstants{ nullptr };
			ID3D11Buffer* vsConstants{ nullptr };
			ID3D11ShaderResourceView* psSRV{ nullptr };
			ID3D11SamplerState* psSampler{ nullptr };

			ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* dsv{ nullptr };
			ID3D11BlendState* blend{ nullptr };
			FLOAT blendFactor[4]{};
			UINT sampleMask{ 0 };
			ID3D11DepthStencilState* depth{ nullptr };
			UINT stencilRef{ 0 };
			ID3D11RasterizerState* raster{ nullptr };

			UINT viewportCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT scissorCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};

			explicit StateGuard(ID3D11DeviceContext* c) : ctx(c)
			{
				vsInstances = psInstances = gsInstances = 32;
				ctx->VSGetShader(&vs, vsClass, &vsInstances);
				ctx->PSGetShader(&ps, psClass, &psInstances);
				ctx->GSGetShader(&gs, gsClass, &gsInstances);
				ctx->HSGetShader(&hs, nullptr, nullptr);
				ctx->DSGetShader(&ds, nullptr, nullptr);

				ctx->IAGetInputLayout(&layout);
				ctx->IAGetPrimitiveTopology(&topology);
				ctx->IAGetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
				ctx->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);

				ctx->PSGetConstantBuffers(0, 1, &psConstants);
				ctx->VSGetConstantBuffers(0, 1, &vsConstants);
				ctx->PSGetShaderResources(0, 1, &psSRV);
				ctx->PSGetSamplers(0, 1, &psSampler);

				ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
				ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
				ctx->OMGetDepthStencilState(&depth, &stencilRef);
				ctx->RSGetState(&raster);
				ctx->RSGetViewports(&viewportCount, viewports);
				ctx->RSGetScissorRects(&scissorCount, scissors);
			}

			~StateGuard()
			{
				ctx->VSSetShader(vs, vsInstances ? vsClass : nullptr, vsInstances);
				ctx->PSSetShader(ps, psInstances ? psClass : nullptr, psInstances);
				ctx->GSSetShader(gs, gsInstances ? gsClass : nullptr, gsInstances);
				ctx->HSSetShader(hs, nullptr, 0);
				ctx->DSSetShader(ds, nullptr, 0);

				ctx->IASetInputLayout(layout);
				ctx->IASetPrimitiveTopology(topology);
				ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
				ctx->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset);

				ctx->PSSetConstantBuffers(0, 1, &psConstants);
				ctx->VSSetConstantBuffers(0, 1, &vsConstants);
				ctx->PSSetShaderResources(0, 1, &psSRV);
				ctx->PSSetSamplers(0, 1, &psSampler);

				ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
				ctx->OMSetBlendState(blend, blendFactor, sampleMask);
				ctx->OMSetDepthStencilState(depth, stencilRef);
				ctx->RSSetState(raster);
				ctx->RSSetViewports(viewportCount, viewportCount ? viewports : nullptr);
				ctx->RSSetScissorRects(scissorCount, scissorCount ? scissors : nullptr);

				for (UINT i = 0; i < vsInstances; ++i) SafeRelease(vsClass[i]);
				for (UINT i = 0; i < psInstances; ++i) SafeRelease(psClass[i]);
				for (UINT i = 0; i < gsInstances; ++i) SafeRelease(gsClass[i]);
				SafeRelease(vs); SafeRelease(ps); SafeRelease(gs); SafeRelease(hs); SafeRelease(ds);
				SafeRelease(layout); SafeRelease(vertexBuffer); SafeRelease(indexBuffer);
				SafeRelease(psConstants); SafeRelease(vsConstants);
				SafeRelease(psSRV); SafeRelease(psSampler);
				for (auto& rtv : rtvs) SafeRelease(rtv);
				SafeRelease(dsv);
				SafeRelease(blend); SafeRelease(depth); SafeRelease(raster);
			}
		};

		RECT ClampRect(const RECT& r, UINT width, UINT height)
		{
			RECT c{};
			c.left = (std::max)(r.left, 0L);
			c.top = (std::max)(r.top, 0L);
			c.right = (std::min)(r.right, static_cast<LONG>(width));
			c.bottom = (std::min)(r.bottom, static_cast<LONG>(height));
			return c;
		}

		bool IsLinearFormat(DXGI_FORMAT format)
		{
			return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
		}

		// A change is only a change if it shows: more than kMinChangedPixels
		// pixels differing by more than kMinChannelDelta in some channel, from
		// the last band that counted as changed. A byte here and there - the
		// band picking up its own faint output, blending noise, slow drift -
		// must not keep it live: every live frame is drawn and committed, which
		// makes DWM present again, which renders again... at the full refresh
		// rate on an idle desktop.
		constexpr int kMinChannelDelta = 6;
		constexpr int kMinChangedPixels = 8;

		struct ChangeLog
		{
			long significant{ 0 };
			long minor{ 0 };
			int lastPixels{ 0 };
			int lastMax{ 0 };
			RECT lastBox{};
			UINT lastW{ 0 }, lastH{ 0 };
			ULONGLONG loggedAt{ 0 };
		};
		ChangeLog g_changeLog;

		bool Differs(Band& band, const RECT& rect, const void* pixels, UINT rowPitch)
		{
			const size_t count{ static_cast<size_t>(band.width) * band.height };
			const auto* src{ static_cast<const uint8_t*>(pixels) };
			auto keep{ [&]
			{
				band.reference.resize(count);
				for (UINT y = 0; y < band.height; ++y)
					memcpy(band.reference.data() + static_cast<size_t>(y) * band.width, src + static_cast<size_t>(y) * rowPitch, band.width * 4);
				band.referenceRect = rect;
			} };
			if (band.reference.size() != count || band.referenceRect.left != rect.left || band.referenceRect.top != rect.top ||
				band.referenceRect.right != rect.right || band.referenceRect.bottom != rect.bottom)
			{
				keep();
				return true;
			}

			int changed{ 0 }, maxDelta{ 0 };
			RECT box{ LONG_MAX, LONG_MAX, -1, -1 };
			for (UINT y = 0; y < band.height; ++y)
			{
				const auto* row{ src + static_cast<size_t>(y) * rowPitch };
				const auto* ref{ reinterpret_cast<const uint8_t*>(band.reference.data() + static_cast<size_t>(y) * band.width) };
				for (UINT x = 0; x < band.width; ++x)
				{
					int delta{ 0 };
					for (int c = 0; c < 4; ++c)
						delta = (std::max)(delta, std::abs(static_cast<int>(row[x * 4 + c]) - static_cast<int>(ref[x * 4 + c])));
					if (delta > kMinChannelDelta)
					{
						++changed;
						box.left = (std::min)(box.left, static_cast<LONG>(x));
						box.top = (std::min)(box.top, static_cast<LONG>(y));
						box.right = (std::max)(box.right, static_cast<LONG>(x));
						box.bottom = (std::max)(box.bottom, static_cast<LONG>(y));
					}
					maxDelta = (std::max)(maxDelta, delta);
				}
			}

			const bool significant{ changed > kMinChangedPixels };
			if (significant)
			{
				keep();
				++g_changeLog.significant;
				g_changeLog.lastPixels = changed;
				g_changeLog.lastMax = maxDelta;
				g_changeLog.lastBox = box;
				g_changeLog.lastW = band.width;
				g_changeLog.lastH = band.height;
			}
			else if (maxDelta > 0)
				++g_changeLog.minor;

			const ULONGLONG now{ GetTickCount64() };
			if (now - g_changeLog.loggedAt >= 2000 && (g_changeLog.significant || g_changeLog.minor))
			{
				ReconLog::Write("[change] 2s: significant=%ld minor=%ld last: %d px max delta %d in (%ld,%ld)-(%ld,%ld) of %ux%u",
					g_changeLog.significant, g_changeLog.minor, g_changeLog.lastPixels, g_changeLog.lastMax,
					g_changeLog.lastBox.left, g_changeLog.lastBox.top, g_changeLog.lastBox.right, g_changeLog.lastBox.bottom,
					g_changeLog.lastW, g_changeLog.lastH);
				g_changeLog = ChangeLog{};
				g_changeLog.loggedAt = now;
			}
			return significant;
		}

		// Collects whatever readbacks the GPU has finished, without waiting.
		bool DrainReadbacks(ULONGLONG now)
		{
			bool changed{ false };
			auto* ctx{ g_cache.context };
			for (auto& band : g_bands)
			{
				if (!band.owner)
					continue;
				// Oldest first, so a newer band is never overwritten by an older one.
				for (int k = 0; k < kRing; ++k)
				{
					const int slot{ (band.next + k) % kRing };
					if (!band.pending[slot])
						continue;
					D3D11_MAPPED_SUBRESOURCE mapped{};
					const HRESULT hr{ ctx->Map(band.staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) };
					if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
						break;
					band.pending[slot] = false;
					if (FAILED(hr))
						continue;
					g_statDrained.fetch_add(1, std::memory_order_relaxed);
					if (Differs(band, band.renderedFor[slot], mapped.pData, mapped.RowPitch))
					{
						band.lastChangeAt = now;
						g_statHashChanges.fetch_add(1, std::memory_order_relaxed);
						// Shown straight away from these pixels, unless this render
						// already went over on the GPU (then a newer one is on show).
						if (!band.handedOver[slot] &&
							PinchBands::Publish(band.owner, band.renderedFor[slot], band.width, band.height,
								mapped.pData, mapped.RowPitch, band.renderedAt[slot]))
						{
							changed = true;
							g_statPublished.fetch_add(1, std::memory_order_relaxed);
						}
					}
					ctx->Unmap(band.staging[slot], 0);
				}
			}
			return changed;
		}
	}

	bool RenderBands(ID3D11Texture2D* frame, UINT width, UINT height)
	{
		if (!frame || !width || !height)
			return false;
		if (!EnsureDevice(frame) || !EnsureShaders())
			return false;

		// Readbacks queued on earlier frames first: whatever the GPU has done.
		const ULONGLONG now{ GetTickCount64() };
		bool changed{ DrainReadbacks(now) };
		g_anyLive.store(false, std::memory_order_relaxed);

		// Static rather than on the stack: this runs deep inside dwmcore's
		// present, on a thread whose stack is not ours to spend. Only ever
		// touched by the render thread.
		static GlassRects::Entry entries[GlassRects::kMaxEntries];
		GlassRects::Display display{};
		const int count{ GlassRects::Snapshot(entries, GlassRects::kMaxEntries, &display) };

		// Bands nobody has asked for in a while belong to windows that are gone.
		for (auto& band : g_bands)
		{
			if (band.owner && now - band.lastUsed > 10000)
				ReleaseBand(band);
		}
		if (count <= 0)
			return changed;

		// The scene is in screen pixels and only describes a single-monitor
		// desktop; anything else and the rects would land in the wrong place.
		if (!display.valid || display.width != width || display.height != height)
		{
			if (!g_reportedMismatch.exchange(true))
			{
				ReconLog::Write("[pass] not rendering: frame %ux%u, display %ux%u valid=%d",
					width, height, display.width, display.height, display.valid ? 1 : 0);
			}
			return changed;
		}

		D3D11_TEXTURE2D_DESC td{};
		frame->GetDesc(&td);
		if (td.SampleDesc.Count != 1 || !EnsureScratch(width, height, td.Format))
			return changed;

		auto* ctx = g_cache.context;
		StateGuard guard{ ctx };

		ID3D11Buffer* noBuffer = nullptr;
		UINT zero = 0;
		ID3D11ShaderResourceView* noSRV = nullptr;

		const float cfgRim{ static_cast<float>(g_configData.glassRimThickness) };
		const float cfgReach{ g_configData.glassRefractionAmount };
		const float far_{ 100000.f };

		bool anyLive{ false };
		for (int n = 0; n < count; ++n)
		{
			auto& entry{ entries[n] };   // this frame's snapshot; ours to adjust
			// Where the window is NOW. The rect was published when DWM last
			// validated the window, and during a fast drag DWM slides it for
			// many frames without validating - the band then gathered the
			// surroundings of where the window had been and carried them along,
			// for a second or so. So it follows the window's live position.
			POINT windowNow{ LONG_MIN, LONG_MIN };
			if (entry.hasWindowOrigin)
			{
				if (RECT windowRect{}; GetWindowRect(entry.hwnd, &windowRect))
				{
					windowNow = { windowRect.left, windowRect.top };
					const LONG dx{ windowRect.left - entry.windowOrigin.x };
					const LONG dy{ windowRect.top - entry.windowOrigin.y };
					if (dx || dy)
					{
						OffsetRect(&entry.rect, dx, dy);
						g_statShifted.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
			// Live while the window is moving (and for a second after), so the
			// compacted surroundings follow it; once it has been still that long
			// the band only needs refreshing for what changes around it.
			const bool moving{ now < entry.settledAt + kLiveMs };

			const LONG bandW{ entry.rect.right - entry.rect.left };
			const LONG bandH{ entry.rect.bottom - entry.rect.top };
			if (bandW <= 0 || bandH <= 0 || bandW > static_cast<LONG>(kMaxBandSide) || bandH > static_cast<LONG>(kMaxBandSide))
				continue;

			Band* band{ EnsureBand(entry.owner, static_cast<UINT>(bandW), static_cast<UINT>(bandH)) };
			if (!band)
				continue;
			band->lastUsed = now;
			// Moved since the last frame (whether or not the scene was
			// re-published): live, and for a while after, so the band settles
			// on the surroundings where the window stopped. Since the last
			// frame, not since publishing - a window DWM never re-validates
			// after a drag would otherwise count as moving forever.
			if (windowNow.x != LONG_MIN)
			{
				if (band->windowSeen && (windowNow.x != band->windowAt.x || windowNow.y != band->windowAt.y))
					band->lastChangeAt = now;
				band->windowAt = windowNow;
				band->windowSeen = true;
			}
			const bool live{ moving || now - band->lastChangeAt < kChangeLiveMs };
			anyLive = anyLive || live;
			// Live: straight to the UI thread on the GPU.
			const bool useShared{ live && PinchBands::SharedEnabled() && EnsureShared(*band) };
			// A readback slot is free: this render is also read back, to spot
			// changes (and, when not live, to show them).
			const bool readBack{ !band->pending[band->next] };
			if (!useShared && !readBack)
				continue;
			if (!live)
				g_statNotSettled.fetch_add(1, std::memory_order_relaxed);
			band->lastRendered = now;

			// The band is as thick as configured (the GUI's edge thickness), up
			// to most of the titlebar - it sits under the title and buttons, so
			// it may run behind them. How much it squeezes stays the same at any
			// thickness: glassRefractionAmount over the default 12 px band, so
			// the defaults (24 / 12) gather twice the band's width from outside.
			const float rim{ (std::min)(cfgRim, 0.9f * static_cast<float>((std::min)(bandW, bandH))) };
			const float compaction{ std::clamp(cfgReach / 12.f, 1.f, 5.f) };
			const float reach{ rim * compaction };
			const float chromatic{ entry.active ? 0.15f : 0.08f };

			// Refresh what this band samples: the strip around the titlebar.
			const LONG margin{ static_cast<LONG>(reach * (1.f + chromatic) + rim) + 4 };   // + rim: the blur taps
			const RECT copy{ ClampRect(RECT{ entry.rect.left - margin, entry.rect.top - margin,
				entry.rect.right + margin, entry.rect.bottom + margin }, width, height) };
			if (copy.right <= copy.left || copy.bottom <= copy.top)
				continue;

			ctx->PSSetShaderResources(0, 1, &noSRV);
			ctx->OMSetRenderTargets(0, nullptr, nullptr);

			D3D11_BOX box{};
			box.left = static_cast<UINT>(copy.left);
			box.top = static_cast<UINT>(copy.top);
			box.right = static_cast<UINT>(copy.right);
			box.bottom = static_cast<UINT>(copy.bottom);
			box.front = 0;
			box.back = 1;
			ctx->CopySubresourceRegion(g_cache.scratch, 0, box.left, box.top, 0, frame, 0, &box);

			ShaderParams params{};
			params.quad[0] = 0.f;
			params.quad[1] = 0.f;
			params.quad[2] = static_cast<float>(bandW);
			params.quad[3] = static_cast<float>(bandH);
			// Edges that get no rim are pushed so far out they are never the
			// nearest one.
			params.rect[0] = (entry.edges & GlassRects::LeftEdge) ? static_cast<float>(entry.rect.left) : -far_;
			params.rect[1] = (entry.edges & GlassRects::TopEdge) ? static_cast<float>(entry.rect.top) : -far_;
			params.rect[2] = (entry.edges & GlassRects::RightEdge) ? static_cast<float>(entry.rect.right) : far_;
			params.rect[3] = (entry.edges & GlassRects::BottomEdge) ? static_cast<float>(entry.rect.bottom) : far_;
			params.invTarget[0] = 1.f / static_cast<float>(bandW);
			params.invTarget[1] = 1.f / static_cast<float>(bandH);
			params.rim = rim;
			params.cornerRadius = entry.cornerRadius;
			params.reach = reach;
			// Mirrored about the edge and packed hard against it: well under 1,
			// so the steep rim of a rounded edge holds most of the reflection.
			params.gamma = 0.5f;
			params.chromatic = chromatic;
			// The reflection brightens toward the rim (up to 1.25x there). Kept
			// modest: it is screened onto the pane, so it only ever adds light,
			// and more read as a light frame round the titlebar.
			params.highlight = 0.25f;
			params.lightDir[0] = -0.5547f;   // up and a little to the left
			params.lightDir[1] = -0.8321f;
			params.encodeSrgb = IsLinearFormat(td.Format) ? 1.f : 0.f;
			params.debug = g_configData.glassDebugRim ? 1.f : 0.f;
			params.origin[0] = static_cast<float>(entry.rect.left);
			params.origin[1] = static_cast<float>(entry.rect.top);
			params.invFrame[0] = 1.f / static_cast<float>(width);
			params.invFrame[1] = 1.f / static_cast<float>(height);
			params.occluderCount = 0.f;

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(ctx->Map(g_cache.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				break;
			memcpy(mapped.pData, &params, sizeof(params));
			ctx->Unmap(g_cache.constants, 0);

			D3D11_VIEWPORT viewport{};
			viewport.Width = static_cast<float>(bandW);
			viewport.Height = static_cast<float>(bandH);
			viewport.MaxDepth = 1.f;

			const FLOAT clear[4]{ 0.f, 0.f, 0.f, 0.f };
			ctx->ClearRenderTargetView(band->rtv, clear);

			ctx->IASetInputLayout(nullptr);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			ctx->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
			ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
			ctx->VSSetShader(g_cache.vs, nullptr, 0);
			ctx->PSSetShader(g_cache.ps, nullptr, 0);
			ctx->GSSetShader(nullptr, nullptr, 0);
			ctx->HSSetShader(nullptr, nullptr, 0);
			ctx->DSSetShader(nullptr, nullptr, 0);
			ctx->VSSetConstantBuffers(0, 1, &g_cache.constants);
			ctx->PSSetConstantBuffers(0, 1, &g_cache.constants);
			ctx->PSSetSamplers(0, 1, &g_cache.sampler);

			const FLOAT blendFactor[4]{ 1.f, 1.f, 1.f, 1.f };
			ctx->OMSetBlendState(g_cache.blend, blendFactor, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(g_cache.depth, 0);
			ctx->RSSetState(g_cache.raster);
			ctx->RSSetViewports(1, &viewport);
			ctx->OMSetRenderTargets(1, &band->rtv, nullptr);
			ctx->PSSetShaderResources(0, 1, &g_cache.scratchSRV);
			ctx->Draw(4, 0);

			ctx->PSSetShaderResources(0, 1, &noSRV);
			ctx->OMSetRenderTargets(0, nullptr, nullptr);

			const uint64_t renderedAt{ Now() };
			g_statRendered.fetch_add(1, std::memory_order_relaxed);
			bool handedOver{ false };
			if (useShared)
			{
				if (HandOffShared(*band, entry.rect, renderedAt))
				{
					handedOver = true;
					// Readbacks still in flight are older than this: they still
					// count for change detection, but are never shown over it.
					for (auto& done : band->handedOver)
						done = true;
					changed = true;
					g_statShared.fetch_add(1, std::memory_order_relaxed);
					if (!g_reportedShared.exchange(true))
					{
						LUID luid{};
						IDXGIDevice* dxgi{ nullptr };
						if (SUCCEEDED(g_cache.device->QueryInterface(IID_PPV_ARGS(&dxgi))))
						{
							IDXGIAdapter* adapter{ nullptr };
							if (SUCCEEDED(dxgi->GetAdapter(&adapter)))
							{
								DXGI_ADAPTER_DESC desc{};
								adapter->GetDesc(&desc);
								luid = desc.AdapterLuid;
								adapter->Release();
							}
							dxgi->Release();
						}
						ReconLog::Write("[pass] first band handed over on the GPU: render device %p adapter %08lX:%08lX flags 0x%X fl 0x%X handle %p",
							g_cache.device, static_cast<unsigned long>(luid.HighPart), luid.LowPart,
							g_cache.device->GetCreationFlags(), static_cast<unsigned>(g_cache.device->GetFeatureLevel()),
							band->sharedHandle[(band->sharedNext + kSharedCount - 1) % kSharedCount]);
					}
				}
			}
			if (readBack)
			{
				// Queue the readback; DrainReadbacks collects it on a later frame.
				ctx->CopyResource(band->staging[band->next], band->texture);
				band->pending[band->next] = true;
				band->handedOver[band->next] = handedOver;
				band->renderedFor[band->next] = entry.rect;
				band->renderedAt[band->next] = renderedAt;
				band->next = (band->next + 1) % kRing;
				// Not live: nothing else may present to collect it. Ask for a
				// few more frames, unless this present is one of those.
				if (!handedOver && now >= g_nudgeUntil)
				{
					g_nudgeUntil = now + kNudgeMs;
					GlassScene::Nudge();
				}
			}

			if (!g_reported.exchange(true))
			{
				ReconLog::Write("[pass] first band: hwnd=%p rect=(%ld,%ld)-(%ld,%ld) edges=0x%X rim=%.1f reach=%.1f frame=%ux%u fmt=%d",
					entry.hwnd, entry.rect.left, entry.rect.top, entry.rect.right, entry.rect.bottom,
					entry.edges, rim, reach, width, height, static_cast<int>(td.Format));
			}
		}

		g_anyLive.store(anyLive, std::memory_order_relaxed);
		return changed;
	}

	void Shutdown()
	{
		ReleaseAll();
		PinchBands::Clear();
		g_reported.store(false);
		g_reportedMismatch.store(false);
		g_statRendered.store(0);
		g_statPublished.store(0);
		g_statNotSettled.store(0);
		g_statShared.store(0);
		g_reportedShared.store(false);
	}

	Stats GetStats()
	{
		return Stats{ g_statRendered.load(), g_statPublished.load(), g_statNotSettled.load(), g_statShared.load(),
			g_statDrained.load(), g_statHashChanges.load(), g_statShifted.load() };
	}

	bool AnyLive()
	{
		return g_anyLive.load(std::memory_order_relaxed);
	}
}
