Texture2D<float4>   tWorld : register(t0); // Апскейлений 3D світ (після DLSS/FSR)
Texture2D<float4>   tUi    : register(t1); // RGB = колір UI, A = Transmittance T
Texture2D<float4>   tMult  : register(t2); // Шар затемнення (vignette / drop shadow)
RWTexture2D<float4> uOut   : register(u0); // Вихідний буфер (SwapChain Backbuffer)

cbuffer CompositeParams : register(b0) {
    uint2 outputSize;
    uint  hasMultiply;
    uint  debugView;
};

[numthreads(32, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outputSize.x || id.y >= outputSize.y) return;
    int3 p = int3(id.xy, 0);

    float4 ui = tUi.Load(p);
    float3 mult = (hasMultiply != 0) ? tMult.Load(p).rgb : float3(1.0, 1.0, 1.0);

    // Якщо піксель не містить емісійного UI, зберігаємо світ без змін:
    if (debugView == 0 && all(ui.rgb <= 1e-4) && all(mult >= 0.999)) {
        uOut[id.xy] = tWorld.Load(p);
        return;
    }

    float4 world = tWorld.Load(p);

    if (debugView != 0) {
        // Режим діагностики: UI поверх темно-синього фону
        float3 dbg = ui.rgb + float3(0.0, 0.05, 0.2) * (1.0 - ui.a);
        uOut[id.xy] = float4(saturate(dbg), 1.0);
        return;
    }

    // Точна комбінація кольорів:
    // ui.rgb - це premultiplied емісійне світло інтерфейсу
    // ui.a   - це коефіцієнт пропускання фону T = prod(1 - alpha)
    // mult   - це мультиплікативне затемнення меню/тіней
    // Захист від затемнення фону підкладками без емісії:
    float effectiveTransmittance = all(ui.rgb <= 1e-4) ? 1.0 : ui.a;
    float3 finalRgb = ui.rgb + world.rgb * effectiveTransmittance * mult;
    uOut[id.xy] = float4(saturate(finalRgb), world.a);
}
