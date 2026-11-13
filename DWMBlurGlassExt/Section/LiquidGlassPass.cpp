/**
 * FileName: LiquidGlassPass.cpp
 *
 * See LiquidGlassPass.h.
*/
#include "LiquidGlassPass.h"
#include "LiquidGlassParams.h"
#include "GlassRectRegistry.h"
#include "CommonDef.h"
#include "../Helper/ReconLog.h"

#include "Shaders/LiquidGlassVS.h"
#include "Shaders/LiquidGlassPS.h"

#include <atomic>
#include <algorithm>
#include <cstring>

namespace MDWMBlurGlassExt::LiquidGlassPass
{
	using namespace MDWMBlurGlass;
	using namespace CommonDef;

	namespace
	{
		// Must match GlassScene's kFadeMs.
		constexpr ULONGLONG kFadeMs = 250;

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

			// A frame-sized copy to sample from: a shader cannot read the
			// render target it is writing. Only the area around each glass
			// window is refreshed into it, each frame.
			ID3D11Texture2D* scratch{ nullptr };
			ID3D11ShaderResourceView* scratchSRV{ nullptr };
			UINT scratchWidth{ 0 };
			UINT scratchHeight{ 0 };
			DXGI_FORMAT scratchFormat{ DXGI_FORMAT_UNKNOWN };
		};

		Cache g_cache{};
		std::atomic_bool g_reported{ false };
		std::atomic_bool g_reportedMismatch{ false };

		template <typename T>
		void SafeRelease(T*& p)
		{
			if (p)
			{
				p->Release();
				p = nullptr;
			}
		}

		void ReleaseAll()
		{
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

		bool EnsureDevice(ID3D11Texture2D* composed)
		{
			ID3D11Device* device = nullptr;
			composed->GetDevice(&device);
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

			// Premultiplied source-over: the shader returns the pinched strip
			// premultiplied by its own coverage, so the blender eases it into
			// the glass DWM already composed underneath.
			if (!g_cache.blend)
			{
				D3D11_BLEND_DESC bd{};
				auto& rt = bd.RenderTarget[0];
				rt.BlendEnable = TRUE;
				rt.SrcBlend = D3D11_BLEND_ONE;
				rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
				rt.BlendOp = D3D11_BLEND_OP_ADD;
				rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
				rt.DestBlendAlpha = D3D11_BLEND_ONE;   // leave the frame's alpha alone
				rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
				rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
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
				// The quad already covers exactly the rect; the scissor is a
				// hard guarantee that nothing outside it is ever written.
				rd.ScissorEnable = TRUE;
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
			ID3D11ClassInstance* vsClass[256]{}, * psClass[256]{}, * gsClass[256]{};

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
				vsInstances = psInstances = gsInstances = 256;
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

		bool WasRendered(HWND hwnd, const HWND* rendered, int count)
		{
			for (int i = 0; i < count; ++i)
			{
				if (rendered[i] == hwnd)
					return true;
			}
			return false;
		}
	}

	void RenderPinch(ID3D11Texture2D* target, UINT width, UINT height, const HWND* rendered, int renderedCount)
	{
		if (!target || !width || !height || renderedCount <= 0)
			return;

		GlassRects::Entry entries[GlassRects::kMaxEntries];
		GlassRects::Display display{};
		const int count{ GlassRects::Snapshot(entries, GlassRects::kMaxEntries, &display) };
		if (count <= 0)
			return;

		// The scene is in screen pixels and only describes a single-monitor
		// desktop; anything else and the rects would land in the wrong place.
		if (!display.valid || display.width != width || display.height != height)
		{
			if (!g_reportedMismatch.exchange(true))
			{
				ReconLog::Write("[pass] not drawing: target %ux%u, display %ux%u valid=%d",
					width, height, display.width, display.height, display.valid ? 1 : 0);
			}
			return;
		}

		// Which entries to draw this frame: DWM drew the window (so its pixels
		// may have been overwritten and it is genuinely on screen - not on
		// another desktop, the lock screen or fully covered), and it has
		// settled since it last moved.
		const ULONGLONG now{ GetTickCount64() };
		int todo[GlassRects::kMaxEntries]{};
		int todoCount{ 0 };
		for (int i = 0; i < count; ++i)
		{
			if (now < entries[i].settledAt)
				continue;
			if (!WasRendered(entries[i].hwnd, rendered, renderedCount))
				continue;
			todo[todoCount++] = i;
		}
		if (!todoCount)
			return;

		if (!EnsureDevice(target) || !EnsureShaders())
			return;

		D3D11_TEXTURE2D_DESC td{};
		target->GetDesc(&td);
		if (td.SampleDesc.Count != 1 || !(td.BindFlags & D3D11_BIND_RENDER_TARGET))
			return;
		if (!EnsureScratch(width, height, td.Format))
			return;

		// A view of this buffer for this frame only. Holding views on the
		// swapchain's buffers across frames would keep them alive past any
		// resize DWM does.
		ID3D11RenderTargetView* rtv = nullptr;
		if (FAILED(g_cache.device->CreateRenderTargetView(target, nullptr, &rtv)) || !rtv)
			return;

		auto* ctx = g_cache.context;
		{
			StateGuard guard{ ctx };

			D3D11_VIEWPORT viewport{};
			viewport.Width = static_cast<float>(width);
			viewport.Height = static_cast<float>(height);
			viewport.MaxDepth = 1.f;

			ID3D11Buffer* noBuffer = nullptr;
			UINT zero = 0;
			ID3D11ShaderResourceView* noSRV = nullptr;

			const float rim{ static_cast<float>(g_configData.glassRimThickness) };
			const float reach{ g_configData.glassRefractionAmount };
			const float intensity{ std::clamp(g_configData.glassIntensity, 0.f, 1.f) };
			const float far_{ 100000.f };

			for (int n = 0; n < todoCount; ++n)
			{
				const auto& entry{ entries[todo[n]] };
				const RECT quad{ ClampRect(entry.rect, width, height) };
				if (quad.right <= quad.left || quad.bottom <= quad.top)
					continue;

				const float chromatic{ entry.active ? 0.15f : 0.08f };

				// Refresh what this window's band samples: the strip around it.
				// The interior comes along too, which is harmless - the band is
				// never sampled - and keeps rounded corners correct.
				const LONG margin{ static_cast<LONG>(reach * (1.f + chromatic)) + 4 };
				const RECT copy{ ClampRect(RECT{ entry.rect.left - margin, entry.rect.top - margin,
					entry.rect.right + margin, entry.rect.bottom + margin }, width, height) };
				if (copy.right <= copy.left || copy.bottom <= copy.top)
					continue;

				// The copy must not see the pass's own bindings.
				ctx->PSSetShaderResources(0, 1, &noSRV);
				ctx->OMSetRenderTargets(0, nullptr, nullptr);

				D3D11_BOX box{};
				box.left = static_cast<UINT>(copy.left);
				box.top = static_cast<UINT>(copy.top);
				box.right = static_cast<UINT>(copy.right);
				box.bottom = static_cast<UINT>(copy.bottom);
				box.front = 0;
				box.back = 1;
				ctx->CopySubresourceRegion(g_cache.scratch, 0, box.left, box.top, 0, target, 0, &box);

				ShaderParams params{};
				params.quad[0] = static_cast<float>(quad.left);
				params.quad[1] = static_cast<float>(quad.top);
				params.quad[2] = static_cast<float>(quad.right);
				params.quad[3] = static_cast<float>(quad.bottom);
				// Edges that get no rim are pushed so far out they are never the
				// nearest one.
				params.rect[0] = (entry.edges & GlassRects::LeftEdge) ? static_cast<float>(entry.rect.left) : -far_;
				params.rect[1] = (entry.edges & GlassRects::TopEdge) ? static_cast<float>(entry.rect.top) : -far_;
				params.rect[2] = (entry.edges & GlassRects::RightEdge) ? static_cast<float>(entry.rect.right) : far_;
				params.rect[3] = (entry.edges & GlassRects::BottomEdge) ? static_cast<float>(entry.rect.bottom) : far_;
				params.invTarget[0] = 1.f / static_cast<float>(width);
				params.invTarget[1] = 1.f / static_cast<float>(height);
				params.rim = rim;
				params.cornerRadius = entry.cornerRadius;
				params.reach = reach;
				params.gamma = 1.4f;
				params.chromatic = chromatic;
				params.highlight = (0.25f + 0.45f * intensity) * (entry.active ? 1.f : 0.6f);
				params.lightDir[0] = -0.5547f;   // up and a little to the left
				params.lightDir[1] = -0.8321f;
				params.fade = std::clamp(static_cast<float>(now - entry.settledAt) / static_cast<float>(kFadeMs), 0.f, 1.f);
				params.debug = g_configData.glassDebugRim ? 1.f : 0.f;
				params.occluderCount = static_cast<float>((std::min)(entry.occluderCount, kMaxShaderOccluders));
				for (int i = 0; i < entry.occluderCount && i < kMaxShaderOccluders; ++i)
				{
					params.occluders[i][0] = static_cast<float>(entry.occluders[i].left);
					params.occluders[i][1] = static_cast<float>(entry.occluders[i].top);
					params.occluders[i][2] = static_cast<float>(entry.occluders[i].right);
					params.occluders[i][3] = static_cast<float>(entry.occluders[i].bottom);
				}

				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(ctx->Map(g_cache.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
					break;
				memcpy(mapped.pData, &params, sizeof(params));
				ctx->Unmap(g_cache.constants, 0);

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
				ctx->OMSetRenderTargets(1, &rtv, nullptr);
				ctx->PSSetShaderResources(0, 1, &g_cache.scratchSRV);

				D3D11_RECT scissor{ quad.left, quad.top, quad.right, quad.bottom };
				ctx->RSSetScissorRects(1, &scissor);
				ctx->Draw(4, 0);

				if (!g_reported.exchange(true))
				{
					ReconLog::Write("[pass] first pinch: hwnd=%p rect=(%ld,%ld)-(%ld,%ld) edges=0x%X radius=%.1f occluders=%d target=%ux%u fmt=%d",
						entry.hwnd, entry.rect.left, entry.rect.top, entry.rect.right, entry.rect.bottom,
						entry.edges, entry.cornerRadius, entry.occluderCount, width, height, static_cast<int>(td.Format));
				}
			}

			ctx->PSSetShaderResources(0, 1, &noSRV);
		}
		rtv->Release();
	}

	void Shutdown()
	{
		ReleaseAll();
		g_reported.store(false);
		g_reportedMismatch.store(false);
	}
}
