# Складання EDPE

## Вимоги

- Windows 10/11 x64.
- Visual Studio 2022 із workload **Desktop development with C++**: MSVC x64 та Windows SDK.
- CMake 3.25 або новіший.
- NMake, який постачається з Visual Studio; команди нижче потрібно виконувати в **x64 Native Tools PowerShell for VS 2022** або Developer PowerShell, де доступна x64 збірка MSVC.
- Ініціалізований Git submodule `external/imgui` (Dear ImGui).

Потрібні C++20 і системні Direct3D 11/DXGI заголовки та бібліотеки з Windows SDK. CMake також збирає шейдери через системний `d3dcompiler`. Проєкт наразі не завантажує або не лінкує NVIDIA NGX чи AMD FidelityFX SDK: NGX-інтеграція в коді поки заглушка.

## Підготовка залежності

Після клонування репозиторію завантажте Dear ImGui:

```powershell
git submodule update --init --recursive
```

## Збірка

З кореня репозиторію, у x64 Developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles" "-DCMAKE_MAKE_PROGRAM=$((Get-Command nmake).Source)"
cmake --build build
```

Головні DLL:

```text
build/EDPE.dll
build/dxgi.dll
```

Ціль `proxy_stage` збирається разом з іншими цілями та ставить runtime-файлы сюди:

```text
build/stage/d3d11.dll       # копія EDPE.dll під назвою проксі
build/stage/dxgi.dll
build/stage/proxy_smoke.exe
```

Для повторної збірки з уже налаштованим каталогом `build` достатньо виконати `cmake --build build` у тій самій x64 Developer PowerShell.

## Тести

Запустіть тести після успішної збірки:

```powershell
ctest --test-dir build --output-on-failure
```

Наявні тести охоплюють temporal math, GPU motion pass, DXBC fanout і proxy smoke checks. GPU-тест потребує Windows та доступного D3D11-пристрою.

## Поточне застереження

У `src/ngx_context.cpp` наразі є ініціалізатор `0xPE` у масиві `kNgxProjectId`. `0xPE` не є допустимим C++ числовим літералом, тому ця версія вихідного коду може завершити компіляцію помилкою MSVC. Перед успішною збіркою потрібно виправити константу на валідне значення. NGX функціональність при цьому все одно залишається вимкненою заглушкою.

## Локальне розміщення проксі для запуску гри

README описує ручний тест із копіюванням `EDPE.dll` як `d3d11.dll` та `dxgi.dll` поруч із `EliteDangerous64.exe`. Збірка або тести не виконують цього розгортання автоматично. Використовуйте лише DLL, отримані після успішної збірки.
