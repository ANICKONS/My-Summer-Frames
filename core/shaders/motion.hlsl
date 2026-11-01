// My Summer Frames: motion vectors for DLSS from the depth buffer and the camera matrices (Unity 5.0 has no motion vectors).
// Copyright (C) 2026 ANICKON
//
// Texture space as stored (row 0 at the top of the texture memory). For every pixel: the world point under it
// (current jittered projection, camera-relative coordinates). A point inside the box of a moving body (the car the
// player sits in, a passing car) moved with that body since the last frame; any other point stands still. Both
// positions are projected with the unjittered view-projections; the vector points from the current position to the
// previous one, in pixels.

cbuffer Frame : register(b0)
{
    float4x4 InvViewProjJittered; // current clip (jittered) -> camera-relative world
    float4x4 ViewProj;            // camera-relative world -> current clip (no jitter)
    float4x4 PrevViewProj;        // camera-relative world (current camera position) -> previous clip (no jitter)
    float2 Size;                  // render target size in pixels
    float2 MvSign;                // test switch: +1 / -1 per axis
    float4 Options;               // x: 1 = flip rows for frame generation (render texture upside down)
};

#define MAX_BODIES 16
struct Body
{
    float4x4 ToLocal;     // camera-relative world -> body local (now)
    float4x4 PrevToWorld; // body local -> camera-relative world (last frame, current camera position)
    float4 Center;        // local box centre; w = box volume (the smallest box wins)
    float4 Extents;       // local box half size
};

cbuffer Bodies : register(b1)
{
    uint BodyCount;
    uint3 BodyPad;
    Body Bodies[MAX_BODIES];
};

Texture2D<float> Depth : register(t0);
RWTexture2D<float2> Motion : register(u0);

float2 MotionOf(uint2 p)
{
    float d = Depth.Load(int3(p, 0));
    float2 uv = (float2(p) + 0.5) / Size;
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    float4 world = mul(InvViewProjJittered, clip);
    world /= world.w;
    float4 prevWorld = world;
    float best = 3.4e38;
    for (uint i = 0; i < BodyCount; i++)
    {
        float3 l = mul(Bodies[i].ToLocal, world).xyz;
        if (all(abs(l - Bodies[i].Center.xyz) <= Bodies[i].Extents.xyz) && Bodies[i].Center.w < best)
        {
            best = Bodies[i].Center.w;
            prevWorld = mul(Bodies[i].PrevToWorld, float4(l, 1.0));
        }
    }
    float4 cur = mul(ViewProj, world);
    float4 prev = mul(PrevViewProj, prevWorld);
    float2 delta = prev.xy / prev.w - cur.xy / cur.w;
    return delta * float2(0.5, -0.5) * Size * MvSign;
}

[numthreads(8, 8, 1)]
void MotionCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    Motion[id.xy] = MotionOf(id.xy);
}

// Reprojection test: the previous colour fetched along the motion vectors against the current colour, and the same
// without motion vectors. Right vectors make the first error clearly smaller than the second while the camera moves.
Texture2D<float4> Current : register(t1);
Texture2D<float4> Previous : register(t2);
Texture2D<float2> MotionIn : register(t3);
SamplerState Linear : register(s0);
// 0 warped error, 4 zero-motion error, 8 pixels, 12 |mv.x|, 16 |mv.y|, 20 outside, 24 depth 0, 28 depth 1
RWByteAddressBuffer Sums : register(u1);

[numthreads(8, 8, 1)]
void ErrorCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    float d = Depth.Load(int3(id.xy, 0));
    if (d <= 0.0) Sums.InterlockedAdd(24, 1);
    if (d >= 1.0) Sums.InterlockedAdd(28, 1);
    float2 mv = MotionIn.Load(int3(id.xy, 0));
    float2 prevUv = (float2(id.xy) + 0.5 + mv) / Size;
    if (any(prevUv < 0.0) || any(prevUv > 1.0))
    {
        Sums.InterlockedAdd(20, 1);
        return;
    }
    float3 c = saturate(Current.Load(int3(id.xy, 0)).rgb);
    float3 w = saturate(Previous.SampleLevel(Linear, prevUv, 0).rgb);
    float3 z = saturate(Previous.Load(int3(id.xy, 0)).rgb);
    // 8-bit steps, so the sums stay below 2^32 up to 4K.
    uint ew = (uint)(dot(abs(c - w), 1.0 / 3.0) * 255.0 + 0.5);
    uint ez = (uint)(dot(abs(c - z), 1.0 / 3.0) * 255.0 + 0.5);
    Sums.InterlockedAdd(0, ew);
    Sums.InterlockedAdd(4, ez);
    Sums.InterlockedAdd(8, 1);
    Sums.InterlockedAdd(12, (uint)min(abs(mv.x) * 4.0, 400.0));
    Sums.InterlockedAdd(16, (uint)min(abs(mv.y) * 4.0, 400.0));
}

// Frame generation works on the back buffer: depth and motion in its orientation. Unity's render textures on
// Direct3D are stored upside down, so rows are flipped and the vertical motion changes sign.
// The player camera's finished picture is also the HUD-less colour (the hand camera, the HUD and the UI come after
// it): frame generation then knows which pixels are screen-fixed.
RWTexture2D<float> FgDepth : register(u2);
RWTexture2D<float2> FgMotion : register(u3);
Texture2D<float4> SceneColor : register(t5);
RWTexture2D<unorm float4> FgHudless : register(u4);

// Depth and motion at the render size (Size = render size).
[numthreads(8, 8, 1)]
void FlipCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    bool flip = Options.x > 0.5;
    uint2 src = flip ? uint2(id.x, (uint)Size.y - 1 - id.y) : id.xy;
    FgDepth[id.xy] = Depth.Load(int3(src, 0));
    float2 mv = MotionIn.Load(int3(src, 0));
    FgMotion[id.xy] = flip ? float2(mv.x, -mv.y) : mv;
}

// The HUD-less picture at the screen size (Size = DLSS output size).
[numthreads(8, 8, 1)]
void HudlessCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    bool flip = Options.x > 0.5;
    uint2 src = flip ? uint2(id.x, (uint)Size.y - 1 - id.y) : id.xy;
    FgHudless[id.xy] = SceneColor.Load(int3(src, 0));
}

// Sharpening after DLSS (the slider): AMD FidelityFX RCAS from FSR 1 (MIT), the filter FSR's own sharpening uses, so
// the slider means the same for both upscalers. Size = output size, Options.y = strength (exp2(-stops), 0.25 .. 1).
Texture2D<float4> SharpenIn : register(t6);
RWTexture2D<unorm float4> SharpenOut : register(u5);

[numthreads(8, 8, 1)]
void SharpenCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    int2 p = int2(id.xy);
    int2 last = int2(Size) - 1;
    float3 b = SharpenIn.Load(int3(clamp(p + int2(0, -1), int2(0, 0), last), 0)).rgb;
    float3 d = SharpenIn.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), last), 0)).rgb;
    float4 e4 = SharpenIn.Load(int3(p, 0));
    float3 e = e4.rgb;
    float3 f = SharpenIn.Load(int3(clamp(p + int2(1, 0), int2(0, 0), last), 0)).rgb;
    float3 h = SharpenIn.Load(int3(clamp(p + int2(0, 1), int2(0, 0), last), 0)).rgb;
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    // The strongest negative lobe that keeps the result inside the neighbourhood's range (no ringing).
    float3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 1e-5);
    float3 lobe3 = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * Options.y;
    float3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    SharpenOut[id.xy] = float4(saturate(c), e4.a);
}
