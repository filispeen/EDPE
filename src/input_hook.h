#pragma once
#include <windows.h>

namespace edpe {
bool patchIatEntry(HMODULE module, const char* targetDll, const char* targetFunction, void* hook, void** orig);
void installInputHooks();
}
