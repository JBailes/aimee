# Proposal: C/C++ Extractor for Code Indexing

## Problem

aimee supports extractors for JS, TS, Python, Go, C#, Shell, CSS, and Dart, but not C or C++. This means `aimee index find` cannot locate definitions in aimee's own codebase or in acktng (the largest C project in the repo). Files with `.c` and `.h` extensions are silently skipped during `index scan`.

## Approach

Add a C/C++ extractor to `extractors.c` following the same pattern as the existing language extractors. The extractor needs four callbacks:

1. **c_import_line**: Extract `#include "..."` (local includes only, skip system `<...>` includes)
2. **c_export_line**: Extract non-static function declarations and type definitions visible outside the file
3. **c_def_line**: Extract function definitions, struct/union/enum/typedef declarations, and `#define` macros
4. **c_route_line**: No-op (C has no route concept)

### Changes

**`extractors.c`:**
- Add `c_exts[]` = `{".c", ".h", NULL}`
- Add `LANG_C` to the `lang_t` enum
- Add `c_import_line`, `c_export_line`, `c_def_line` callbacks
- Wire into `detect_lang`, `index_has_extractor`, and all four dispatch functions (`extract_imports`, `extract_exports`, `extract_routes`, `extract_definitions`)

### What C definitions look like

```c
// Functions (top-level, not indented)
int function_name(args)           -> definition "function_name"
static void helper(void)          -> definition "helper"
void *complex_return(int x, ...)  -> definition "complex_return"

// Types
struct foo_t { ... };             -> definition "foo_t"
typedef struct { ... } bar_t;     -> definition "bar_t"
enum color { RED, GREEN };        -> definition "color"
union data { ... };               -> definition "data"

// Macros (exported as definitions for findability)
#define MAX_SIZE 1024             -> definition "MAX_SIZE"

// Imports
#include "header.h"               -> import "header.h"
// (skip #include <stdlib.h>)

// Exports: non-static functions and type declarations in .h files
```

### Parsing approach

Line-based heuristic matching, consistent with all other extractors. No AST parsing. The patterns are:
- Lines starting with `#include "` for imports
- Lines starting with `#define ` for macro definitions
- Lines containing `struct `, `enum `, `union `, `typedef ` for type definitions
- Lines matching `type_name function_name(` at indent 0 for function definitions
- Skip lines inside block comments (`/* ... */`) via a simple state tracker

## Trade-offs

- Line-based parsing can miss multi-line function signatures. This is acceptable since all other extractors have the same limitation.
- `#define` macros will include both constants and function-like macros, which could be noisy. Worth including since macros are a primary way to find C constants.
- No special handling for preprocessor conditionals (`#ifdef`). All definitions are indexed regardless of conditional compilation.

## Testing

Add test cases to the existing test suite or create a new `test_extractors.c` that verifies:
- `#include "foo.h"` is extracted as an import
- `#include <stdio.h>` is NOT extracted
- Function definitions at top level are extracted with correct line numbers
- `struct`, `enum`, `typedef`, `#define` are extracted
- Static functions are extracted as definitions (they're still findable within the file)
