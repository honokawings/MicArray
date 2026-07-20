# Repository Guidelines

## Project Structure & Module Organization

`main.cpp` starts the Qt 6 Widgets application. Production code lives under `src/`, grouped by responsibility: `app/` for settings and logging, `control/` for control values, `device/` for TCP lifecycle, `protocol/` for frame encoding and parsing, `media/` for audio/video processing, and `ui/` for widgets. C++ tests mirror these areas under `tests/protocol`, `tests/device`, and `tests/media`; Python tests for the mock server are in `tests/tools`. Keep protocol references in `refer/`, maintained behavior documentation in `doc/Readme.md`, developer utilities in `tools/`, and release automation in `scripts/`. Treat `build*/` and `dist/` as generated output.

## Build, Test, and Development Commands

Use Qt 6.5 or newer, CMake, and a compatible C++20 toolchain.

```powershell
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="<Qt-kit-path>"
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
```

Run the app as `build\dev\MicArray.exe`. To create a deployable Windows package in `dist/`, run `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-release.ps1`; pass `-SkipTests` only when test results are already available.

## Coding Style & Naming Conventions

Follow the existing Qt/C++ style: four-space indentation, Allman braces, `PascalCase` types, `lowerCamelCase` functions and variables, and `m_` prefixes for members. Keep classes in the `MicArray` namespace and pair implementations as `lowercase.h`/`lowercase.cpp`. Group includes as project, Qt, then standard-library headers. Python follows PEP 8 with `snake_case`. No formatter configuration is checked in, so match adjacent code and keep diffs focused.

## Testing Guidelines

Use Qt Test for C++ and standard-library `unittest` for Python. Name C++ files `test_<area>.cpp` and Python methods `test_<behavior>`. Add regression coverage for protocol framing, partial/coalesced TCP input, threading boundaries, and invalid media. There is no numeric coverage threshold; every behavior change should include targeted tests. Update `doc/Readme.md` and its change record when behavior, protocol assumptions, UI, testing, or release flow changes.

## Commit & Pull Request Guidelines

Recent history uses short Chinese summaries describing the delivered outcome, without conventional prefixes; keep the subject concise and put rationale or verification in the body. Pull requests should explain scope and risk, list commands run, link the relevant issue, and include screenshots for UI changes. Call out protocol or configuration changes explicitly and avoid committing generated build or release artifacts.
