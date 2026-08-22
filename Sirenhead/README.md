# Sirenhead

`Sirenhead` is a native Win32 plugin for x32dbg. It reproduces the runtime
workflow of `WildCoyotte` inside the debugger process:

- runs the exact `FairplayKD_Client.h` handshake from the WildCoyotte sources
  for the current x32dbg process;
- tests `ReadProcessMemory` against `gta_sa.exe` when it is available;
- queries the 96-byte driver blob;
- queries the `ntoskrnl` module information exposed by command 121.

Initialization starts in a worker thread as soon as x32dbg loads the plugin.
The workflow is automatically retried when a debug session or debuggee process
starts. The **Sirenhead** plugin menu also exposes **Status** and **Retry now**.

## Build

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release
```

The output is `build/Release/Sirenhead.dp32`. Copy it into the x32dbg
`x32/plugins` directory and restart x32dbg. The host debugger may need to be
started as administrator if the `FairplayKD0` device ACL requires elevation.
