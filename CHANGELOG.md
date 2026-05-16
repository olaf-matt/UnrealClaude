# UnrealClaude Changelog

## [Unreleased] — v1.5.0

### Fixed

- **Blueprint self-function calls** — `CallFunction` nodes can now call user-defined functions on the same Blueprint (e.g. calling `UpdateFog` from inside `SetWeatherState`). Previously, only `KismetSystemLibrary` and `KismetMathLibrary` were searched, causing "Function not found" for any Blueprint-own function.

### Added

- **Function graph support for all node operations** — `add_node`, `add_nodes`, `delete_node`, `connect_pins`, `disconnect_pins`, and `set_pin_value` now accept `graph_name` (string) and `is_function_graph` (bool) params. When `is_function_graph: true`, the tool targets `Blueprint->FunctionGraphs` instead of `Blueprint->UbergraphPages`, enabling full node wiring inside user-defined function graphs. Previously, these operations were silently limited to the EventGraph.

- **`GameplayStatics` class resolution** — `CallFunction` nodes now resolve `GameplayStatics` as a named `target_class`, and it is included in the automatic fallback search. Enables creating `Get All Actors Of Class`, `SpawnActor`, `GetPlayerController`, etc. without boilerplate.

- **Node GUID acceptance (documented)** — `connect_pins`, `delete_node`, and `set_pin_value` accept node GUIDs returned by `blueprint_query` `get_graph`, not just MCP-generated IDs. This was already implemented but undocumented; the context file now clearly describes both accepted ID formats.

---

## [1.4.5] — 2026-04

### Fixed

- **Blueprint Interface function creation crash** — Creating a Blueprint Interface that already existed crashed the editor. Fixed duplicate-creation guard in the `create` operation.

- **Interface function inputs silently dropped** — `add_function_input` on an Interface Blueprint was accepted but the inputs were not persisted. Root cause was incorrect graph traversal when the Interface had no generated class. Fixed graph lookup to use `Blueprint->FunctionGraphs` directly.

---

## [1.4.4] and earlier

See git log for prior history.
