cbuffer CinemaPanelParameters : register(b0) {
    float4 clip_top_left;
    float4 clip_top_right;
    float4 clip_bottom_left;
    float4 clip_bottom_right;
    float4 source_rect;
    uint source_width;
    uint source_height;
    uint source_slice;
    uint decode_generated_srgb;
};

Texture2DArray<float4> generated_texture : register(t0);
SamplerState linear_sampler : register(s0);

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput CinemaPanelVS(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    if (vertex_id == 0) {
        output.position = clip_top_left;
        output.uv = float2(0.0, 0.0);
    } else if (vertex_id == 1 || vertex_id == 4) {
        output.position = clip_top_right;
        output.uv = float2(1.0, 0.0);
    } else if (vertex_id == 2 || vertex_id == 3) {
        output.position = clip_bottom_left;
        output.uv = float2(0.0, 1.0);
    } else {
        output.position = clip_bottom_right;
        output.uv = float2(1.0, 1.0);
    }
    return output;
}

float4 CinemaPanelPS(VertexOutput input) : SV_Target0 {
    const float2 dimensions = float2(source_width, source_height);
    const float2 pixel = source_rect.xy + saturate(input.uv) * source_rect.zw;
    const float2 uv = pixel / dimensions;
    float4 color = generated_texture.SampleLevel(
        linear_sampler, float3(uv, source_slice), 0.0);
    if (decode_generated_srgb != 0) {
        // Witcher supplies display-encoded flattened colour to FSR.
        const float3 low = color.rgb / 12.92;
        const float3 high = pow(max((color.rgb + 0.055) / 1.055, 0.0), 2.4);
        color.rgb = lerp(high, low, step(color.rgb, 0.04045));
    }
    color.a = 1.0;
    return color;
}

VertexOutput CinemaFlattenVS(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

// Pull back the *displayed* image into the original 2D guide coordinates.
// This is evaluation of the forward homogeneous panel map at each flat pixel,
// not an approximation based on headset FOV or a rescaling of the vectors.
float4 CinemaFlattenPS(VertexOutput input) : SV_Target0 {
    float2 uv = saturate(input.uv);
    float4 clip = lerp(lerp(clip_top_left, clip_top_right, uv.x),
                       lerp(clip_bottom_left, clip_bottom_right, uv.x), uv.y);
    if (!all(isfinite(clip)) || clip.w <= 1e-6) return float4(0, 0, 0, 1);
    float2 xr_uv = clip.xy / clip.w * float2(.5, -.5) + .5;
    if (any(xr_uv < 0) || any(xr_uv > 1)) return float4(0, 0, 0, 1);
    // Sample using the negotiated XR view (linear-light result), then encode
    // for FSR's explicit SRGB input contract. PanelPS decodes the result once.
    float4 color = generated_texture.SampleLevel(linear_sampler, float3(xr_uv, source_slice), 0);
    color.rgb = lerp(1.055 * pow(max(color.rgb, 0.0), 1.0 / 2.4) - 0.055,
                     12.92 * color.rgb, step(color.rgb, 0.0031308));
    color.a = 1;
    return color;
}
