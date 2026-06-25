// ─── Fullscreen Quad Vertex Shader for Stereo Reprojection ──────────────────
// This shader samples the game's back buffer and projects it into the
// VR view for each eye, applying lens distortion correction.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct PSInput {
    float2 uv : TEXCOORD0;
    float4 position : SV_POSITION;
};

// ─── Vertex Shader ──────────────────────────────────────────────────────────

VSOutput vs_main(uint vertex_id : SV_VertexID) {
    VSOutput output;

    // Fullscreen triangle (no index buffer needed)
    float x = float(vertex_id & 1) * 2.0f - 1.0f;
    float y = float((vertex_id >> 1) & 1) * 2.0f - 1.0f;
    output.position = float4(x, -y, 0.0f, 1.0f);
    output.uv = float2((x + 1.0f) * 0.5f, (-y + 1.0f) * 0.5f);

    return output;
}

// ─── Pixel Shader: Stereo Reprojection ──────────────────────────────────────

Texture2D<float4> game_frame : register(t0);
SamplerState linear_sampler : register(s0);

cbuffer StereoParams : register(b0) {
    float4x4 eye_projection;      // Eye projection matrix
    float4x4 eye_view;            // Eye view matrix (inverse)
    float4x4 game_projection;     // Original game projection matrix
    float4x4 game_view;           // Original game view matrix (inverse)
    float4  hmd_warp_params;      // Lens distortion: {k1, k2, scale, center_offset}
    float2  render_target_size;   // Width, height of eye render target
    float   eye_aspect;           // Aspect ratio for the eye
    float   padding;
};

// Lens distortion correction (barrel distortion inverse)
float2 apply_lens_distortion(float2 uv, float4 params) {
    // params = {k1, k2, scale, center_offset}
    float2 uv_centered = uv - 0.5f;
    float r2 = dot(uv_centered, uv_centered);
    float r4 = r2 * r2;

    // Barrel distortion: uv * (1 + k1*r^2 + k2*r^4)
    float distortion = 1.0f + params.x * r2 + params.y * r4;
    float2 distorted = uv_centered * distortion;

    // Scale correction
    distorted *= params.z;

    return distorted + 0.5f;
}

// Chromatic aberration correction
float3 apply_chromatic_aberration(float2 uv, float2 center,
                                  float4 params, Texture2D<float4> frame,
                                  SamplerState samp)
{
    float2 offset = uv - center;
    float r2 = dot(offset, offset);

    // Slight offset per channel
    float ca_scale = 0.003f;
    float r = frame.SampleLevel(samp, uv + offset * (1.0f + ca_scale), 0).r;
    float g = frame.SampleLevel(samp, uv, 0).g;
    float b = frame.SampleLevel(samp, uv + offset * (1.0f - ca_scale), 0).b;

    return float3(r, g, b);
}

float4 ps_main(PSInput input) : SV_TARGET {
    // Apply lens distortion to UV coordinates
    float2 distorted_uv = apply_lens_distortion(input.uv, hmd_warp_params);

    // Clamp to avoid edge artifacts
    distorted_uv = clamp(distorted_uv, 0.0f, 1.0f);

    // Apply chromatic aberration correction
    float3 color = apply_chromatic_aberration(
        distorted_uv, float2(0.5f, 0.5f), hmd_warp_params,
        game_frame, linear_sampler);

    // For stereo reprojection, the game frame is sampled with a
    // projective mapping that accounts for the difference between
    // the original game camera and the VR eye camera.
    //
    // In a full implementation, this would reconstruct the world
    // position from the game depth buffer and reproject it into
    // the VR eye view. For MVP, we use a simple 2D mapping.

    return float4(color, 1.0f);
}

// ─── Barrel Distortion Post-Process ─────────────────────────────────────────

float4 barrel_distortion_ps(PSInput input) : SV_TARGET {
    float2 uv = input.uv;

    // Convert to centered coords
    float2 uv_centered = uv - 0.5f;
    float r2 = dot(uv_centered, uv_centered);

    // Barrel distortion (for VR lens correction)
    float k1 = 0.3f;  // Will be calibrated per HMD
    float k2 = 0.1f;
    float distortion = 1.0f + k1 * r2 + k2 * r2 * r2;

    float2 distorted = uv_centered * distortion + 0.5f;

    if (distorted.x < 0.0f || distorted.x > 1.0f ||
        distorted.y < 0.0f || distorted.y > 1.0f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    return game_frame.SampleLevel(linear_sampler, distorted, 0);
}
