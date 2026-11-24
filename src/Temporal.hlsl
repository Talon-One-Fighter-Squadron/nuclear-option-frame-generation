Texture2D<float4> sceneColor : register(t0);
Texture2D<float> worldDepth : register(t1);
Texture2D<float2> worldMotion : register(t2);
Texture2D<float4> previousColor : register(t3);
Texture2D<float4> beforeCockpit : register(t4);
Texture2D<float4> afterCockpit : register(t5);
Texture2D<float> cockpitDepth : register(t6);
Texture2D<float2> cockpitMotion : register(t7);
Texture2D<float4> nativeCoverage : register(t8);
RWTexture2D<float4> worldColor : register(u2);
RWTexture2D<float2> worldBackward : register(u3);
RWTexture2D<float> mergedDepth : register(u0);
RWTexture2D<float2> backwardMotion : register(u1);
SamplerState linearClamp : register(s0);
cbuffer Camera : register(b0) {
    row_major float4x4 clipToPrevious;
    uint2 size;
    uint resetHistory;
    uint hasCockpit;
    float4 planes; // world near/far, cockpit near/far
    float2 deltaJitter;
};
float maxChannel(float3 c) { return max(c.r,max(c.g,c.b)); }
float colorError(float3 a,float3 b) {
    return maxChannel(abs(a-b))/(0.08 + max(maxChannel(a),maxChannel(b)));
}
float historyError(float2 uv,float2 motion,float3 color) {
    return colorError(previousColor.SampleLevel(linearClamp, uv+motion+deltaJitter/size,0).rgb,color);
}
[numthreads(8,8,1)]
void TemporalInputs(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size)) return;
    int2 p=id.xy;
    float2 uv=(p+0.5)/size;
    float depth=worldDepth[p];
    // URP encodes previous-to-current velocity. Our textures have also been flipped vertically for DXGI.
    float2 motion=worldMotion[p]*float2(-1,1);
    float3 color=sceneColor[p].rgb;
    float3 clean=color;
    bool split=hasCockpit>=2;
    if(split) {
        worldBackward[p]=motion;
        // Remove the low-resolution foreground from the world history. Sample a nearby uncovered
        // texel for the local post-process response where the cockpit hides the world.
        float2 texel=1.0/size;
        float coverage=0;
        [unroll] for(int q=0;q<4;q++) {
            float2 d=float2((q&1)?1:-1,(q&2)?1:-1)*0.75;
            coverage=max(coverage,nativeCoverage.SampleLevel(linearClamp,uv+d*texel,0).a);
        }
        if(coverage>.001) {
            float3 gain=1;
            float3 before=beforeCockpit[p].rgb;
            float3 a=0,b=0;float count=0;
            [unroll] for(int q=0;q<8;q++) {
                float2 offset=float2(cos(q*0.78539816),sin(q*0.78539816))*6*texel;
                if(nativeCoverage.SampleLevel(linearClamp,uv+offset,0).a>.005) continue;
                a+=beforeCockpit.SampleLevel(linearClamp,uv+offset,0).rgb;
                b+=sceneColor.SampleLevel(linearClamp,uv+offset,0).rgb;
                count++;
            }
            if(count>0)gain=clamp((b+.002)/(a+.002),.04,5);
            clean=hasCockpit==3?before:before*gain;
        }
        worldColor[p]=float4(clean,1);
    }
    bool cockpit=false;
    if(hasCockpit) {
        float3 before=beforeCockpit[p].rgb, after=afterCockpit[p].rgb;
        cockpit=maxChannel(abs(before-after))>0.001 && cockpitDepth[p]>0;
        if(cockpit) {
            float z=planes.z*planes.w/(planes.z+cockpitDepth[p]*(planes.w-planes.z));
            depth=saturate((planes.x*planes.y/max(z,planes.x)-planes.x)/(planes.y-planes.x));
            motion=cockpitMotion[p]*float2(-1,1);
        }
    }
    float4 prev=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),clipToPrevious);
    float3 prevNdc=prev.xyz/max(prev.w,1e-6);
    float2 geometricMotion=prevNdc.xy*float2(.5,-.5)+.5-uv;
    float residual=historyError(uv,motion,color);
    if(!resetHistory) {
        // Cockpit surfaces move with the aircraft. World geometry can have missing object vectors.
        // Accept the alternative only when it reprojects the previous color substantially better.
        float2 candidate=cockpit?float2(0,0):geometricMotion;
        if((cockpit || all(abs(motion)*size<0.01)) && length((motion-candidate)*size)>1.0 && all(abs(candidate)<1)) {
            float alternative=historyError(uv,candidate,color);
            if(alternative+0.035<residual*0.65) { motion=candidate; }
        }
    }
    if(split && !cockpit) worldBackward[p]=motion;
    mergedDepth[p]=depth;
    backwardMotion[p]=motion;
}

