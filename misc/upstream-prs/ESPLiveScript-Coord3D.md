# ESPLiveScript: Fix `__userDefined__` types as function parameters + (optional) add `Coord3D` built-in

## Summary

Two complementary improvements, in order of priority:

1. **Bug fix (Fix A)** — `parseCreateArguments` incorrectly applies register optimisation to struct-typed parameters, corrupting the `target` field that `getVarType()` uses for member lookup. This causes a parser crash ("member not found") for any user-defined struct used as a function parameter type.

2. **New built-in (Fix B, optional)** — Add `Coord3D` (three `int` fields: `x`, `y`, `z`) as a built-in type, analogous to `CRGB`. This is needed if you want to register C++ external functions that take a `Coord3D` parameter (e.g., `addExternal("void setRGBCoord(Coord3D,CRGB)", ptr)`).

Fix A alone is sufficient to allow scripts to write helper functions with user-defined struct parameters. Fix B additionally allows the C++ host to expose functions with `Coord3D` in their signature string.

---

## Root cause of the crash

### Symptom

```
member  not foudn in Coord3D
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
  #0 copyPrty(NodeToken*, NodeToken*) at NodeToken.h:1272
  #1 createNodeVariable(Token*, bool) at NodeToken.h:1544
  ...
```

### Analysis

`NodeToken` uses the `target` field for two distinct purposes:

| Context | `target` value | Meaning |
|---|---|---|
| Register-optimised local/param | small integer (0, 1, 2…) | register slot index |
| User-defined struct variable | `EOF_TEXTARRAY` (9999) | "no member selected; return whole-struct type" |
| User-defined struct member access | text index of member name | "look up this member in the struct" |

In `NodeToken::getVarType()` (`NodeToken.h:739`):

```cpp
if (type == TokenUserDefinedVariable)
{
    if (target != EOF_TEXTARRAY)
    {
        if (getTargetText()[0] == '@')
            return &_userDefinedTypes[_vartype];        // whole struct
        int i = findMember(_vartype, getTargetText());  // look up member
        if (i < 0) { printf("member not found"); return NULL; }  // CRASH
    }
    return &_userDefinedTypes[_vartype];                // whole struct (EOF case)
}
```

`parseStatement` (`ESPLiveScript.h:1726`) correctly guards register mode for primitive types only:

```cpp
if (_for_depth_reg.get() <= _MAX_FOR_DEPTH_REG_2
    and (d == __float__ or d == __int__ or d == __s_int__
         or d == __uint8_t__ or d == __uint32_t__ or d == __uint16_t__))
{
    _is_variable_as_register.set(true);
}
```

But `parseCreateArguments` (`ESPLiveScript.h:2075`) has no such type guard:

```cpp
if (_for_depth_reg.get() <= _MAX_FOR_DEPTH_REG_2)
{
    _is_variable_as_register.set(true);  // ← applies to ALL types including structs
}
if (_is_variable_as_register.get())
{
    _nd = NodeToken(_nd, defLocalVariableNodeAsRegister);
    _nd.target = _for_depth_reg.get();  // ← corrupts target for struct params!
    _for_depth_reg.increase();
}
```

When `Coord3D p` is declared as a function parameter and `_for_depth_reg ≤ _MAX_FOR_DEPTH_REG_2`, `p`'s context entry gets `target = 0` (or 1, 2…). Later, when `p.x` is parsed, `getVarType()` sees `target != EOF_TEXTARRAY`, calls `findMember(k, all_targets.getText(0))`, finds some unrelated text at index 0, and crashes.

Local variable `Coord3D pos;` works because `parseStatement` only enables register mode for the primitive types listed above — `__userDefined__` is not in that list.

---

## Fix A — `parseCreateArguments`: guard register mode by type (recommended)

**File:** `ESPLiveScript.h`

In `parseCreateArguments`, replace the unconditional register-mode check (around line 2075) with the same type guard used in `parseStatement`. The check must happen after `parseType()` has resolved the type.

The first parameter:

```diff
-        if (_for_depth_reg.get() <= _MAX_FOR_DEPTH_REG_2)
-        {
-            _is_variable_as_register.set(true);
-        }
+        if (_for_depth_reg.get() <= _MAX_FOR_DEPTH_REG_2)
+        {
+            varTypeEnum paramType = nodeTokenList.get().getVarType()->_varType;
+            if (paramType == __float__ or paramType == __int__ or paramType == __s_int__ or
+                paramType == __uint8_t__ or paramType == __uint32_t__ or paramType == __uint16_t__)
+            {
+                _is_variable_as_register.set(true);
+            }
+        }
```

The same fix must be applied in the `while (Match(TokenComma))` loop (around line 2121), which processes subsequent parameters.

### Effect

After this fix, scripts can write:

```c
void helper(Coord3D p, CRGB c) {
    setRGBCoord(p.x, p.y, p.z, c);
}
```

without crashing. This works for any user-defined struct, not just `Coord3D`.

### Why this is safe

- Struct-typed parameters cannot fit in a single Xtensa register; register optimisation for them is incorrect to begin with.
- `parseStatement` already excludes structs from register mode — this aligns `parseCreateArguments` with the same policy.
- Existing scripts are unaffected: primitive-type parameters continue to use register optimisation as before.

---

## Fix B — Add `Coord3D` as a built-in type (optional, for external registration)

Fix A does not change how `addExternal()` parses its type strings. If the C++ host needs to register functions with `Coord3D` in the signature (e.g., `"void setRGBCoord(Coord3D,CRGB)"`), `Coord3D` must be a built-in `TokenKeywordVarType`.

### 1. `asm_struct_enum.h` — add `__Coord3D__` to the type enum

```diff
 enum varTypeEnum
 {
     __none__,
     __uint8_t__,
     __uint16_t__,
     __uint32_t__,
     __int__,
     __s_int__,
     __float__,
     __void__,
     __CRGB__,
     __CRGBW__,
     __char__,
     __Args__,
     __bool__,
+    __Coord3D__,
     __userDefined__,
     __unknown__
 };
```

### 2. `tokenizer.h` — three locations

#### 2a. Debug name array `varTypeEnumNames[]`

```diff
     "__bool__",
+    "__Coord3D__",
     "__userDefined__",
```

#### 2b. `_varTypes[]` — add the type descriptor after `__bool__`

`Coord3D` has three `int` members (4 bytes each, loaded/stored with `l32i`/`s32i`):

```diff
+    {
+        ._varType = __Coord3D__,
+        .varName = "d",
+        ._varSize = 3,
+        .load    = {l32i, l32i, l32i},
+        .store   = {s32i, s32i, s32i},
+        .membersNames = {"x", "y", "z"},
+        .starts       = {0, 4, 8},
+        .memberSize   = {1, 1, 1},
+        .types        = {__int__, __int__, __int__},
+        .sizes        = {4, 4, 4},
+        .size         = 3,
+        .total_size   = 12,
+    },
+
 };   // end of _varTypes[]
```

#### 2c. Keyword table — add `"Coord3D"` to the type-variable section

```diff
-#define nb_keywords 39
-#define nb_typeVariables 13
+#define nb_keywords 40
+#define nb_typeVariables 14
 string keyword_array[nb_keywords] =
     {"none", "uint8_t", "uint16_t", "uint32_t", "int", "s_int", "float", "void", "CRGB",
-     "CRGBW", "char", "Args", "bool", "external", "for", ...};
+     "CRGBW", "char", "Args", "bool", "Coord3D", "external", "for", ...};
```

`"Coord3D"` must be placed at index `nb_typeVariables - 1` (the last slot before the non-type keywords), so the tokenizer assigns it `TokenKeywordVarType` and maps it to `__Coord3D__` via its position in the array.

### Effect of Fix B alone (without Fix A)

`Coord3D` becomes a `TokenKeywordVarType`, not a `TokenUserDefinedVariable`. This means it bypasses the `__userDefined__` path entirely — including the register-mode collision — so the crash does not occur. Additionally, the host can call:

```cpp
addExternal("void setRGBCoord(Coord3D,CRGB)", (void*)_setRGBCoord);
```

### Notes

- `int` is 4 bytes on all ESP32 targets (Xtensa LX7 and RISC-V); `total_size = 12` is correct.
- `__userDefined__` and `__unknown__` shift by one enum value. They are only referenced by name in the codebase, not by numeric literal, so no other changes are needed.
- The `_varTypes[]` array must stay indexed by `varTypeEnum` value; the new entry at position 13 maintains that invariant.
- No changes to `ESPLivescriptRuntime.h` or `execute.h` are expected — the existing struct member load/store machinery already handles both built-in struct types and user-defined types with `l32i`/`s32i`.

---

## Recommendation

Apply **Fix A** first — it is a small, general bug fix that benefits all user-defined struct types. Apply **Fix B** if external function registration with `Coord3D` parameter types is needed on the host side.

With only Fix A, MoonLight scripts can write:

```c
void helper(Coord3D p, CRGB c) {
    setRGBCoord(p.x, p.y, p.z, c);
}
```

With Fix B additionally, the MoonLight C++ side can expose:

```cpp
static void _setRGBCoord(Coord3D pos, CRGB color) {
    gNode->layer->setRGB(pos, color);
}
// registered as: "void setRGBCoord(Coord3D,CRGB)"
```
