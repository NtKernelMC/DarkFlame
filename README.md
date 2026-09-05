# DarkFlame

Visual Studio 2026 C++ solution containing `DarkFlame.exe` and two x86 DLLs:

- `DarkFlameAgent.dll` is manually mapped into the suspended MTA launcher and
  installs the child-process hook.
- `DarkFlameClient.dll` is manually mapped by the bootstrap agent into
  `gta_sa.exe` and owns the runtime loader and netc hooks.

Build `DarkFlame.sln` with `Release | x86`. Artifacts are written to
`bin/Release/x86`.


Both manually mapped DLLs install a PDB-aware crash handler. Release builds emit
`DarkFlameAgent.pdb` and `DarkFlameClient.pdb`; keep each PDB beside its DLL.
`DarkFlame.log` is written beside `DarkFlame.exe` and reset at each loader
start. Crash traces and minidumps go to its `CrashDumps` folder, with
manual-map addresses resolved to function names and source lines. Each process
start removes only its own previous
DarkFlame crash artifacts, so stale dumps do not pile up.

Bootstrap environment variables are removed from Loader, Agent, and Client as
soon as their values have been consumed, including failure paths.

The required x86 MinHook sources are vendored under
`Agent/third_party/minhook`; the solution has no external library dependency.
MTA:SA Province Software
