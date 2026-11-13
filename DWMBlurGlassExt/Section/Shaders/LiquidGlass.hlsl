// LiquidGlass.hlsl
//
// The edge "pinch" of the Win11 Liquid Glass material. It runs once per frame
// on the finished, composed frame, just before DWM presents it (see
// Section/D3DPresentHook.cpp), over a band `rim` px wide just inside the edges
// of each glass region.
//
// Into that band it compacts the surroundings: the strip of desktop `reach` px
// wide lying just OUTSIDE the edge is squeezed into the `rim` px just inside
// it, most strongly at the very edge - which is how the curved edge of a thick
// pane of glass gathers light from around it. The interior of the pane is left
// to the Composition material underneath (blurred, shaded, tinted).
//
// Every sample is taken from outside the glass region, never from the band this
// pass writes. DWM only re-composes damaged regions, so a buffer can come back
// still carrying the band drawn into it on an earlier frame; because the band
// is never read, redrawing over it gives the same answer every time.
//
// DWM composes in R16G16B16A16_FLOAT scRGB - linear, 1.0 = SDR white - or in
// B8G8R8A8 when nothing is HDR. Either way nothing here decodes or encodes
// gamma: samples go back out exactly as they came in.
//
// Compile with:
//   fxc /nologo /T vs_5_0 /E VSMain /O3 /Fh LiquidGlassVS.h /Vn g_liquidGlassVS LiquidGlass.hlsl
//   fxc /nologo /T ps_5_0 /E PSMain /O3 /Fh LiquidGlassPS.h /Vn g_liquidGlassPS LiquidGlass.hlsl

cbuffer Params : register(b0)
{
    float4 quad;        // area to rasterise, target px (the glass rect, clipped)
    float4 rect;        // the glass rect, target px; edges that must not get a
                        // rim are pushed far outside so they never come nearest
    float2 invTarget;   // 1 / target size
    float  rim;         // width of the band written, px
    float  cornerRadius;

    float  reach;       // width of the outside strip compacted into it, px
    float  gamma;       // > 1 concentrates the compaction at the very edge
    float  chromatic;   // per-channel spread of the reach, 0..1
    float  highlight;   // specular line strength, linear light

    float2 lightDir;    // unit vector pointing TOWARD the light
    float  fade;        // 0..1, eases the rim in once the window settles
    float  debug;       // != 0 floods the band with magenta

    float  occluderCount;
    float3 pad0;
    // Windows stacked above this one, target px: the pass runs on the finished
    // frame, so without these it would paint across whatever covers the glass.
    float4 occluders[8];
};

Texture2D<float4> Frame : register(t0);
SamplerState LinearClamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 px  : TEXCOORD0;   // position in target pixels
};

// Four vertices as a triangle strip covering `quad`. No vertex or index buffer
// is bound - one less piece of DWM's pipeline state to disturb.
VSOut VSMain(uint vid : SV_VertexID)
{
    float2 corner = float2(vid & 1, (vid >> 1) & 1);
    float2 px = lerp(quad.xy, quad.zw, corner);

    VSOut o;
    o.pos = float4(px * invTarget * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.px = px;
    return o;
}

// Signed distance to a rounded rectangle: negative inside, zero on the edge.
float sdRoundedBox(float2 p, float2 halfSize, float radius)
{
    float2 q = abs(p) - halfSize + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 center = (rect.xy + rect.zw) * 0.5;
    float2 halfSize = (rect.zw - rect.xy) * 0.5;
    float2 p = i.px - center;

    float sd = sdRoundedBox(p, halfSize, cornerRadius);
    float band = max(rim, 1.0);

    // Outside the glass, or deeper in than the band: leave the pixel exactly as
    // DWM composed it.
    if (sd > 0.0 || sd < -band)
        discard;

    // Anything stacked above this window owns these pixels.
    int count = (int)occluderCount;
    [loop]
    for (int k = 0; k < count; ++k)
    {
        float4 o = occluders[k];
        if (i.px.x >= o.x && i.px.x < o.z && i.px.y >= o.y && i.px.y < o.w)
            discard;
    }

    if (debug > 0.5)
        return float4(1.0, 0.0, 1.0, 1.0);

    // Outward normal off the SDF gradient - exact along the edges and round
    // the corners alike.
    float2 e = float2(0.5, 0.0);
    float2 grad = float2(
        sdRoundedBox(p + e.xy, halfSize, cornerRadius) - sdRoundedBox(p - e.xy, halfSize, cornerRadius),
        sdRoundedBox(p + e.yx, halfSize, cornerRadius) - sdRoundedBox(p - e.yx, halfSize, cornerRadius));
    float2 n = grad / max(length(grad), 1e-5);

    float depth = -sd;              // 0 at the edge .. band at the inner side
    float t = saturate(depth / band);

    // Where this pixel gathers from. Step out to the edge (plus a hair, so the
    // sample is never inside the band), then a further distance that shrinks
    // to nothing at the inner side of the band: the full `reach`-wide strip
    // outside lands, compacted, in the `band` px inside.
    float2 edge = i.px + n * (depth + 0.75);
    float outward = reach * pow(1.0 - t, gamma);

    float3 col;
    col.r = Frame.SampleLevel(LinearClamp, (edge + n * outward * (1.0 + chromatic)) * invTarget, 0).r;
    col.g = Frame.SampleLevel(LinearClamp, (edge + n * outward) * invTarget, 0).g;
    col.b = Frame.SampleLevel(LinearClamp, (edge + n * outward * (1.0 - chromatic)) * invTarget, 0).b;

    // A crisp specular line where the edge catches the light, strongest on the
    // edges facing it; the far edges keep a little of it, reflected back
    // through the thickness of the glass.
    float facing = saturate(dot(n, lightDir));
    float line_ = exp(-depth * depth / 1.6);
    col += highlight * line_ * (0.3 + 0.7 * facing);

    // Solid over the outer part of the band, easing into the glass beneath by
    // its inner side, so there is no seam where the band stops.
    float coverage = (1.0 - smoothstep(0.5, 1.0, t)) * saturate(fade);

    // Premultiplied; the blender lays it over what DWM composed.
    return float4(col * coverage, coverage);
}
