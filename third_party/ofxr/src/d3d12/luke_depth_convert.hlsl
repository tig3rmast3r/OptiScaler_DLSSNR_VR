Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> ConvertedDepth : register(u0);
cbuffer DepthDilation : register(b0)
{
    uint DepthInverted;
    uint DilateMode;
};

[numthreads(8, 8, 1)]
void ConvertLukeDepth(uint3 thread_id : SV_DispatchThreadID)
{
    uint width;
    uint height;
    ConvertedDepth.GetDimensions(width, height);
    if (thread_id.x >= width || thread_id.y >= height)
        return;
    if (DilateMode == 0) {
        ConvertedDepth[thread_id.xy] = SourceDepth.Load(
            int3(thread_id.xy, 0));
        return;
    }
    // One guide pixel horizontally, optionally one vertically as well.
    // Horizon's captured depth is inverted (near = larger).
    float nearest = DepthInverted != 0 ? 0.0 : 1.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        if (DilateMode == 1 && y != 0) continue;
        int sample_y = clamp(int(thread_id.y) + y, 0, int(height) - 1);
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            int sample_x = clamp(int(thread_id.x) + x, 0, int(width) - 1);
            float candidate = SourceDepth.Load(int3(sample_x, sample_y, 0));
            nearest = DepthInverted != 0
                ? max(nearest, candidate) : min(nearest, candidate);
        }
    }
    ConvertedDepth[thread_id.xy] = nearest;
}
