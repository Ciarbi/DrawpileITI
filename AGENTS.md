# Agent Notes

## Build Commands
- Build (from project root): `cmake --build build` (or `ninja -C build` with the
  Ninja generator used by `.kilo`).
- Configure once: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` then
  `cmake --build build`.
- The project mixes C++ (Qt5/Qt6, CMake) and Rust (Cargo workspace in
  `Cargo.toml`). **Cargo/rustc is required** because `src/drawdance` (the
  server engine) is added when `CLIENT` or `SERVER` is enabled and is built via
  `include(Cargo)` (see `cmake/Cargo.cmake`). Without `cargo` in `PATH`, CMake
  warns and Rust-dependent features are unavailable.

## Options (set at configure time with `-DKEY=ON/OFF`)
- `CLIENT`, `SERVER`, `SERVERGUI`, `TOOLS`, `TESTS`, `CLANG_TIDY`
- `DP_MIN_QT_VERSION`, `DP_MIN_QT_VERSION_GUI`
- `INTERFACE_MODE` (`Desktop` | `SmallScreen` | `Dynamic`), `ANDROID`

## Tests
- Desktop tests are CTest targets. Enable them with `-DTESTS=ON`.
- List: `ctest --test-dir build -N` (names like `dpcommon_*`, `dpengine_*`,
  `dpimpex_*`, `dpmsg_*`, `test_client_*`).
- Run: `ctest --test-dir build -j` (sets `QT_QPA_PLATFORM=offscreen`).
- The `cmake/Tests.cmake` helpers (`add_unit_test`/`add_unit_tests`) register
  each executable. There is no top-level `make test` alias, but CTest drives
  them.

## Lint / Tidy
- No project-wide lint script. Compiler warnings are enabled via
  `cmake/DrawpileCompilerOptions.cmake`.
- Clang-Tidy is optional: enable with `-DCLANG_TIDY=ON` (CI uses `OFF`). The
  `.clang-tidy` file in the repo root configures it. A curated set of modernize
  + readability checks is enabled, with the project-specific
  `clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling`
  disable retained because the C-heavy drawdance engine intentionally uses raw
  buffer patterns.

## Touch input
- `src/libclient/view/touchhandler.cpp` disambiguates one-finger touch (draw vs.
  pan vs. tap/double-tap/tap-and-hold). The brush cursor / outline is updated
  for every `touchMoved` (see `CanvasControllerBase::penMoveEvent`, which routes
  a cursor-only update through the `penHover` branch when the pen is up).
- Default `OneFingerTouchAction` is `Pan` on desktop and `Guess` on
  Android/Emscripten; switch to `Draw` in Settings → Touch to draw with touch.
