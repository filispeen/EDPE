Texture2D<float4>   tWorld    : register(t0); // Апскейлений 3D світ (після DLSS/FSR/TAA)
Texture2D<float4>   tUi       : register(t1); // Тонемаплений колір UI (LdrLayer.rgb = T(H))
Texture2D<float4>   tMult     : register(t2); // Шар мультиплікативного затемнення (vignette / drop shadow)
Texture2D<float4>   tHdrUi    : register(t3); // Лінійний HDR-шар UI (RGB = емісія, A = Transmittance T = prod(1 - alpha))
Texture2D<float4>   tVisorLdr : register(t4); // Тонемаплена сцена + наекранний шолом (для дельта-композиту)
Texture2D<float4>   tCleanLdr : register(t5); // Тонемаплена сцена без шолома
SamplerState        sLinear   : register(s0);
RWTexture2D<float4> uOut      : register(u0); // Вихідний буфер

cbuffer CompositeParams : register(b0) {
    uint2  outputSize;
    uint   hasMultiply;
    uint   debugView;
    uint   hasVisorDelta;
    float  bgBrightness;
    float  bgGamma;
    float  uiBrightness;
    float  uiGamma;
    float  bgAttenGamma;
    float  sharpenStrength;
    float  _pad;
};

float4 loadWorldSharpened(int2 p) {
    if (sharpenStrength <= 1e-4) {
        return tWorld.Load(int3(p, 0));
    }
    int2 hi = max(int2(outputSize) - int2(1, 1), int2(0, 0));
    float4 center = tWorld.Load(int3(clamp(p, int2(0, 0), hi), 0));
    float3 a = saturate(tWorld.Load(int3(clamp(p + int2(-1, -1), int2(0, 0), hi), 0)).rgb);
    float3 b = saturate(tWorld.Load(int3(clamp(p + int2( 0, -1), int2(0, 0), hi), 0)).rgb);
    float3 c = saturate(tWorld.Load(int3(clamp(p + int2( 1, -1), int2(0, 0), hi), 0)).rgb);
    float3 d = saturate(tWorld.Load(int3(clamp(p + int2(-1,  0), int2(0, 0), hi), 0)).rgb);
    float3 e = saturate(center.rgb);
    float3 f = saturate(tWorld.Load(int3(clamp(p + int2( 1,  0), int2(0, 0), hi), 0)).rgb);
    float3 g = saturate(tWorld.Load(int3(clamp(p + int2(-1,  1), int2(0, 0), hi), 0)).rgb);
    float3 h = saturate(tWorld.Load(int3(clamp(p + int2( 0,  1), int2(0, 0), hi), 0)).rgb);
    float3 k = saturate(tWorld.Load(int3(clamp(p + int2( 1,  1), int2(0, 0), hi), 0)).rgb);

    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mn2 = min(min(a, c), min(g, k));
    mn += min(mn, mn2);

    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 mx2 = max(max(a, c), max(g, k));
    mx += max(mx, mx2);

    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));
    float3 w = amp * (-0.2 * saturate(sharpenStrength));
    float3 rcpW = 1.0 / (1.0 + 4.0 * w);
    float3 res = saturate(max(((b + d + f + h) * w + e) * rcpW, 0.0));
    return float4(res, center.a);
}

[numthreads(32, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outputSize.x || id.y >= outputSize.y) return;
    int3 p = int3(id.xy, 0);

    float4 hdrUi = tHdrUi.Load(p);
    float transmittance = saturate(hdrUi.a);
    float3 mult = (hasMultiply != 0) ? tMult.Load(p).rgb : float3(1.0, 1.0, 1.0);

    float3 visorDelta = float3(0.0, 0.0, 0.0);
    if (debugView == 0 && hasVisorDelta != 0) {
        float2 uv = (float2(p.xy) + 0.5) / float2(outputSize);
        visorDelta = tVisorLdr.SampleLevel(sLinear, uv, 0).rgb - tCleanLdr.SampleLevel(sLinear, uv, 0).rgb;
    }

    // Якщо піксель не містить ні емісійного UI, ні напівпрозорої підкладки (transmittance == 1),
    // повертаємо піксель 3D-світу (плюс дельта шолома, якщо активна) без читання tUi:
    if (debugView == 0 && all(hdrUi.rgb <= 1e-5) && transmittance >= 0.999 && all(mult >= 0.999)) {
        float4 worldOnly = loadWorldSharpened(p.xy);
        if (hasVisorDelta != 0) {
            worldOnly.rgb = saturate(worldOnly.rgb + visorDelta);
        }
        uOut[id.xy] = worldOnly;
        return;
    }

    float4 ui = tUi.Load(p);
    int2 maxCoord = max(int2(outputSize) - int2(1, 1), int2(0, 0));
    int3 corners[4] = {
        int3(0, 0, 0),
        int3(maxCoord.x, 0, 0),
        int3(0, maxCoord.y, 0),
        int3(maxCoord.x, maxCoord.y, 0)
    };
    float3 blackFloor = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int ci = 0; ci < 4; ++ci) {
        float4 hc = tHdrUi.Load(corners[ci]);
        float4 uc = tUi.Load(corners[ci]);
        if (hc.a >= 0.999 && all(hc.rgb <= 1e-5) && all(uc.rgb < 0.08)) {
            blackFloor = uc.rgb;
            break;
        }
    }
    float3 uiRaw = max(ui.rgb - blackFloor * transmittance, 0.0);

    float effUiBrightness = (uiBrightness > 0.0) ? uiBrightness : 1.0;
    float effUiGamma      = (uiGamma > 0.0) ? uiGamma : 1.0;
    float effBgBrightness = (bgBrightness > 0.0) ? bgBrightness : 1.0;
    float effBgGamma      = (bgGamma > 0.0) ? bgGamma : 1.0;
    float g               = (bgAttenGamma > 0.0) ? max(bgAttenGamma, 0.1) : 2.2;

    float3 uiPart = pow(max(uiRaw, 0.0), max(effUiGamma, 0.1)) * effUiBrightness;

    if (debugView != 0) {
        // Режим діагностики: UI поверх темно-синього фону
        float3 dbg = uiPart + float3(0.0, 0.05, 0.2) * (1.0 - transmittance);
        uOut[id.xy] = float4(saturate(dbg), 1.0);
        return;
    }

    float4 world = loadWorldSharpened(p.xy);
    float3 cleanMult = all(mult >= 0.999) ? float3(1.0, 1.0, 1.0) : mult;
    float3 atten = transmittance * cleanMult;
    float  minAtten = min(atten.r, min(atten.g, atten.b));

    float3 attenWorld = all(atten >= 0.999)
        ? world.rgb
        : pow(max(pow(max(world.rgb, 0.0), g) * atten, 0.0), 1.0 / g);
    float backdropWeight = (minAtten < 0.999) ? saturate((1.0 - minAtten) * 4.0) : 0.0;
    float3 backdropTuned = pow(max(attenWorld, 0.0), max(effBgGamma, 0.1)) * effBgBrightness;
    float3 worldAndBackdrop = lerp(attenWorld, backdropTuned, backdropWeight);

    // Додаємо UI та ослаблений 3D-фон у лінійному світлі (gamma 2.2 -> linear -> add -> gamma 2.2),
    // щоб напівпрозорий/м'який помаранчевий glow HUD не підсилювався у гамма-просторі.
    // Для низькояскравого HUD, який потрапляє у тіньову зону 3D LUT при тонемапінгу поверх чорного,
    // зберігаємо лінійну хроматичність із tHdrUi, щоб уникнути червоних артефактів.
    float3 uiLin = pow(max(uiPart, 0.0), 2.2);
    float3 hdrPos = max(hdrUi.rgb, 0.0);
    float  hPeak  = max(hdrPos.r, max(hdrPos.g, hdrPos.b));
    float  uPeak  = max(uiLin.r, max(uiLin.g, uiLin.b));
    if (hPeak > 1e-5 && uPeak > 1e-5) {
        float3 uiChromaLin = hdrPos * (uPeak / hPeak);
        float  uiRawPeak   = max(uiRaw.r, max(uiRaw.g, uiRaw.b));
        // Only recover LUT shadow-toe chromaticity crush on very dim glow pixels;
        // midtone and bright HUD (>= 0.32) preserve 100% of Cobra's Pass 52 LUT hue.
        float  toeMix      = 1.0 - smoothstep(0.15, 0.32, uiRawPeak);
        uiLin = lerp(uiLin, uiChromaLin, toeMix);
    }
    float3 worldAndBackdropLin = pow(max(worldAndBackdrop, 0.0), 2.2);
    float3 sumLin = uiLin + worldAndBackdropLin;
    float  sumPeak = max(sumLin.r, max(sumLin.g, sumLin.b));
    if (sumPeak > 1.0) {
        // Soft filmic highlight roll-off toward warm white instead of per-channel hard clipping.
        float excess = sumPeak - 1.0;
        sumLin = lerp(sumLin / sumPeak, float3(1.0, 1.0, 1.0), saturate(excess / (1.0 + excess)));
    }
    float3 finalSrgb = pow(max(sumLin, 0.0), 1.0 / 2.2);
    float3 finalRgb = finalSrgb + visorDelta * atten;
    uOut[id.xy] = float4(saturate(finalRgb), world.a);
}
