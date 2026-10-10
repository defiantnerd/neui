<!-- neui reference. The .nsl language specification. -->

# The neui shading language (`.nsl`), version 1

**Status: implemented by the compiler and CPU executor in `src/shader/`**
(the `neui-shader` library, API `src/shader/nsl.h`; phase 1 of
`plans/shader-effects.md`). No host exposes it yet: `NEUI_API_SHADER` and the
compound layer come in phases 2 and 3. Tests: `tests/test_shader_lang.cpp` and
`tests/test_shader_exec.cpp`.

**What is normative:** this document for syntax, types and semantics. The
exact polynomial behind each approximated built-in (§9.2) is defined by the
reference implementation (`src/shader/nsl_math.h`) and pinned by checksums. Its accuracy bounds are
given here.

## 1. Overview

A `.nsl` shader is an **image generator**: a pure function from a position
inside a layer to a colour. neui runs it once per device pixel of the layer and
draws the result like a bitmap, inside the normal paint order. A shader has no
inputs except the position and its uniforms. It has no textures, no state that
survives between pixels or frames, and no side effects.

```glsl
// ring_glow.nsl - a value-driven glow ring for a knob
uniform float value = 0.5;              // 0..1, bound to neui.param.value
uniform vec4  accent = vec4(0.2, 0.7, 1.0, 1.0);

vec4 main(vec2 p) {
  vec2  c    = neui_size * 0.5;
  float r    = min(c.x, c.y) * 0.8;
  float d    = abs(length(p - c) - r);  // distance to the ring centreline
  float ring = 1.0 - smoothstep(0.0, 1.5 * neui_px, d - 2.0);
  float glow = exp(-d * mix(0.6, 0.15, value));
  float a    = saturate(ring + glow * value);
  return vec4(accent.rgb, a * accent.a);
}
```

The language is a small subset of GLSL ES 1.00 with a few deliberate
differences (summarised in §14). Every program is fully unrolled and inlined
at compile time, so its cost is known before it runs and it always
terminates.

## 2. Source text

- **Encoding:** UTF-8. A leading byte-order mark is skipped. Non-ASCII bytes
  are allowed only inside comments.
- **Line breaks:** LF, CRLF and CR each count as one line break.
- **Comments:** `// ...` to the end of the line, and `/* ... */`, which do not
  nest. An unterminated `/*` is an error.
- **Identifiers:** `[A-Za-z_][A-Za-z0-9_]*`, at most 63 characters. Reserved
  and therefore errors: the prefixes `neui_` and `gl_`, any identifier
  containing `__`, every keyword (§13), and every built-in function name (§9).
- **Integer literals:** decimal digits only (`0`, `42`). No hex, no octal, no
  suffix. The value must fit in a signed 32-bit int.
- **Float literals:** `1.0`, `1.`, `.5`, `1e3`, `1.5e-3`, with an optional
  `f` / `F` suffix. Conversion gives the nearest binary32 value
  (round-half-to-even) and is **independent of the C locale**. A literal that
  overflows to infinity is an error. One that underflows becomes 0 or a
  denormal, per IEEE.
- **Directives:** the only directive is `#pragma neui <name>` on a line of its
  own (§11). Any other line starting with `#` is an error ("the preprocessor
  is not supported"), and so are `#define`, `#include` and `#version`.

## 3. Program structure

A program is a sequence of global declarations:

- `uniform` declarations (§6);
- `const` declarations with constant initialisers;
- function definitions (§8), including exactly one `vec4 main(vec2 p)`, whose
  parameter can have any name;
- `precision` statements (`precision highp float;`), accepted and ignored.

Everything is **declared before use**, and there are no prototypes. A
function can only call functions defined above it, and a call to itself is an
error, so there is no recursion. A local variable cannot take the name of a
function. Mutable globals do not exist: a global is `uniform` or `const`.

## 4. Types and values

| Type | Values | Notes |
|---|---|---|
| `float` | IEEE-754 binary32 | the only runtime numeric type |
| `vec2`, `vec3`, `vec4` | 2-4 floats | component-wise arithmetic |
| `bool` | `true`, `false` | from comparisons and logic |
| `int` | signed 32-bit | **compile-time only**: literals, `const int`, loop counters |
| `void` | none | function return type only |

**Every `int` value is known at compile time,** because loops are unrolled.
Int arithmetic (`+`, `-`, `*`, unary `-`) is evaluated by the compiler, and
an overflow is an error. **Int division (`/`) is an error**: GLSL's
`1/2 == 0` is a classic porting trap. Write `1.0 / 2.0`.

**Implicit conversion:** int → float only, wherever a float is expected (an
operand next to a float, an argument, an initialiser, a return value, a
constructor argument). Nothing else converts implicitly: bool never becomes a
number, and float never becomes int.

**Constructors:**
- `float(x)` for int, bool (`0.0` / `1.0`) and float;
- `vecN(s)` splats a scalar to every component;
- `vecN(a, b, ...)` consumes the components of its arguments in order, and
  their total must be exactly N;
- `vecN(v)` with a single longer vector argument keeps the first N
  components;
- `bool(...)` and `int(...)` do not exist.

**Swizzles:** `.xyzw`, `.rgba` or `.stpq` (one set per swizzle, no mixing),
1-4 components, on vectors only. A swizzle used as an assignment target may
not repeat a component.

**Uninitialised locals and `out` parameters are zero** (GLSL leaves them
undefined).

## 5. Variables and scope

- Block scoping as in GLSL. A `for` init variable is scoped to its loop.
  Function parameters live in the function's outermost block.
- Redeclaring a name in the same scope is an error. Shadowing a name from an
  enclosing scope (including a uniform or const) is allowed.
- `const` locals and globals need a constant initialiser: literals, other
  consts, constructors, and built-ins applied to constants. Uniforms are not
  constant.

## 6. Uniforms

```glsl
uniform float value;                    // default 0.0
uniform float radius = 0.4;             // default value (extension over GLSL)
uniform vec4  accent = vec4(1.0, 0.5, 0.0, 1.0);
```

- **Types:** `float`, `vec2`, `vec3`, `vec4`. The default must be a constant
  expression. Without one, the uniform starts at zero.
- **Values come from neui**, through the compound layer's `u.<name>`
  properties and bindings, `shader_api->draw`, or the bake API. A float
  attribute sets a `float` uniform, or one component through `u.<name>.x`.
- **Colours:** an int ARGB value set on a `vec4` uniform arrives as
  **straight RGBA**, each component computed as `float(byte) / 255.0`
  (IEEE division, exact rule). A colour never travels through a float
  attribute, so all 32 bits survive.
- **Reserved uniforms** are implicitly declared and read-only:

| Name | Type | Value |
|---|---|---|
| `neui_size` | `vec2` | layer size in logical px |
| `neui_scale` | `float` | device px per logical px (display scale × layer `resolution`) |
| `neui_px` | `float` | `1.0 / neui_scale`: one device pixel in logical px, for anti-aliased edges |
| `neui_time` | `float` | seconds on the layer's animation clock, wrapped to `[0, 3600)` |

**Time** is wrapped because DAW sessions run for hours and a float loses
precision as it grows. 3600 has many divisors, so loops whose period divides
it stay seamless: `sin(neui_time * 6.2831853 / 4.0)` repeats exactly every 4
s. A program that reads `neui_time` is time-dependent: it animates (phase 6)
and is re-run when the clock ticks. A program that does not read it is re-run
only when its inputs change.

## 7. Expressions

Operators, from highest to lowest precedence:

| Precedence | Operators | Operands |
|---|---|---|
| postfix | `f(...)`, `T(...)`, `.swizzle`, `( )` | |
| unary | `+`, `-`, `!` | `-` on float / vec / int; `!` on bool |
| multiplicative | `*`, `/` | float / vec, component-wise; vec op float and float op vec broadcast |
| additive | `+`, `-` | as multiplicative |
| relational | `<`, `>`, `<=`, `>=` | **scalars only**, result bool |
| equality | `==`, `!=` | any two values of the same type, result bool (vectors: all components) |
| logical | `&&`, then `^^`, then `\|\|` | bool |
| conditional | `c ? a : b` | `c` bool; `a`, `b` the same type |

**Evaluation order and exactness:**
- Operands are evaluated left to right, and **every operation is performed
  exactly as written**: no reassociation, no FMA contraction, no algebraic
  rewriting that could change a bit (`x / c` never becomes `x * (1/c)`). The
  only rewrites allowed are exact ones (`x * 1.0` → `x`, constant folding with
  the executor's own arithmetic).
- `&&`, `||` and `?:` may evaluate both sides. Expressions have no side
  effects, so the result is the same.

**Assignment is a statement, not an expression**: `=`, `+=`, `-=`, `*=`,
`/=`, optionally to a swizzle (`c.rg = ...`). `++` and `--` exist only in a
`for` update. There is no `%` (use `mod`), and there are no bitwise operators.

## 8. Statements and functions

**Statements:** block `{ }`, declaration, assignment, function call (for
`out` / `inout` results), `if` / `else`, `for`, `break`, `continue`,
`return`, and the empty statement `;`. `if`, `?:` and loop conditions need a
`bool`.

**`for` loops have a compile-time trip count:**

```
for (int i = A; i OP B; UPDATE) body
```

- `A`, `B` and the step `C` are constant int expressions;
- `OP` is one of `<` `<=` `>` `>=` `!=`;
- `UPDATE` is `i++`, `++i`, `i--`, `--i`, `i += C` or `i -= C`, with `C > 0`;
- the trip count must be between 0 and 64, and a loop that would not
  terminate is an error;
- the body may read `i` (an int, so `float(i)` or implicit conversion in float
  context) but may not assign it;
- `break` and `continue` are allowed. The loop is still unrolled to its full
  trip count, and pixels that have left it are masked.

`while`, `do` and `switch` are not in v1.

**Functions:**

```glsl
float ring(vec2 p, vec2 c, float r) { return abs(length(p - c) - r); }
void  polar(vec2 v, out float radius, out float angle) { radius = length(v); angle = atan(v.y, v.x); }
```

- Return type: `float`, `vecN`, `bool` or `void`. Parameters: `float`, `vecN`,
  `bool` or `int`, with the qualifier `in` (default), `out` or `inout`.
- Every path of a non-void function must end in `return`; otherwise it is an
  error.
- Calls are **inlined**. Arguments are evaluated left to right, then copied in.
  `out` / `inout` results are copied back in parameter order after the body,
  which is GLSL semantics.
- User functions **cannot be overloaded**, and their names cannot be those of
  built-ins.
- `main` is the entry point. It cannot be called.

## 9. Built-in functions

`genType` means `float`, `vec2`, `vec3` or `vec4`, applied component-wise.
Where a parameter is written `float` next to `genType`, a scalar is broadcast.

### 9.1 Exact built-ins

These are composites of IEEE operations (each correctly rounded), evaluated
in exactly the order given, so every executor produces the same bits.

| Function | Definition |
|---|---|
| `abs(x)` | clear the sign bit |
| `sign(x)` | `x > 0 ? 1 : (x < 0 ? -1 : 0)`; NaN → 0 |
| `floor(x)` | exact; `floor(-0.0)` is `-0.0` |
| `ceil(x)` | `-floor(-x)` |
| `fract(x)` | `x - floor(x)`; can round to `1.0` for tiny negative `x` |
| `mod(x, y)` | `x - y * floor(x / y)`, **floored** (never HLSL `fmod`); `mod(x, 0)` is NaN |
| `min(x, y)` | `y < x ? y : x` |
| `max(x, y)` | `x < y ? y : x` |
| `clamp(x, lo, hi)` | `min(max(x, lo), hi)`; `lo > hi` gives `hi` |
| `saturate(x)` | `clamp(x, 0.0, 1.0)` |
| `mix(x, y, a)` | `x * (1 - a) + y * a` (exact at `a = 0` and `a = 1`) |
| `step(e, x)` | `x < e ? 0 : 1` |
| `smoothstep(e0, e1, x)` | `e0 == e1`: `step(e0, x)`; else `t = clamp((x - e0) / (e1 - e0), 0, 1)`, then `(t * t) * (3 - 2 * t)` |
| `sqrt(x)` | IEEE; `x < 0` → NaN |
| `inversesqrt(x)` | `1 / sqrt(x)` |
| `dot(a, b)` | `((a.x*b.x + a.y*b.y) + a.z*b.z) + a.w*b.w`, left to right |
| `length(v)` | `sqrt(dot(v, v))` |
| `distance(a, b)` | `length(a - b)` |
| `normalize(v)` | `dot(v, v) == 0` → zero vector; else `v * inversesqrt(dot(v, v))` |
| `cross(a, b)` | `vec3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x)` |
| `reflect(i, n)` | `i - (2 * dot(n, i)) * n` |
| `radians(x)`, `degrees(x)` | `x * 0.017453292`, `x * 57.29578` |

### 9.2 Approximated built-ins

neui ships its own polynomials instead of libm or GPU intrinsics. They use
only the operations from §9.1 and float↔int conversions of in-range values,
so they are bit-identical on every CPU executor. The GPU emitters write the
same polynomials (§12).

| Function | Definition | Accuracy (measured in phase 0) |
|---|---|---|
| `sin(x)`, `cos(x)` | Cody-Waite reduction by π/2 (three-part constant), Cephes polynomials on [-π/4, π/4] | abs ≤ 1e-7 for \|x\| ≤ 64, degrading for larger \|x\| |
| `tan(x)` | `sin(x) / cos(x)` | from sin / cos |
| `exp2(x)` | reduction to [-0.5, 0.5) + degree-6 polynomial; `x < -126` → 0 (never a denormal); `x ≥ 128` → +inf | rel ≤ 1.5e-7 |
| `log2(x)` | exponent + atanh series on [√½, √2]; denormals exact; `x < 0` → NaN; `0` → -inf | abs ≤ 1.5e-7 for \|result\| ≤ 1, rel beyond |
| `exp(x)` | `exp2(x * 1.4426950)` | from exp2 |
| `log(x)` | `log2(x) * 0.6931472` | from log2 |
| `pow(x, y)` | `x > 0`: `exp2(y * log2(x))`; `x == 0`: `y > 0` → 0, `y == 0` → 1, `y < 0` → +inf; `x < 0` → NaN | from exp2 / log2 |
| `atan(y, x)` | reduction to [0, 1] and [-tan(π/8), tan(π/8)], Cephes polynomial; `atan(0, 0)` = 0; a NaN argument, or both arguments infinite, gives NaN | abs ≤ 3e-7 |
| `atan(t)` | `atan(t, 1.0)` | |
| `asin(x)` | `atan(x, sqrt(1 - x*x))`; \|x\| > 1 → NaN | |
| `acos(x)` | `atan(sqrt(1 - x*x), x)`; \|x\| > 1 → NaN | |

### 9.3 Noise

The usual `fract(sin(x) * 43758.5)` hash works, but `sin` of a large argument
amplifies the last bit, so its result is identical on every CPU executor and
only close on a GPU. The noise built-ins are **integer-exact**, so they match
on every executor, GPU included (SM4 has integer operations).

**`float neui_hash(vec2 p)`** → [0, 1):

```
coord(v): v = (v != v) ? 0 : v; v = floor(v); v = clamp(v, -16777216, 16777216);
          return uint(int(v))                                   // two's complement
h  = coord(p.x) * 0x8DA6B343u  ^  coord(p.y) * 0xD8163841u      // 32-bit wrapping
h ^= h >> 16;  h *= 0x7FEB352Du;
h ^= h >> 15;  h *= 0x846CA68Bu;
h ^= h >> 16;
return float(h >> 8) * (1.0 / 16777216.0)
```

**`float neui_noise(vec2 p)`** → [0, 1), value noise:

```
i = floor(p);  f = p - i;  u = f * f * (3 - 2 * f);
a = neui_hash(i);              b = neui_hash(i + vec2(1, 0));
c = neui_hash(i + vec2(0, 1)); d = neui_hash(i + vec2(1, 1));
return mix(mix(a, b, u.x), mix(c, d, u.x), u.y)
```

## 10. Arithmetic model

- **IEEE-754 binary32**, round-to-nearest-even, **gradual underflow**
  (denormals are kept). No FMA contraction.
- **The CPU executor pins this environment** for the duration of a draw, then
  restores the caller's. A DAW may run its UI thread with flush-to-zero or
  denormals-are-zero set, and phase 0 showed that this changes results
  (`log2` of a denormal) unless the environment is pinned.
- **NaN and infinity** follow IEEE through arithmetic. Comparisons with NaN
  are false, and `!=` is true. A NaN's sign and payload are **not
  observable**: no operation reads float bits, and the output rule maps NaN to
  0. This matters because x86 and ARM generate NaNs with different bits.
- **Division by zero** is IEEE: `x / 0` is ±inf, and `0 / 0` is NaN.

## 11. Entry point, pixel grid and output

**Pixel grid.** A layer at logical rect `(x, y, w, h)`, drawn under the
current translation `(tx, ty)` at effective scale `s` (`neui_scale`), covers
the device rect

```
X0 = (x + tx) * s    X1 = X0 + w * s    (and Y0, Y1 likewise)
```

The executor evaluates every device pixel column `i` from `floor(X0)` to
`ceil(X1) - 1`, and every row `j` likewise, at

```
p = ( ((i - floor(X0)) + 0.5 - (X0 - floor(X0))) / s ,
      ((j - floor(Y0)) + 0.5 - (Y0 - floor(Y0))) / s )
```

In words: `p` is the **centre of the device pixel**, in **layer-local
logical px**, **y-down**. The result covers whole device pixels and is never
resampled. The phase-0 D2D spike confirmed that a GPU executor reproduces
this grid exactly (approach B: draw at the floored device origin and pass the
sub-pixel remainder `X0 - floor(X0)` as a constant).

**Output.** `main` returns **straight** (non-premultiplied) RGBA. Each device
pixel becomes premultiplied 8-bit BGRA:

```
a  = clamp01(ret.a) * coverage(i, j)
c  = clamp01(ret.c) * a                        for c in r, g, b
a8 = floor(a * 255 + 0.5)
c8 = min(floor(c * 255 + d(i, j)), a8)         d = 0.5 unless dithering
```

- `clamp01(v)` is `v > 0 ? (v < 1 ? v : 1) : 0`, so NaN maps to 0, +inf to 1
  and -inf to 0.
- `coverage(i, j) = cx(i) * cy(j)`, where `cx(i)` is the overlap of
  `[i, i + 1)` with `[X0, X1)`. It is 1 inside the layer and fractional only
  on the edge pixels of a layer that does not sit on whole device pixels, so
  shader layers get the same anti-aliased edge as vector drawing.
- A premultiplied channel never exceeds alpha.

**Dithering** (`#pragma neui dither`) replaces `d` with an ordered 4×4 Bayer
offset, which hides banding in large, slow gradients:

```
B = [ 0  8  2 10
     12  4 14  6
      3 11  1  9
     15  7 13  5 ]
d(i, j) = (B[(j - floor(Y0)) & 3][(i - floor(X0)) & 3] + 0.5) / 16
```

The pattern is anchored to the layer, so it does not crawl when the layer
moves.

**Pragmas:**

| Pragma | Effect |
|---|---|
| `#pragma neui dither` | ordered dithering on output |
| `#pragma neui cpu_only` | never use a GPU executor for this shader (exact pixels everywhere, at CPU speed) |

## 12. Determinism and fidelity

- **CPU executor:** bit-identical on every platform, for every program.
  Phase 0 confirmed this for AppleClang arm64, MSVC x64, GCC 13 x64 and
  Clang 18 x64, over the built-in sweeps (including denormals and specials)
  and four test programs at 1×, 1.5×, 2× and a fractional, dithered origin.
  CI pins it on every platform (`neui_tests`, `test_shader_exec.cpp`).
  Implementation rules:
  - shader arithmetic, including the compiler's constant folding, is
    compiled in **one** library (`src/shader/`, `neui-shader`) with
    `-ffp-contract=off` (MSVC: `/fp:precise`), optimised in every build type;
  - no libm;
  - the FP environment is pinned per draw and per compile.
- **GPU executor (D2D):** emits neui's own polynomials and the same pixel
  grid, but GPUs may fuse multiply-adds, flush denormals and round division
  differently. Contract: within 2/255 per channel on ≥ 99.9% of pixels, and
  within 8/255 on all pixels. `#pragma neui cpu_only` and a session switch
  force the CPU path when exact pixels matter more than speed.

## 13. Limits

A program that exceeds a limit does not compile, and the diagnostic points at
the construct that crossed it. Limits keep the cost of a shader bounded and
known at compile time, which protects against untrusted skins and against a
GPU timeout (TDR on Windows).

| Limit | Value |
|---|---|
| source size | 64 KiB |
| identifier length | 63 |
| nesting depth (statements, blocks and expressions) | 128 |
| nesting depth after inlining | 512 |
| user functions | 64 |
| uniforms | 32 declarations, 64 scalar components |
| `for` trip count | 64 |
| `for` nesting depth | 4 |
| product of nested trip counts | 1024 |
| scalar operations after inlining, unrolling and folding | 8192 |
| simultaneously live scalar values | 256 |
| compile work: statements and expressions lowered after inlining and unrolling | 1,048,576 |

Nesting after inlining and compile work bound the compiler itself, its stack
and its running time. Inlining multiplies work (a
function that calls another twice, which calls another twice, ...), and the
optimiser can fold the result down to a few operations, so neither the source
limits nor the operation limit would catch it.

## 14. Keywords and differences from GLSL ES 1.00

**Keywords:** `uniform const float int bool vec2 vec3 vec4 void true false
if else for break continue return in out inout precision lowp mediump highp`.

**Reserved, an error with a "not supported in .nsl v1" message:** `while do
switch case default discard struct attribute varying layout centroid flat
smooth invariant mat2 mat3 mat4 mat2x2 mat2x3 mat2x4 mat3x2 mat3x3 mat3x4
mat4x2 mat4x3 mat4x4 ivec2 ivec3 ivec4 bvec2 bvec3 bvec4 uvec2 uvec3 uvec4
uint double dvec2 dvec3 dvec4 half sampler2D sampler3D samplerCube texture
texture2D textureCube asm class union enum typedef template this goto inline
noinline volatile public static extern interface long short unsigned sizeof
cast namespace using`.

**Differences from GLSL ES 1.00, in one place:**
- Entry point `vec4 main(vec2 p)` returns the colour; there is no
  `gl_FragColor` / `gl_FragCoord`.
- Uniform defaults; reserved `neui_*` uniforms and built-ins.
- `int` is compile-time only, and int `/` is an error. Implicit int → float.
- Uninitialised values are zero. Every edge case is defined (§9, §10, §11).
- No preprocessor except `#pragma neui`. No arrays, structs, matrices,
  textures, derivatives, `while`, `discard`, mutable globals or user
  overloading.
- Assignment is a statement. Loops have a constant trip count.
- Added: `saturate`.

## 15. Diagnostics

```
knob.nsl:12:17: error: 'value' is not declared
knob.nsl:3:1: error: int division; write 1.0 / 2.0
knob.nsl:20:5: error: loop runs 128 times; the limit is 64
```

- Format: `<name>:<line>:<col>: error: <message>`. `<name>` is the shader's
  file or resource name, or `<source>` for inline source.
- Lines and columns are 1-based, and columns count bytes (a tab is 1).
- v1 reports the first error only.

## 16. Porting notes

**From Shadertoy / GLSL:**

| Shadertoy | `.nsl` |
|---|---|
| `void mainImage(out vec4 o, in vec2 fragCoord)` | `vec4 main(vec2 p)` returning the colour |
| `fragCoord` (device px, **y-up**) | `p` (logical px, **y-down**); flip with `neui_size.y - p.y` |
| `iResolution.xy` | `neui_size` (logical) or `neui_size * neui_scale` (device) |
| `iTime` | `neui_time` (wrapped to an hour) |
| `iChannel0`, `texture(...)` | not in v1 |
| `while (...)`, data-dependent loops | a `for` with a constant bound + `break` |
| colour output | straight alpha; neui premultiplies |

**From HLSL:** `float4` → `vec4`, `lerp` → `mix`, `frac` → `fract`,
`atan2(y, x)` → `atan(y, x)`, `rsqrt` → `inversesqrt`; `saturate` is the
same. **`fmod` is not `mod`**: HLSL truncates, and `.nsl` floors (they differ
for negative operands). No `mul`, because there are no matrices.

**From bgfx:** see "bgfx source compatibility" in `plans/shader-effects.md`.
bgfx shader bodies are GLSL, so porting is mechanical.

## 17. Grammar

```ebnf
program        = { global } ;
global         = uniform_decl | const_decl | function | precision_stmt ;
uniform_decl   = "uniform" [ precision ] value_type IDENT [ "=" expr ] ";" ;
const_decl     = "const" [ precision ] type IDENT "=" expr { "," IDENT "=" expr } ";" ;
precision_stmt = "precision" precision "float" ";" ;
precision      = "lowp" | "mediump" | "highp" ;
function       = ( type | "void" ) IDENT "(" [ param { "," param } ] ")" block ;
param          = [ "in" | "out" | "inout" ] [ precision ] type IDENT ;
type           = value_type | "bool" | "int" ;
value_type     = "float" | "vec2" | "vec3" | "vec4" ;

block          = "{" { statement } "}" ;
statement      = block | var_decl | assign | call_stmt | if_stmt | for_stmt
               | "break" ";" | "continue" ";" | "return" [ expr ] ";" | ";" ;
var_decl       = [ "const" ] [ precision ] type IDENT [ "=" expr ]
                 { "," IDENT [ "=" expr ] } ";" ;
assign         = lvalue ( "=" | "+=" | "-=" | "*=" | "/=" ) expr ";" ;
lvalue         = IDENT [ "." SWIZZLE ] ;
call_stmt      = call ";" ;
if_stmt        = "if" "(" expr ")" statement [ "else" statement ] ;
for_stmt       = "for" "(" "int" IDENT "=" expr ";" IDENT relop expr ";" update ")" statement ;
update         = IDENT ( "++" | "--" ) | ( "++" | "--" ) IDENT
               | IDENT ( "+=" | "-=" ) expr ;

expr           = logical_or [ "?" expr ":" expr ] ;
logical_or     = logical_xor { "||" logical_xor } ;
logical_xor    = logical_and { "^^" logical_and } ;
logical_and    = equality { "&&" equality } ;
equality       = relational { ( "==" | "!=" ) relational } ;
relational     = additive [ relop additive ] ;
relop          = "<" | ">" | "<=" | ">=" | "!=" ;
additive       = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = unary { ( "*" | "/" ) unary } ;
unary          = [ "+" | "-" | "!" ] unary | postfix ;
postfix        = primary { "." SWIZZLE } ;
primary        = INT_LITERAL | FLOAT_LITERAL | "true" | "false" | IDENT
               | call | "(" expr ")" ;
call           = ( IDENT | type ) "(" [ expr { "," expr } ] ")" ;
```

`relop` includes `!=` only in a `for` condition. In expressions, `!=` is
parsed by `equality`.
