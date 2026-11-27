// Compiled at package build time. UWP never invokes a shader compiler at runtime.
cbuffer FrameConstants : register(b0)
{
    float2 TextureScale;
    float2 Padding;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint id : SV_VertexID)
{
    VertexOutput result;
    float2 uv = float2(id & 1, id >> 1);
    result.position = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    result.uv = uv * TextureScale;
    return result;
}

Texture2D FrameTexture : register(t0);
SamplerState PointSampler : register(s0);

float4 PSMain(VertexOutput input) : SV_TARGET
{
    // Game frames are drawn with blending disabled; retaining alpha also lets
    // the same shader composite the premultiplied transparent artwork frame.
    return FrameTexture.Sample(PointSampler, input.uv);
}
