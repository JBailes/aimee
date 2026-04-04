# Proposal: Lua Extractor for Code Indexing

## Problem

acktng embeds Lua 5.4 for scripting, and WOL may adopt scripting as well. Lua files (`.lua`) are skipped during indexing, so `aimee index find` cannot locate Lua function definitions, module requires, or exported symbols.

## Approach

Add a Lua extractor to `extractors.c` following the existing pattern.

### Callbacks

1. **lua_import_line**: Extract `require("...")` and `require '...'` calls
2. **lua_export_line**: Extract module-level assignments that look like exports (e.g., `M.func_name = ...`, `return { ... }`)
3. **lua_def_line**: Extract function definitions:
   - `function name(...)` (global functions)
   - `local function name(...)` (local functions)
   - `function M.name(...)` or `function M:name(...)` (module methods)
4. **lua_route_line**: No-op

### Changes

**`extractors.c`:**
- Add `lua_exts[]` = `{".lua", NULL}`
- Add `LANG_LUA` to the `lang_t` enum
- Add the four callbacks
- Wire into all dispatch functions

### Parsing patterns

```lua
-- Imports
require("module_name")            -> import "module_name"
require 'module_name'             -> import "module_name"
local mod = require("name")      -> import "name"

-- Definitions
function global_func(args)        -> definition "global_func"
local function helper(args)       -> definition "helper"
function M.method(self, args)     -> definition "M.method"
function M:method(args)           -> definition "M:method"

-- Exports (top-level assignments to module table)
M.field = value                   -> export "field"
```

## Trade-offs

- Lua's dynamic nature means some patterns (like `M[key] = function()`) won't be caught. Line-based heuristics handle the common patterns.
- Module table name varies (`M`, `mod`, etc.), but we only need to catch `function X.Y(` and `function X:Y(` patterns.

## Testing

Verify imports, function definitions (all three forms), and exports are correctly extracted with line numbers.
