// LiquidGlass.hlsl
//
// Renders the edge "pinch" of one Liquid Glass titlebar into a small texture of
// its own. That texture is handed to Composition and composited inside the
// window's glass material - under its tint, gradient, title and buttons - so
// this shader never touches the frame DWM puts on screen; it only reads it.
//
// Into a band `rim` px wide along the titlebar's edges it mirrors the
// surroundings just OUTSIDE each edge, mirrored about it - what is right
// outside shows right inside, what is further out shows further in - scaled
// like the rim of a lens: a little squeezed at the edge (stretchStart),
// magnified by the inner side (stretchEnd) - which is how the curved edge
// of a thick pane of glass gathers light from
// around it. Everywhere else the texture is transparent, so the blurred pane
// shows through.
//
// Samples come only from outside the titlebar, never from the window itself.
//
// DWM composes in R16G16B16A16_FLOAT scRGB - linear, 1.0 = SDR white - or in
// B8G8R8A8 when nothing is HDR. The band texture is 8-bit and Composition reads
// it as sRGB, so linear frames are encoded on the way out.
//
// Compile with:
//   fxc /nologo /T vs_5_0 /E VSMain /O3 /Fh LiquidGlassVS.h /Vn g_liquidGlassVS LiquidGlass.hlsl
//   fxc /nologo /T ps_5_0 /E PSMain /O3 /Fh LiquidGlassPS.h /Vn g_liquidGlassPS LiquidGlass.hlsl

cbuffer Params : register(b0)
{
    float4 quad;        // area to rasterise, band-texture px
    float4 rect;        // the titlebar, screen px; edges that get no rim are
                        // pushed far outside so they never come nearest
    float2 invTarget;   // 1 / band texture size
    float  rim;         // width of the band, px
    float  cornerRadius;

    float  stretchStart; // reflection scale at the rim (0.6 = squeezed)
    float  stretchEnd;   // reflection scale at the band's inner side (2 = doubled)
    float  chromatic;   // per-channel spread of how far out it gathers, 0..1
    float  highlight;   // how much brighter the reflection gets toward the rim

    float2 lightDir;    // unit vector pointing TOWARD the light
    float  encodeSrgb;  // != 0: frame is linear, encode to sRGB
    float  debug;       // != 0 floods the band with magenta

    float2 origin;      // screen px of the band texture's (0,0)
    float2 invFrame;    // 1 / composed frame size

    float  occluderCount;
    float  fade;        // share of the band, from its inner end, it fades in over (0..1]
    float2 pad0;
    float4 occluders[8];
};

Texture2D<float4> Frame : register(t0);
SamplerState LinearClamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 px  : TEXCOORD0;   // position in band-texture pixels
};

// Four vertices as a triangle strip covering `quad`. No vertex or index buffer.
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

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 px = i.px + origin;   // screen position
    float2 center = (rect.xy + rect.zw) * 0.5;
    float2 halfSize = (rect.zw - rect.xy) * 0.5;
    float2 p = px - center;

    float sd = sdRoundedBox(p, halfSize, cornerRadius);
    float band = max(rim, 1.0);

    // Outside the glass or deeper in than the band: transparent, so the
    // blurred pane underneath shows.
    if (sd > 0.0 || sd < -band)
        return float4(0.0, 0.0, 0.0, 0.0);

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

    // The band is the edge of a rounded pane of glass: steep at the rim,
    // flattening to the pane by its inner side. `steep` is 1 at the rim and 0
    // where the surface has gone flat.
    float steep = 1.0 - t;

    // Where this pixel gathers from: step out to the edge (plus a hair, so the
    // sample is never the titlebar itself), then further out the deeper into
    // the band this pixel is - the edge acts as a mirror, so what lies just
    // outside appears just inside, and something approaching the window meets
    // its own image at the edge. Like the rim of a lens, the reflection is
    // scaled by `stretch`, which runs from stretchStart at the rim to
    // stretchEnd at the inner side: a little squeezed at the edge, opening out
    // to magnified as the surface flattens. `outward` integrates 1 / stretch
    // over the depth, so each band pixel shows exactly that scale.
    float2 edge = px + n * (depth + 0.75);
    float a = stretchStart;
    float b = stretchEnd;
    float outward = abs(b - a) < 1e-3
        ? depth / a
        : band / (b - a) * log((a + (b - a) * t) / a);

    // Sharp at the rim, increasingly blurred toward the inner side, so the
    // reflection dissolves into the blurred pane rather than ending on a line.
    // Past the screen border the clamp sampler repeats the last row there is.
    float2 tangent = float2(-n.y, n.x);
    // Never quite sharp, even at the rim: a pixel or so of softness keeps it
    // reading as light on glass rather than a cut-out picture.
    float blur = max(1.0, 0.6 * band * t * t);
    float2 d1 = tangent * blur;
    float2 d2 = n * blur * 0.5;

    float2 pr = edge + n * outward * (1.0 + chromatic);
    float2 pg = edge + n * outward;
    float2 pb = edge + n * outward * (1.0 - chromatic);
    float3 col;
    col.r = 0.4 * Frame.SampleLevel(LinearClamp, pr * invFrame, 0).r
          + 0.15 * (Frame.SampleLevel(LinearClamp, (pr + d1) * invFrame, 0).r + Frame.SampleLevel(LinearClamp, (pr - d1) * invFrame, 0).r
                  + Frame.SampleLevel(LinearClamp, (pr + d2) * invFrame, 0).r + Frame.SampleLevel(LinearClamp, (pr - d2) * invFrame, 0).r);
    col.g = 0.4 * Frame.SampleLevel(LinearClamp, pg * invFrame, 0).g
          + 0.15 * (Frame.SampleLevel(LinearClamp, (pg + d1) * invFrame, 0).g + Frame.SampleLevel(LinearClamp, (pg - d1) * invFrame, 0).g
                  + Frame.SampleLevel(LinearClamp, (pg + d2) * invFrame, 0).g + Frame.SampleLevel(LinearClamp, (pg - d2) * invFrame, 0).g);
    col.b = 0.4 * Frame.SampleLevel(LinearClamp, pb * invFrame, 0).b
          + 0.15 * (Frame.SampleLevel(LinearClamp, (pb + d1) * invFrame, 0).b + Frame.SampleLevel(LinearClamp, (pb - d1) * invFrame, 0).b
                  + Frame.SampleLevel(LinearClamp, (pb + d2) * invFrame, 0).b + Frame.SampleLevel(LinearClamp, (pb - d2) * invFrame, 0).b);

    // Glass reflects more the more glancing the angle, so the reflection
    // brightens toward the steep rim. It is the reflected image that gets
    // brighter - no line is drawn on top of it.
    col *= 1.0 + highlight * steep * steep;

    if (encodeSrgb > 0.5)
        col = LinearToSrgb(col);
    else
        col = saturate(col);

    // Faint where the band meets the titlebar's blur, building up to full
    // strength over `fade` of the band's width (from its inner end; the Fade
    // distance setting) and full from there to the rim. Starts from zero with
    // zero slope, so there is no seam on the inner side.
    float fadeIn = saturate(steep / max(fade, 0.01));
    float coverage = fadeIn * fadeIn;

    // A side that is not a rim edge (the titlebar's bottom, where it meets the
    // client area) has its edge pushed far out in `rect`; the bands running
    // into it fade out over a few pixels instead of stopping on a hard line.
    const float runOut = 10.0;
    float2 lo = origin + quad.xy;
    float2 hi = origin + quad.zw;
    if (rect.x < lo.x - 1.0) coverage *= saturate((px.x - lo.x) / runOut);
    if (rect.y < lo.y - 1.0) coverage *= saturate((px.y - lo.y) / runOut);
    if (rect.z > hi.x + 1.0) coverage *= saturate((hi.x - px.x) / runOut);
    if (rect.w > hi.y + 1.0) coverage *= saturate((hi.y - px.y) / runOut);

    // Premultiplied, as Composition expects.
    return float4(col * coverage, coverage);
}
