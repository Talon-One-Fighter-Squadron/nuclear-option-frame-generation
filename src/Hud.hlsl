Texture2D<float4> finalColor : register(t0);
Texture2D<float4> sceneOnly : register(t1);
RWTexture2D<float4> uiLayer : register(u0);
[numthreads(8,8,1)]
void HudOpacity(uint3 id:SV_DispatchThreadID) {
    uint w,h;uiLayer.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
    float3 f=finalColor[id.xy].rgb, b=sceneOnly[id.xy].rgb;
    // Minimum opacity capable of producing the observed composite with a bounded UI color.
    // Saturated HUD strokes and black translucent backgrounds each constrain at least one channel.
    float3 diff=f-b;
    float3 alpha=max(diff/max(1-b,0.001),-diff/max(b,0.001));
    float a=saturate(max(alpha.x,max(alpha.y,alpha.z)));
    uiLayer[id.xy]=float4(saturate(f-(1-a)*b),a);
}

