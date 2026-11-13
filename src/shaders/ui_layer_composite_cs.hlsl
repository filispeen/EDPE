Texture2D<float4>   tWorld : register(t0); // Апскейлений 3D світ (після DLSS/FSR/TAA)
Texture2D<float4>   tUi    : register(t1); // Тонемаплений колір UI (LdrLayer.rgb)
Texture2D<float4>   tMult  : register(t2); // Шар мультиплікативного затемнення (vignette / drop shadow)
Texture2D<float4>   tHdrUi : register(t3); // Лінійний HDR-шар UI (RGB = емісія, A = Transmittance T = prod(1 - alpha))
RWTexture2D<float4> uOut   : register(u0); // Вихідний буфер

cbuffer CompositeParams : register(b0) {
    uint2 outputSize;
    uint  hasMultiply;
    uint  debugView;
};

float3 srgbToLinear(float3 c) {
    c = max(c, 0.0);
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

float3 linearToSrgb(float3 c) {
    c = max(c, 0.0);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, 0.0031308));
}

[numthreads(32, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outputSize.x || id.y >= outputSize.y) return;
    int3 p = int3(id.xy, 0);

    float4 hdrUi = tHdrUi.Load(p);
    float transmittance = saturate(hdrUi.a);
    float3 mult = (hasMultiply != 0) ? tMult.Load(p).rgb : float3(1.0, 1.0, 1.0);

    // Якщо піксель не містить ні емісійного UI, ні напівпрозорої підкладки (transmittance == 1),
    // миттєво повертаємо піксель 3D-світу без читання tUi та без перетворень:
    if (debugView == 0 && all(hdrUi.rgb <= 1e-5) && transmittance >= 0.999 && all(mult >= 0.999)) {
        uOut[id.xy] = tWorld.Load(p);
        return;
    }

    float4 ui = tUi.Load(p);
    float4 corner0 = tUi.Load(int3(0, 0, 0));
    float4 hdrCorner0 = tHdrUi.Load(int3(0, 0, 0));
    float3 blackFloor = (hdrCorner0.a >= 0.999 && all(hdrCorner0.rgb <= 1e-5) && all(corner0.rgb < 0.08))
        ? corner0.rgb : float3(0.0, 0.0, 0.0);
    float3 uiRgb = max(ui.rgb - blackFloor * transmittance, 0.0);

    if (debugView != 0) {
        // Режим діагностики: UI поверх темно-синього фону
        float3 dbg = uiRgb + float3(0.0, 0.05, 0.2) * (1.0 - transmittance);
        uOut[id.xy] = float4(saturate(dbg), 1.0);
        return;
    }

    float4 world = tWorld.Load(p);
    float3 cleanMult = all(mult >= 0.999) ? float3(1.0, 1.0, 1.0) : mult;

    // Оскільки коефіцієнт пропускання T = prod(1 - alpha) накопичується у лінійному
    // HDR-просторі до тонемапінгу (Pass 35), ослаблення фону виконується у лінійному
    // світлі перед поверненням у гамма-простір sRGB:
    float3 worldLin = srgbToLinear(world.rgb);
    float3 attenWorld = linearToSrgb(worldLin * (transmittance * cleanMult));
    float3 finalRgb = uiRgb + attenWorld;
    uOut[id.xy] = float4(saturate(finalRgb), world.a);
}
