Texture2D<float2> CurrentMotion : register(t0);
Texture2D<float> CurrentDepth : register(t1);
RWTexture2D<float2> SameEyeMotion : register(u0);

cbuffer Composition : register(b0)
{
    uint2 SourceBase;
    uint2 CropOrigin;
    float2 CurrentForwardScale;
    float2 WindowExtent;
    float4 TransformRow0;
    float4 TransformRow1;
    float4 TransformRow2;
    float4 TransformRow3;
    float4 MiddleSameRow0;
    float4 MiddleSameRow1;
    float4 MiddleSameRow2;
    float4 MiddleSameRow3;
    float4 PreviousSameRow0;
    float4 PreviousSameRow1;
    float4 PreviousSameRow2;
    float4 PreviousSameRow3;
};

float2 ProjectUv(float4 clip)
{
    float4 mapped = clip.x * TransformRow0 +
        clip.y * TransformRow1 +
        clip.z * TransformRow2 +
        clip.w * TransformRow3;
    return float2(mapped.x, -mapped.y) / mapped.w * 0.5 + 0.5;
}

float2 ProjectMiddleSameUv(float4 clip)
{
    float4 mapped = clip.x * MiddleSameRow0 +
        clip.y * MiddleSameRow1 +
        clip.z * MiddleSameRow2 +
        clip.w * MiddleSameRow3;
    return float2(mapped.x, -mapped.y) / mapped.w * 0.5 + 0.5;
}

float2 ProjectPreviousSameUv(float4 clip)
{
    float4 mapped = clip.x * PreviousSameRow0 +
        clip.y * PreviousSameRow1 +
        clip.z * PreviousSameRow2 +
        clip.w * PreviousSameRow3;
    return float2(mapped.x, -mapped.y) / mapped.w * 0.5 + 0.5;
}

[numthreads(8, 8, 1)]
void ComposeLukeSameEyeMotion(uint3 thread_id : SV_DispatchThreadID)
{
    uint width, height;
    SameEyeMotion.GetDimensions(width, height);
    if (thread_id.x >= width || thread_id.y >= height) return;

    uint2 logical = CropOrigin + thread_id.xy;
    float2 raw = CurrentMotion.Load(int3(SourceBase + logical, 0));
    float depth = CurrentDepth.Load(int3(thread_id.xy, 0));
    float2 uv = (float2(thread_id.xy) + 0.5) / WindowExtent;
    float4 clip = float4(uv.x * 2.0 - 1.0,
        1.0 - uv.y * 2.0, depth, 1.0);
    float2 at_depth = ProjectUv(clip);
    float4 far_clip = clip;
    far_clip.z = 0.0;
    float2 at_infinity = ProjectUv(far_clip);
    float2 stereo_disparity = (at_depth - at_infinity) * WindowExtent;
    // Raw AER points at the preceding opposite eye. Remove only its
    // depth-dependent eye separation. Its residual spans one AER interval,
    // while the same-eye AFW history spans two. Add the missing earlier
    // interval from the actual camera history, without sampling the previous
    // opposite eye's contaminated motion field.
    float2 middle_same = ProjectMiddleSameUv(clip);
    float2 previous_same = ProjectPreviousSameUv(clip);
    float2 previous_camera_step =
        (previous_same - middle_same) * WindowExtent;
    float2 corrected = raw +
        (stereo_disparity - previous_camera_step) / CurrentForwardScale;
    SameEyeMotion[thread_id.xy] = all(isfinite(corrected))
        ? corrected : float2(0.0, 0.0);
}
