#pragma once

#include <windows.h>

void EdpeLog(const wchar_t* message);
void EdpeLogFlush();
void EdpeLogShutdown(bool waitForThread);
void EdpeLogDumpRecent(HANDLE file);
