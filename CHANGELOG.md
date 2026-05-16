# UnrealClaude Changelog

## [Unreleased] — v1.5.0

### Fixed

- **Blueprint self-function calls** — `CallFunction` nodes can now call user-defined functions on the same Blueprint (e.g. calling `UpdateFog` from inside `SetWeatherState`). Previously, only `KismetSystemLibrary` and `KismetMathLibrary` were searched, causing "Function not found" for any Blueprint-own function.

### Added

- **Function graph support for all node operations** — `add_node`, `add_nodes`, `delete_node`, `connect_pins`, `disconnect_pins`, and `set_pin_value` now accept `graph_name` (string) and `is_function_graph` (bool) params. When `is_function_graph: true`, the tool targets `Blueprint->FunctionGraphs` instead of `Blueprint->UbergraphPages`, enabling full node wiring inside user-defined function graphs. Previously, these operations were silently limited to the EventGraph.

- **`GameplayStatics` class resolution** — `CallFunction` nodes now resolve `GameplayStatics` as a named `target_class`, and it is included in the automatic fallback search. Enables creating `Get All Actors Of Class`, `SpawnActor`, `GetPlayerController`, etc. without boilerplate.

- **Node GUID acceptance (documented)** — `connect_pins`, `delete_node`, and `set_pin_value` accept node GUIDs returned by `blueprint_query` `get_graph`, not just MCP-generated IDs. This was already implemented but undocumented; the context file now clearly describes both accepted ID formats.

- **Comprehensive tool description improvements** — All major tool schemas updated with actionable parameter guidance to reduce trial-and-error:
  - `blueprint_modify`: Added per-operation required-params quick-reference, per-node-type `node_params` examples (`CallFunction` self vs library calls, `VariableGet/Set`, `Event`, `Sequence`), exec pin naming guide (`execute`/`then`, `True`/`False`, `then_0`…), `variable_type` list corrected to supported primitives only (object ref types removed — they must be added manually), `inputs` array for `add_function` now documented, `graph_name` + `is_function_graph` combination usage clarified.
  - `blueprint_query`: Added 4-step workflow guide, explicit note that `get_nodes` returns IDs usable directly by `blueprint_modify`, `get_node_pins` highlighted as the pre-wiring pin-discovery step, `graph_name` behavior clarified per operation.
  - `set_property`: Added value format table (FVector uppercase X/Y/Z, FLinearColor R/G/B/A + hex `#RRGGBBAA`, FRotator Pitch/Yaw/Roll), component name casing note, `actor_name` clarified as label or internal name.
  - `spawn_actor`: `name` param clarified as the Outliner label required for other tools to find the actor.
  - `material`: Added operation quick-reference, `parameters` object format example with uppercase R/G/B/A, required-param notes per operation.
  - `anim_blueprint_modify`: `source_pin`/`target_pin` now list typical pin names per node type and reference `inspect_node_pins` for discovery.

---

## [1.4.5] — 2026-04

### Fixed

- **Blueprint Interface function creation crash** — Creating a Blueprint Interface that already existed crashed the editor. Fixed duplicate-creation guard in the `create` operation.

- **Interface function inputs silently dropped** — `add_function_input` on an Interface Blueprint was accepted but the inputs were not persisted. Root cause was incorrect graph traversal when the Interface had no generated class. Fixed graph lookup to use `Blueprint->FunctionGraphs` directly.

---

## [1.4.4] and earlier

See git log for prior history.
