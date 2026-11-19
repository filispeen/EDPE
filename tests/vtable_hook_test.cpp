#include "vtable_hook.h"

namespace {

struct MockInterface {
    virtual int __stdcall first(int x) = 0;
    virtual int __stdcall second(int x) = 0;
    virtual ~MockInterface() = default;
};

struct MockObject : public MockInterface {
    int __stdcall first(int x) override { return x + 1; }
    int __stdcall second(int x) override { return x * 2; }
};

using MethodFn = int(__stdcall*)(MockInterface*, int);
MethodFn g_origFirst = nullptr;
int g_hookCalls = 0;

int __stdcall hookedFirst(MockInterface* self, int x) {
    ++g_hookCalls;
    return g_origFirst ? g_origFirst(self, x + 10) : -1;
}

} // namespace

int main() {
    MockObject obj;
    MockInterface* iface = &obj;
    void** origTable = *reinterpret_cast<void***>(iface);

    edpe::VTableHook hook;
    void* outOrig = nullptr;
    if (!hook.hook(iface, 0, reinterpret_cast<void*>(&hookedFirst), &outOrig, edpe::HookMode::LiveCopy)) {
        return 1;
    }
    g_origFirst = reinterpret_cast<MethodFn>(outOrig);

    if (!hook.isInitialized() || hook.mode() != edpe::HookMode::LiveCopy) return 2;
    if (!hook.isLiveCopyVptrValid(iface)) return 3;
    if (hook.isLiveCopyVptrValid(nullptr)) return 4;

    int dummy = 0;
    if (hook.isLiveCopyVptrValid(&dummy)) return 5;

    // Steady-state LiveCopy call must leave vptr valid and reclaimLive must return false (no drift).
    if (hook.reclaimLive(iface)) return 6;
    if (iface->first(5) != 16 || g_hookCalls != 1) return 7;
    if (iface->second(6) != 12) return 8;

    // Simulate runtime vptr drift back to original table; reclaimLive must detect & restore it.
    *reinterpret_cast<void***>(iface) = origTable;
    if (hook.isLiveCopyVptrValid(iface)) return 9;
    if (!hook.reclaimLive(iface)) return 10;
    if (!hook.isLiveCopyVptrValid(iface)) return 11;
    if (iface->first(7) != 18 || g_hookCalls != 2) return 12;

    hook.unhookAll();
    if (hook.isInitialized()) return 13;
    if (hook.isLiveCopyVptrValid(iface)) return 14;
    if (*reinterpret_cast<void***>(iface) != origTable) return 15;
    if (iface->first(5) != 6) return 16;

    // Verify InPlace mode fast intact check and selective reclaimLive recovery.
    g_hookCalls = 0;
    edpe::VTableHook inPlaceHook;
    void* inPlaceOrig = nullptr;
    if (!inPlaceHook.hook(iface, 0, reinterpret_cast<void*>(&hookedFirst), &inPlaceOrig, edpe::HookMode::InPlace)) {
        return 17;
    }
    g_origFirst = reinterpret_cast<MethodFn>(inPlaceOrig);
    if (!inPlaceHook.isInitialized() || inPlaceHook.mode() != edpe::HookMode::InPlace) return 18;
    if (!inPlaceHook.isPrimaryVptrValidFast(iface) || !inPlaceHook.areInPlaceHooksIntactFast(iface)) return 19;
    if (inPlaceHook.reclaimLive(iface)) return 20;
    if (iface->first(5) != 16 || g_hookCalls != 1) return 21;

    // Simulate InPlace slot overwrite; reclaimLive must detect & restore the slot.
    DWORD oldProt = 0;
    if (!VirtualProtect(&origTable[0], sizeof(void*), PAGE_READWRITE, &oldProt)) return 22;
    origTable[0] = inPlaceOrig;
    DWORD ignored = 0;
    VirtualProtect(&origTable[0], sizeof(void*), oldProt, &ignored);
    if (inPlaceHook.areInPlaceHooksIntactFast(iface)) return 23;
    if (!inPlaceHook.reclaimLive(iface)) return 24;
    if (!inPlaceHook.areInPlaceHooksIntactFast(iface)) return 25;
    if (iface->first(7) != 18 || g_hookCalls != 2) return 26;

    inPlaceHook.unhookAll();
    if (inPlaceHook.isInitialized()) return 27;
    if (iface->first(5) != 6) return 28;

    return 0;
}
