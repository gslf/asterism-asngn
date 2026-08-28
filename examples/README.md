# Starter configurations

These are configuration templates, not engine roots. Copy the files you need
into a separate runtime directory; do not run asngn with `examples/` itself as
the engine root. Copy the assembled astools package directories from
`build/packages/` (or another asngn build tree) into the runtime directory's
`tools/` folder.

Embedded llama.cpp:

```powershell
New-Item -ItemType Directory $env:USERPROFILE\asngn
Copy-Item examples\embedded.xcdn $env:USERPROFILE\asngn\config.xcdn
Copy-Item examples\astools.xcdn $env:USERPROFILE\asngn\astools.xcdn
```

LM Studio:

```powershell
New-Item -ItemType Directory $env:USERPROFILE\asngn
Copy-Item examples\lmstudio.xcdn $env:USERPROFILE\asngn\config.xcdn
Copy-Item examples\astools.xcdn $env:USERPROFILE\asngn\astools.xcdn
```

Before starting, replace both `REPLACE_WITH_..._MODEL` markers in the LM Studio
template. Relative model and tool paths are relative to the engine root. Keep
runtime `memory/`, `sessions/`, `cache/`, `telemetry/`, logs, and workspaces
outside the source repository.
