#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <atomic>
#include <cstring>

extern bool UiMenuVisible();

template <typename TDevice, typename TObjectInstance, typename TDeviceInstance,
          typename TEffectInfo, typename TActionFormat, typename TImageInfoHeader,
          typename TEnumObjectsCallback, typename TEnumEffectsCallback, typename TEnumEffectsInFileCallback,
          typename TStr>
class SafeDirectInputDeviceProxy : public TDevice {
private:
    TDevice* m_real;
    REFIID m_iid;
    std::atomic<ULONG> m_refCount{1};

public:
    SafeDirectInputDeviceProxy(TDevice* real, REFIID iid)
        : m_real(real), m_iid(iid) {}

    virtual ~SafeDirectInputDeviceProxy() = default;

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppvObj) override {
        if (!ppvObj) return E_POINTER;

        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, m_iid)) {
            *ppvObj = static_cast<TDevice*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppvObj);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        m_real->AddRef();
        return ++m_refCount;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        m_real->Release();
        ULONG count = --m_refCount;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // --- IDirectInputDevice8 (Input isolation when ImGui overlay is open) ---
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cbData, LPVOID lpvData) override {
        HRESULT hr = m_real->GetDeviceState(cbData, lpvData);
        if (UiMenuVisible()) {
            if (lpvData && cbData > 0) {
                std::memset(lpvData, 0, cbData);
            }
            return DI_OK;
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) override {
        // When menu is visible, drain buffered events without DIGDD_PEEK so the hardware buffer won't overflow
        DWORD actualFlags = UiMenuVisible() ? (dwFlags & ~DIGDD_PEEK) : dwFlags;
        HRESULT hr = m_real->GetDeviceData(cbObjectData, rgdod, pdwInOut, actualFlags);

        if (UiMenuVisible()) {
            if (pdwInOut) {
                *pdwInOut = 0;
            }
            return DI_OK;
        }
        return hr;
    }

    // --- Passthrough for all other methods ---
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS a) override { return m_real->GetCapabilities(a); }
    HRESULT STDMETHODCALLTYPE EnumObjects(TEnumObjectsCallback a, LPVOID b, DWORD c) override { return m_real->EnumObjects(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID a, LPDIPROPHEADER b) override { return m_real->GetProperty(a, b); }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID a, LPCDIPROPHEADER b) override { return m_real->SetProperty(a, b); }
    HRESULT STDMETHODCALLTYPE Acquire() override { return m_real->Acquire(); }
    HRESULT STDMETHODCALLTYPE Unacquire() override { return m_real->Unacquire(); }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT a) override { return m_real->SetDataFormat(a); }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE a) override { return m_real->SetEventNotification(a); }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND a, DWORD b) override { return m_real->SetCooperativeLevel(a, b); }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(TObjectInstance a, DWORD b, DWORD c) override { return m_real->GetObjectInfo(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(TDeviceInstance a) override { return m_real->GetDeviceInfo(a); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a, DWORD b) override { return m_real->RunControlPanel(a, b); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b, REFGUID c) override { return m_real->Initialize(a, b, c); }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID a, LPCDIEFFECT b, LPDIRECTINPUTEFFECT* c, LPUNKNOWN d) override { return m_real->CreateEffect(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE EnumEffects(TEnumEffectsCallback a, LPVOID b, DWORD c) override { return m_real->EnumEffects(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(TEffectInfo a, REFGUID b) override { return m_real->GetEffectInfo(a, b); }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD a) override { return m_real->GetForceFeedbackState(a); }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD a) override { return m_real->SendForceFeedbackCommand(a); }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK a, LPVOID b, DWORD c) override { return m_real->EnumCreatedEffectObjects(a, b, c); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE a) override { return m_real->Escape(a); }
    HRESULT STDMETHODCALLTYPE Poll() override { return m_real->Poll(); }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD a, LPCDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) override { return m_real->SendDeviceData(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(TStr a, TEnumEffectsInFileCallback b, LPVOID c, DWORD d) override { return m_real->EnumEffectsInFile(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(TStr a, DWORD b, LPDIFILEEFFECT c, DWORD d) override { return m_real->WriteEffectToFile(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE BuildActionMap(TActionFormat a, TStr b, DWORD c) override { return m_real->BuildActionMap(a, b, c); }
    HRESULT STDMETHODCALLTYPE SetActionMap(TActionFormat a, TStr b, DWORD c) override { return m_real->SetActionMap(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetImageInfo(TImageInfoHeader a) override { return m_real->GetImageInfo(a); }
};

using SafeDirectInputDevice8WProxy = SafeDirectInputDeviceProxy<
    IDirectInputDevice8W, LPDIDEVICEOBJECTINSTANCEW, LPDIDEVICEINSTANCEW,
    LPDIEFFECTINFOW, LPDIACTIONFORMATW, LPDIDEVICEIMAGEINFOHEADERW,
    LPDIENUMDEVICEOBJECTSCALLBACKW, LPDIENUMEFFECTSCALLBACKW, LPDIENUMEFFECTSINFILECALLBACK,
    LPCWSTR>;

using SafeDirectInputDevice8AProxy = SafeDirectInputDeviceProxy<
    IDirectInputDevice8A, LPDIDEVICEOBJECTINSTANCEA, LPDIDEVICEINSTANCEA,
    LPDIEFFECTINFOA, LPDIACTIONFORMATA, LPDIDEVICEIMAGEINFOHEADERA,
    LPDIENUMDEVICEOBJECTSCALLBACKA, LPDIENUMEFFECTSCALLBACKA, LPDIENUMEFFECTSINFILECALLBACK,
    LPCSTR>;
