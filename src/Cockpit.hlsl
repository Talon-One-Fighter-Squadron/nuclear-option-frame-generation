Texture2D<float4> resolvedScene : register(t0);
Texture2D<float4> nativeCockpit : register(t1);
Texture2D<float4> cockpitBeforePost : register(t2);
Texture2D<float4> sceneAfterPost : register(t3);
Texture2D<float4> opaqueCoverage : register(t4);
RWTexture2D<float4> finalScene : register(u0);
SamplerState linearClamp : register(s0);
cbuffer CompositeOptions : register(b0) { uint flags; };

[numthreads(8,8,1)]
void CompositeCockpit(uint3 id:SV_DispatchThreadID) {
    uint w,h; finalScene.GetDimensions(w,h); if(id.x>=w || id.y>=h)return;
    float4 base=resolvedScene[id.xy], front=nativeCockpit[id.xy];
    float a=saturate(opaqueCoverage[id.xy].a);
    if(a<0.01) {finalScene[id.xy]=base;return;}
    if(flags&2) {
        finalScene[id.xy]=float4(lerp(base.rgb,front.rgb,a),base.a);
        return;
    }
    float2 uv=(id.xy+0.5)/float2(w,h);
    // Bring native cockpit shading into the color space of the game's final post-process.
    // Estimate its local response over several source texels so the transfer cannot erase small markings.
    uint iw,ih; cockpitBeforePost.GetDimensions(iw,ih);
    float2 stepUV=1.5/float2(iw,ih);
    float3 before=0,after=0;
    [unroll] for(int y=-1;y<=1;y++) [unroll] for(int x=-1;x<=1;x++) {
        float2 tap=uv+float2(x,y)*stepUV;
        before+=cockpitBeforePost.SampleLevel(linearClamp,tap,0).rgb;
        after+=sceneAfterPost.SampleLevel(linearClamp,tap,0).rgb;
    }
    float3 gain=clamp((after+0.009)/(before+0.009),0.04,5.0);
    finalScene[id.xy]=float4(lerp(base.rgb,saturate(front.rgb*gain),a),base.a);
}

