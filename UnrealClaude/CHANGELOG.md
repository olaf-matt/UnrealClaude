# UnrealClaude Plugin — Changelog

## Unreleased (branch: fix/blueprint-interface-bugs)

### Added
- `add_function_input` operation on `blueprint_modify` — adds a single typed input pin to an
  existing function on a Blueprint Interface. Supports the same type tokens as `variable_type`
  (bool, int, float, byte, string, Vector, Rotator, Transform). Duplicate pin names are rejected
  with a clear error. Regular Blueprint function parameters must still be added via the `inputs`
  array on `add_function` or manually in the editor.

  **Files changed:**
  - `Private/BlueprintEditor.h` — `AddFunctionInput` static method declaration
  - `Private/BlueprintEditor.cpp` — `AddFunctionInput` implementation
  - `Private/BlueprintUtils.h` — `FORCEINLINE` facade wrapper
  - `Private/MCP/Tools/MCPTool_BlueprintModify.h` — operation declared, params documented
  - `Private/MCP/Tools/MCPTool_BlueprintModify.cpp` — constant, dispatch, handler

### Fixed (committed fbdaa79)
- Function graph node ops (`add_node`, `connect_pins`, `delete_node`, `set_pin_value`,
  `disconnect_pins`) now target the correct function graph when `graph_name` +
  `is_function_graph: true` are supplied. Previously all write ops defaulted to EventGraph.
- `CallFunction` node can now call Blueprint self-functions without `target_class`.
  The tool searches `FunctionGraphs` on the owning Blueprint before falling back to
  system libraries.
- `GameplayStatics` added as a named class alias so `GetAllActorsOfClass` and other
  `UGameplayStatics` functions resolve correctly.

### Fixed (committed ea9dda5)
- Blueprint Interface function creation: `add_function` now marks the entry node
  `bIsEditable = true` so the Details panel exposes the signature editor (fixes "Graph is
  not editable" / frozen signature bug).
- `inputs` array on `add_function` previously silently dropped for Interface blueprints —
  now applied via `UserDefinedPins` + `ReconstructNode`.

### Fixed (committed in BlueprintLoader.cpp, staged)
- Duplicate `create` no longer crashes or silently overwrites. `FPackageName::DoesPackageExist`
  check added before `CreatePackage` catches on-disk assets even when not loaded in memory.
  The `FindObject` guard is retained as a secondary check for in-session assets not yet saved.

---

## v1.4.5 (0de6e94)
Baseline before MyProject7 session work.
