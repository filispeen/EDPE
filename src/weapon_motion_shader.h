#pragma once

namespace edpe {

// HLSL shaders for pure D3D11 first-person weapon/viewmodel motion capture:
//   - kWeaponStreamOutShader: pass-through GS for Stream-Output capture of post-VS clip positions
//   - kWeaponMotionRasterShader: rasterizes matched (curClip, prevClip) vertex streams into
//     WeaponMotion (R16G16B16A16_FLOAT):
//       xy = prev - cur in render pixels (+Y down, jitter delta stripped when viewmodel jittered)
//       z  = canonicalDepth = saturate(rawDepth * worldNear / weaponNear)
//       w  = 1.0 (valid coverage)
inline constexpr char kWeaponMotionRasterShader[] = R"(
cbuffer WeaponMotionConstants : register(b0) {
    float4 viewport;      // x: width, y: height, z: worldNear, w: weaponNear
    float4 jitterDelta;   // x: jitterNowX - jitterPrevX, y: jitterNowY - jitterPrevY, z: stripJitter (0 or 1), w: 0
};

struct VSIn {
    float4 curClip  : POSITION0;
    float4 prevClip : POSITION1;
};

struct VSOut {
    float4 pos      : SV_Position;
    float4 curClip  : TEXCOORD0;
    float4 prevClip : TEXCOORD1;
};

VSOut vs_main(VSIn vin) {
    VSOut o;
    o.pos = vin.curClip;
    o.curClip = vin.curClip;
    o.prevClip = vin.prevClip;
    return o;
}

float4 ps_main(VSOut pin) : SV_Target {
    bool validW = (pin.curClip.w > 1e-6 && pin.prevClip.w > 1e-6);
    float2 curNdc  = pin.curClip.xy / max(pin.curClip.w, 1e-6);
    float2 prevNdc = pin.prevClip.xy / max(pin.prevClip.w, 1e-6);

    float2 curPx  = float2((curNdc.x  + 1.0) * 0.5 * viewport.x, (1.0 - curNdc.y)  * 0.5 * viewport.y);
    float2 prevPx = float2((prevNdc.x + 1.0) * 0.5 * viewport.x, (1.0 - prevNdc.y) * 0.5 * viewport.y);
    // Reject mirrored left/right draw swaps (det < 0 or flipped X/Y axes) and extreme mesh distortion.
    float2 dPrevX = ddx(prevPx);
    float2 dPrevY = ddy(prevPx);
    float det = dPrevX.x * dPrevY.y - dPrevX.y * dPrevY.x;
    if (!validW || !isfinite(det) || det < 0.2 || det > 5.0 || dPrevX.x < 0.25 || dPrevY.y < 0.25) {
        discard;
    }
    float2 mv = prevPx - curPx;
    if (jitterDelta.z > 0.5) {
        mv += jitterDelta.xy;
    }
    mv = all(isfinite(mv)) ? clamp(mv, -65504.0, 65504.0) : float2(0.0, 0.0);

    float rawDepth = saturate(pin.curClip.z / pin.curClip.w);
    float nearScale = (viewport.w > 1e-6) ? (viewport.z / viewport.w) : 1.0;
    float canonicalDepth = saturate(rawDepth * nearScale);
    return float4(mv.x, mv.y, canonicalDepth, 1.0);
}
)";

} // namespace edpe
