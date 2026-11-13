/**
 * FileName: DefFunctionList.h
 *
 * Copyright (C) 2024 Maplespe
 *
 * This file is part of MToolBox and DWMBlurGlass.
 * DWMBlurGlass is free software: you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation, either version 3
 * of the License, or any later version.
 *
 * DWMBlurGlass is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with Foobar.
 * If not, see <https://www.gnu.org/licenses/lgpl-3.0.html>.
*/
#pragma once
#include "../framework.h"

namespace MDWMBlurGlassExt
{
	using namespace MDWMBlurGlass;

	constexpr auto g_hookFunList = std::to_array<std::pair<MHostModuleType, LPCSTR>>
	({
		{ dwmcore, "CRenderingTechnique::ExecuteBlur" },
		{ dwmcore, "CD2DContext::FillEffect" },
		{ dwmcore, "CCustomBlur::BuildEffect" },
		{ dwmcore, "CCustomBlur::DetermineOutputScale" },
		{ dwmcore, "CCustomBlur::Draw" },
		{ dwmcore, "CDrawingContext::DrawVisualTree" },
		{ dwmcore, "CRenderData::DrawImageResource_FillMode" },
		{ dwmcore, "CFilterEffect::CalcInversedWorldInputBoundsFromVisibleWorldOutputBoundsRecursive" },
		{ dwmcore, "CArrayBasedCoverageSet::AddAntiOccluderRect" },
		{ dwmcore, "CArrayBasedCoverageSet::IsCovered" },

		{ dwmcore, "CBrushRenderingGraph::RenderSubgraphs" },
		{ dwmcore, "CWindowNode::RenderImage" },
		{ dwmcore, "CWindowNode::GetHwnd" },

		{ dwmcore, "CChannel::DuplicateSharedResource" },
		{ dwmcore, "CChannel::MatrixTransformUpdate" },
		{ dwmcore, "CVisual::GetVisualTree" },
		{ dwmcore, "CVisual::GetBounds" },
		{ dwmcore, "CVisual::GetHwnd" },
		{ dwmcore, "CVisual::GetTopLevelWindow" },
		{ dwmcore, "CVisual::GetWindowBackgroundTreatmentInternal" },
		{ dwmcore, "CArrayBasedCoverageSet::Add" },
		{ dwmcore, "CZOrderedRect::UpdateDeviceRect" },
		{ dwmcore, "CResource::GetOwningProcessId" },
		{ dwmcore, "COcclusionContext::PostSubgraph" },
		{ dwmcore, "CDrawingContext::IsOccluded" },
		{ dwmcore, "CDrawingContext::GetCurrentVisual" },
		{ dwmcore, "CDrawingContext::GetClipBoundsWorld" },
		{ dwmcore, "CCustomBlur::Reset" },
		{ dwmcore, "GetCurrentFrameId" },
		{ dwmcore, "CBlurRenderingGraph::DeterminePreScale" },

		{ dwmcore, "CCommonRegistryData::m_backdropBlurCachingThrottleQPCTimeDelta" },

		{ udwm, "CDesktopManager::s_pDesktopManagerInstance" },
		{ udwm, "CDesktopManager::s_csDwmInstance" },
		{ udwm, "CDesktopManager::MonitorDpiFromPoint"},


		{ udwm, "CSolidColorLegacyMilBrushProxy::Update" },
		{ udwm, "CWindowList::GetExtendedFrameBounds" },
		{ udwm, "CWindowList::UpdateAccentBlurRect" },
		{ udwm, "CWindowList::GetSyncedWindowDataByHwnd" },
		{ udwm, "CWindowList::StyleChange" },
		{ udwm, "CWindowList::CloakChange" },
		{ udwm, "CWindowList::CheckForMaximizedChange" },
		{ udwm, "CWindowList::GetWindowListForDesktop" },
		{ udwm, "CWindowList::GetShellWindowForDesktop" },
		{ udwm, "ResourceHelper::CreateGeometryFromHRGN" },
		{ udwm, "?CreateRectangleGeometry@ResourceHelper@@SAJPEBUtagRECT@@PEAPEAVCRectangleGeometryProxy@@@Z" },
		{ udwm, "ResourceHelper::CreateCombinedGeometry" },
		{ udwm, "HrgnFromRects" },

		{ udwm, "CAccent::UpdateAccentPolicy" },
		{ udwm, "CAccent::UpdateAccentBlurRect" },
		{ udwm, "CAccent::s_IsPolicyActive" },

		{ udwm, "CTopLevelWindow::TreatAsActiveWindow" },
		{ udwm, "CTopLevelWindow::s_ChooseWindowFrameFromStyle" },
		{ udwm, "CTopLevelWindow::ValidateVisual" },
		{ udwm, "CTopLevelWindow::UpdateWindowVisuals" },
		{ udwm, "CTopLevelWindow::UpdateNCAreaButton" },
		{ udwm, "CTopLevelWindow::GetActualWindowRect" },
		{ udwm, "CTopLevelWindow::GetBorderMargins" },
		{ udwm, "CTopLevelWindow::UpdateText" },
		{ udwm, "CTopLevelWindow::InitializeVisualTreeClone" },
		{ udwm, "CTopLevelWindow::UpdateNCAreaBackground" },
		{ udwm, "CTopLevelWindow::OnAccentPolicyUpdated" },
		{ udwm, "CTopLevelWindow::OnClipUpdated" },
		{ udwm, "CTopLevelWindow::~CTopLevelWindow" },
		{ udwm, "CTopLevelWindow::UpdateClientBlur" },
		{ udwm, "CTopLevelWindow::UpdateAccent" },
		{ udwm, "CTopLevelWindow::CalculateBackgroundType" },
		{ udwm, "CTopLevelWindow::UpdateSystemBackdropVisual" },
		{ udwm, "CTopLevelWindow::CloneVisualTree" },
		{ udwm, "CTopLevelWindow::SetDirtyFlags" },
		{ udwm, "CTopLevelWindow::OnSystemBackdropUpdated" },
		{ udwm, "CTopLevelWindow::OnClipUpdated" },
		{ udwm, "CTopLevelWindow::OnBlurBehindUpdated" },
		{ udwm, "CTopLevelWindow::OnAccentPolicyUpdated" },
		{ udwm, "CTopLevelWindow::GetSolidColorCaptionColor" },
		{ udwm, "CTopLevelWindow::GetWindowColorizationColor" },
		{ udwm, "CTopLevelWindow::GetCurrentDefaultColorizationFlags" },
		{ udwm, "CTopLevelWindow::IsTrullyMinimized" },

		{ udwm, "CGlassColorizationParameters::AdjustWindowColorization" },

		{ udwm, "CText::SetColor" },
		{ udwm, "CText::SetText" },
		{ udwm, "CText::SetSize" },
		{ udwm, "CText::ValidateResources" },

		{ udwm, "CWindowList::BlurBehindChange" },
		{ udwm, "CWindowList::GetSyncedWindowData" },

		{ udwm, "CButton::UpdateLayout" },
		{ udwm, "CButton::RedrawVisual"},
		{ udwm, "CButton::~CButton" },

		{ udwm, "VisualCollection::Remove" },
		{ udwm, "VisualCollection::RemoveAll" },
		{ udwm, "VisualCollection::InsertRelative" },

		{ udwm, "CDesktopManager::HandleInteropDeviceLost" },

		{ udwm, "CVisualProxy::SetClip" },
		{ udwm, "CVisualProxy::SetSize" },

		{ udwm, "CVisual::Create" },
		{ udwm, "CVisual::SetInsetFromParent" },
		{ udwm, "CVisual::SetInsetFromParentRight" },
		{ udwm, "CVisual::SetInsetFromParentLeft" },
		{ udwm, "CVisual::SetSize" },
		{ udwm, "CVisual::SetOffset" },
		{ udwm, "CVisual::InitializeVisualTreeClone" },
		{ udwm, "CVisual::Unhide" },
		{ udwm, "CVisual::Hide" },
		{ udwm, "CVisual::ConnectToParent" },
		{ udwm, "CVisual::SetOpacity" },
		{ udwm, "CVisual::UpdateOpacity" },
		{ udwm, "CVisual::SetScale" },
		{ udwm, "CVisual::SendSetOpacity" },
		{ udwm, "CVisual::RenderRecursive" },
		{ udwm, "CVisual::SetDirtyChildren" },
		{ udwm, "CVisual::CreateFromSharedHandle" },
		{ udwm, "CVisual::InitializeFromSharedHandle" },
		{ udwm, "CVisual::MoveToFront" },
		{ udwm, "CVisual::Initialize" },
		{ udwm, "CVisual::WrapExistingResource" },

		{ udwm, "CContainerVisual" },
		{ udwm, "CContainerVisual::Create" },

		{ udwm, "CRenderDataVisual::AddInstruction" },
		{ udwm, "CRenderDataVisual::ClearInstructions" },
		{ udwm, "CDrawGeometryInstruction::Create" },
		{ udwm, "CCanvasVisual::Create" },

		{ udwm, "CWindowData::IsImmersiveWindow" },
		{ udwm, "CWindowData::IsWindowVisibleAndUncloaked" },

		{ udwm, "CMatrixTransformProxy::Update" },
		{ udwm, "CRgnGeometryProxy::Update" },

		// Win11 post-composition pass (Section/D3DPresentHook.cpp). DWM presents
		// through its own private stack - IDXGISwapChainDWM1, not IDXGISwapChain -
		// so there is no public vtable to patch. PrePresent runs once per frame
		// after composition is complete and before the present goes out;
		// CreateBackBuffer hands us the composed ID3D11Texture2D to work on.
		// NOTE: entries are matched to symbol offsets by INDEX, so only ever
		// append here, and rebuild DWMBlurGlassHost as well as the extension.
		{ dwmcore, "CLegacySwapChain::PrePresent" },
		{ dwmcore, "CLegacySwapChain::CreateBackBuffer" },
		{ dwmcore, "CDDisplaySwapChain::PrePresent" },

		// From PrePresent, GetBackBuffer yields an IDeviceTarget which is
		// concretely a CDeviceTextureTarget - and that hands out the composed
		// desktop as a plain ID3D11Texture2D plus ready-made RTV and SRV, which
		// is everything the post-composition pass needs. GetTexture2D and
		// GetShaderResourceView each have an adjustor-thunk twin, so those two
		// are named in decorated form to pick the primary out unambiguously.
		{ dwmcore, "CDDisplaySwapChain::GetBackBuffer" },
		{ dwmcore, "?GetTexture2D@CDeviceTextureTarget@@UEAAPEAUID3D11Texture2D@@XZ" },
		{ dwmcore, "CDeviceTextureTarget::GetRenderTargetView" },
		{ dwmcore, "?GetShaderResourceView@CDeviceTextureTarget@@UEAAPEAUID3D11ShaderResourceView@@XZ" },
		{ dwmcore, "CDDisplaySwapChainBuffer::GetD3D11Resource" },
		// dwmcore is linked with COMDAT folding, so a one-line getter can share
		// an address with an unrelated one-line getter of the same shape. That
		// makes "is this function in the object's vtable" an unreliable type
		// check, so these accessors are HOOKED and their real return values
		// observed, rather than called speculatively on a guessed `this`.
		{ dwmcore, "CDrawingContext::BeginFrame" },

		// DWM composes back to front, so at the instant this is called the frame
		// target already holds everything underneath this window - other windows,
		// desktop icons, wallpaper. Capturing there and refracting after the
		// window has drawn is what lets the rim show what is actually behind the
		// glass, which a post-composition pass structurally cannot see.
		{ dwmcore, "CWindowNode::RenderContent" },

		// The edge pinch's per-frame anchor. CDDisplaySwapChain::PrePresent is
		// gone from dwmcore as of 10.0.26100.9278, so the pass now runs inside
		// the render target's present instead: Present(this) checks
		// PresentNeeded, then flips through COverlayContext::Present, which asks
		// CDDisplaySwapChainBuffer::GetD3D11Resource for the buffer it is about
		// to put on screen - the moment the pass draws into it.
		// (Takes only `this`; checked against the disassembly of that build.)
		{ dwmcore, "CDDisplayRenderTarget::Present" },
	});
}