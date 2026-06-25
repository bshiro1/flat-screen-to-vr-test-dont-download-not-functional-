// ─── Depth-Based 3D Reprojection Shader for VR ─────────────────────────────
// 
// Reconstructs world positions from the game's depth buffer and reprojects
// them into each eye's VR view. This eliminates the "cardboard" 2D feel
// and provides correct stereo parallax with full 6DoF head movement.
//
// Inputs:
//   t0: Game color buffer (RGBA8 UNORM SRGB)
//   t1: Linear depth buffer (R32 FLOAT)
//   cb0: Reprojection constants

Texture2D<float4> game_color : register(t0);
Texture2D<float>  linear_depth : register(t1);
SamplerState      point_sampler : register(s0);
SamplerState      linear_sampler : register(s1);

cbuffer ReprojectionCB : register(b0) {
    float4x4 inverse_game_view_proj;  // (View * Proj)^-1 for game camera
    float4x4 eye_view_proj_left;      // Left eye View * Proj
    float4x4 eye_view_proj_right;     // Right eye View * Proj
    float4x4 eye_view_proj;           // Current eye's VP (set per-pass)
    float2   depth_range;             // near, far
    float2   render_target_size;      // width, height
    float    eye_index;               // 0 = left, 1 = right
    float    padding;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// ─── Fullscreen Triangle Vertex Shader ──────────────────────────────────────

VSOutput vs_main(uint vertex_id : SV_VertexID) {
    VSOutput output;
    float x = float(vertex_id & 1) * 2.0f - 1.0f;
    float y = float((vertex_id >> 1) & 1) * 2.0f - 1.0f;
    output.position = float4(x, -y, 0.0f, 1.0f);
    output.uv = float2((x + 1.0f) * 0.5f, (-y + 1.0f) * 0.5f);
    return output;
}

// ─── World Position Reconstruction from Depth ───────────────────────────────

float3 reconstruct_world_position(float2 uv, float depth) {
    // Convert UV to clip space [-1, 1]
    float4 clip_pos;
    clip_pos.x = uv.x * 2.0f - 1.0f;
    clip_pos.y = (1.0f - uv.y) * 2.0f - 1.0f; // Flip Y
    clip_pos.z = depth;           // Linear depth [0,1] mapped to [near, far]
    clip_pos.w = 1.0f;

    // Unproject: world_pos = inverse(V*P) * clip_pos
    float4 world_pos = mul(inverse_game_view_proj, clip_pos);
    world_pos /= world_pos.w;

    return world_pos.xyz;
}

// ─── Reprojection Pixel Shader ──────────────────────────────────────────────

float4 ps_reproject(VSOutput input) : SV_TARGET {
    // Sample linear depth
    float depth = linear_depth.SampleLevel(point_sampler, input.uv, 0);

    // Skip background (depth == 1.0 meaning far plane)
    if (depth >= 1.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Reconstruct world position from depth
    float3 world_pos = reconstruct_world_position(input.uv, depth);

    // Reproject into VR eye view
    float4 eye_clip = mul(eye_view_proj, float4(world_pos, 1.0f));
    float2 eye_uv = eye_clip.xy / eye_clip.w;
    eye_uv = eye_uv * float2(0.5f, -0.5f) + 0.5f; // To UV space

    // Check if within bounds
    if (any(eye_uv < 0.0f) || any(eye_uv > 1.0f)) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Sample the game color at the reprojected UV
    float4 color = game_color.SampleLevel(linear_sampler, eye_uv, 0);

    return color;
}

// ─── Simple 2D Blit (Fallback, no depth) ────────────────────────────────────

float4 ps_blit(VSOutput input) : SV_TARGET {
    return game_color.SampleLevel(linear_sampler, input.uv, 0);
}

// ─── Depth Linearization (from native Z-buffer) ─────────────────────────────

float linearize_depth(float depth, float near, float far) {
    // For D3D11 reversed-Z: depth = 0 is near, depth = 1 is far
    // Convert to linear distance
    float z = depth;
    float z_near = near;
    float z_far = far;
    return z_near * z_far / (z_far + z * (z_near - z_far));
}

// ─── Compute Shader: Native → Linear Depth Conversion ──────────────────────

RWTexture2D<float> linear_depth_output : register(u0);
Texture2D<float>   native_depth_buffer : register(t2);

[numthreads(8, 8, 1)]
void cs_depth_to_linear(uint3 id : SV_DispatchThreadID) {
    float2 uv = (id.xy + 0.5f) / float2(render_target_size);
    float native_depth = native_depth_buffer[id.xy].r;
    float linear = linearize_depth(native_depth, depth_range.x, depth_range.y);
    linear_depth_output[id.xy] = linear;
}
