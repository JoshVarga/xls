# DSLX Built-In Functions and Standard Library

This page documents the DSLX **built-in functions** (i.e. functions that don't
require an import and are available in the top level namespace) and **standard
library modules** (i.e. which are imported with an unqualified name like `import
std`).

Generally built-in functions have some capabilities that cannot be easily done
in a user-defined function, and thus do not live in the standard library.

[TOC]

## Built-ins

This section describes the built-in functions provided for use in the DSL that
do not need to be explicitly imported.

Some of the built-ins have exlamation marks at the end; e.g. `assert!` --
exclamation marks in DSLX indicate built-in functions that have special
facilities that normal functions are not capable of. This is intended to help
the user discern when they're calling "something that will behave like a normal
function" versus "something that has special abilities beyond normal functions,
e.g. special side-effects".

NOTE: A brief note on "Parallel Primitives": the DSL is expected to grow
additional support for use of high-level parallel primitives over time, adding
operators for order-insensitive reductions, scans, groupings, and similar. By
making these operations known to the compiler in their high level form, we
potentially enable optimizations and analyses on their higher level ("lifted")
form. As of now, `map` is the sole parallel-primitive-oriented built-in.

### `add_with_carry`

Operation that produces the result of the add, as well as the carry bit as an
output. The binary add operators works similar to software programming
languages, preserving the length of the input operands, so this built-in can
assist when easy access to the carry out value is desired. Has the following
signature:

```
fn add_with_carry<N>(x: uN[N], y: uN[N]) -> (u1, uN[N])
```

### `widening_cast` and `checked_cast`

`widening_cast` and `checked_cast` cast bits-type values to bits-type values
with additional checks compared to casting with `as`.

`widening_cast` will report a static error if the type casted to is unable to
respresent all values of the type casted from (ex. `widening_cast<u5>(s3:0)`
will fail because s3:-1 cannot be respresented as an unsigned number).

`checked_cast` will cause a runtime error during dslx interpretation if the
value being casted is unable to fit within the type casted to (ex.
`checked_cast<u5>(s3:0)` will succeed while `checked_cast<u5>(s3:-1)` will cause
the dslx interpreter to fail.

Currently both `widening_cast` and `checked_cast` will lower into a normal IR
cast and will not generate additional assertions at the IR or Verilog level.

```
fn widening_cast<sN[N]>(value: uN[M]) -> sN[N]; where N > M
fn widening_cast<sN[N]>(value: sN[M]) -> sN[N]; where N >= M
fn widening_cast<uN[N]>(value: uN[M]) -> uN[N]; where N >= M

fn checked_cast<xN[M]>(value: uN[N]) -> xN[M]
fn checked_cast<xN[M]>(value: sN[N]) -> xN[M]
```

### `smulp` and `umulp`

`smulp` and `umulp` perform signed and unsigned partial multiplications. These
operations return a two-element tuple with the property that the sum of the two
elements is equal to the product of the original inputs. Performing a partial
multiplication allows for a pipeline stage in the middle of a multiply. These
operations have the following signatures:

```
fn smulp<N>(lhs: sN[N], rhs: sN[N]) -> (sN[N], sN[N])
fn umulp<N>(lhs: uN[N], rhs: uN[N]) -> (uN[N], uN[N])
```

### `map`

`map`, similarly to other languages, executes a transformation function on all
the elements of an original array to produce the resulting "mapped' array.
[For example](https://github.com/google/xls/tree/main/xls/dslx/tests/map_of_stdlib_parametric.x):
taking the absolute value of each element in an input array:

```dslx
import std;

fn main(x: s3[3]) -> s3[3] {
  let y: s3[3] = map(x, std::abs);
  y
}

#[test]
fn main_test() {
  let got: s3[3] = main(s3[3]:[-1, 1, 0]);
  assert_eq(s3[3]:[1, 1, 0], got)
}
```

Note that map is special, in that we can pass it a callee *as if* it were a
value. As a function that "takes" a function as an argument, `map` is a special
built-in -- in language implementor parlance it is a *higher order function*.

Implementation note: Functions are not first class values in the DSL, so the
name of the function must be referred to directly.

Note: Novel higher order functions (e.g. if a user wanted to write their own
`map`) cannot currently be written in user-level DSL code.

### `zip`

`zip` places elements of two same-sized arrays together in an array of 2-tuples.

Its signature is: `zip(lhs: T[N], rhs: U[N]) -> (T, U)[N]`.

```dslx
#[test]
fn test_zip_array_size_1() {
    const LHS = u8[1]:[42];
    const RHS = u16[1]:[64];
    const WANT = (u8, u16)[1]:[(42, 64)]
    assert_eq(zip(LHS, RHS), WANT);
}

#[test]
fn test_zip_array_size_2() {
    assert_eq(zip(u32[2]:[1, 2], u64[2]:[10, 11]), (u32, u64)[2]:[(1, 10), (2, 11)]);
}
```

### `array_rev`

`array_rev` reverses the elements of an array.

```dslx
#[test]
fn test_array_rev() {
  assert_eq(array_rev(u8[1]:[42]), u8[1]:[42]);
  assert_eq(array_rev(u3[2]:[1, 2]), u3[2]:[2, 1]);
  assert_eq(array_rev(u3[3]:[2, 3, 4]), u3[3]:[4, 3, 2]);
  assert_eq(array_rev(u4[3]:[0xf, 0, 0]), u4[3]:[0, 0, 0xf]);
  assert_eq(array_rev(u3[0]:[]), u3[0]:[]);
}
```

### `const_assert!`

Performs a check on a constant expression that only fails at compile-time (not
at runtime).

Note that this can only be applied to compile-time-constant expressions.

```dslx
#[test]
fn test_const_assert() {
  const N = u32:42;
  const_assert!(N >= u32:1<<5);
}
```

Keep in mind that all `const_assert!`s in a function evaluate, similar to
`static_assert` in C++ -- they are effectively part of the type system, so you
cannot suppress them by putting them in a conditional:

```dslx-snippet
fn f() {
  if false {
    const_assert!(false);  // <-- still fails even inside the "if false"
  }
}
```

### `clz`, `ctz`

DSLX provides the common "count leading zeroes" and "count trailing zeroes"
functions:

```dslx-snippet
  let x0 = u32:0x0FFFFFF8;
  let x1 = clz(x0);
  let x2 = ctz(x0);
  assert_eq(u32:4, x1);
  assert_eq(u32:3, x2)
```

### `decode`

Converts a binary-encoded value into a one-hot value. For an operand value of
`n``interpreted as an unsigned number, the`n`-th result bit and only the`n`-th
result bit is set. Has the following signature:

```
fn decode<uN[W]>(x: uN[N]) -> uN[W]
```

The width of the decode operation may be less than the maximum value expressible
by the input (`2**N - 1`). If the encoded operand value is larger than the
number of bits of the result, the result is zero.

Example usage:
[`dslx/tests/decode.x`](https://github.com/google/xls/tree/main/xls/dslx/tests/decode.x).

See also the
[IR semantics for the `decode` op](./ir_semantics.md#decode).

### `encode`

Converts a one-hot value to a binary-encoded value of the "hot" bit of the
input. If the `n`-th bit and only the `n`-th bit of the operand is set, the
result is equal to the value `n` as an unsigned number. Has the following
signature:

```
fn encode(x: uN[N]) -> uN[ceil(log2(N))]
```

If multiple bits of the input are set, the result is equal to the logical or of
the results produced by the input bits individually. For example, if bit 3 and
bit 5 of an encode input are set the result is equal to `3 | 5 = 7`.

Example usage:
[`dslx/tests/encode.x`](https://github.com/google/xls/tree/main/xls/dslx/tests/encode.x).

See also the
[IR semantics for the `encode` op](./ir_semantics.md#encode).

### `one_hot`

Converts a value to one-hot form. Has the following signature:

```
fn one_hot<N, NP1=N+1>(x: uN[N], lsb_is_prio: bool) -> uN[NP1]
```

When `lsb_is_prio` is true, the least significant bit that is set becomes the
one-hot bit in the result. When it is false, the most significant bit that is
set becomes the one-hot bit in the result.

When all bits in the input are unset, the additional bit present in the output
value (MSb) becomes set.

Example usage:
[`dslx/tests/one_hot.x`](https://github.com/google/xls/tree/main/xls/dslx/tests/one_hot.x).

See also the
[IR semantics for the `one_hot` op](./ir_semantics.md#one_hot).

### `one_hot_sel`

Produces the result of 'or'-ing all case values for which the corresponding bit
of the selector is enabled. In cases where the selector has exactly one bit set
(it is in one-hot form) this is equivalent to a match.

```
fn one_hot_sel<N, M>(selector: uN[N], cases: uN[N][M]) -> uN[N]
```

Evaluates each case value and `or`s each case together if the corresponding bit
in the selector is set. The first element of `cases` is included if the LSB is
set, the second if the next least significant bit and so on. If no selector bits
are set this evaluates to zero. This function is not generally used directly
though the compiler will when possible synthesize the equivalent code from a
`match` expression. This is included for testing purposes and for bespoke
'intrinsic-style programming' use cases.

### `signex`

Casting has well-defined extension rules, but in some cases it is necessary to
be explicit about sign-extensions, if just for code readability. For this, there
is the `signex` built-in.

To invoke the `signex` built-in, provide it with the operand to sign extend
(lhs), as well as the target type to extend to: these operands may be either
signed or unsigned. Note that the *value* of the right hand side is ignored,
only its type is used to determine the result type of the sign extension.

```dslx
#[test]
fn test_signex() {
  let x = u8:0xff;
  let s: s32 = signex(x, s32:0);
  let u: u32 = signex(x, u32:0);
  assert_eq(s as u32, u)
}
```

Note that both `s` and `u` contain the same bits in the above example.

### slice

Array-slice built-in operation. Note that the "want" argument is *not* used as a
value, but is just used to reflect the desired slice type. (Prior to constexprs
being passed to built-in functions, this was the canonical way to reflect a
constexpr in the type system.) Has the following signature:

```
fn slice<T: type, N, M, S>(xs: T[N], start: uN[M], want: T[S]) -> T[S]
```

### `rev`

`rev` is used to reverse the bits in an unsigned bits value. The LSb in the
input becomes the MSb in the result, the 2nd LSb becomes the 2nd MSb in the
result, and so on.

```dslx
// (Dummy) wrapper around reverse.
fn wrapper<N: u32>(x: bits[N]) -> bits[N] {
  rev(x)
}

// Target for IR conversion that works on u3s.
fn main(x: u3) -> u3 {
  wrapper(x)
}

// Reverse examples.
#[test]
fn test_reverse() {
  assert_eq(u3:0b100, main(u3:0b001));
  assert_eq(u3:0b001, main(u3:0b100));
  assert_eq(bits[0]:0, rev(bits[0]:0));
  assert_eq(u1:1, rev(u1:1));
  assert_eq(u2:0b10, rev(u2:0b01));
  assert_eq(u2:0b00, rev(u2:0b00));
}
```

### `bit_slice_update`

`bit_slice_update(subject, start, value)` returns a copy of the bits-typed value
`subject` where the contiguous bits starting at index `start` (where 0 is the
least-significant bit) are replaced with `value`. The bit-width of the returned
value is the same as the bit-width of `subject`. Any updated bit indices which
are out of bounds (if `start + bit-width(value) >= bit-width(subject)`) are
ignored. Example usage:
[`dslx/tests/bit_slice_update.x`](https://github.com/google/xls/tree/main/xls/dslx/tests/bit_slice_update.x).

### bit-wise reductions: `and_reduce`, `or_reduce`, `xor_reduce`

These are unary reduction operations applied to a bits-typed value:

*   `and_reduce`: evaluates to bool:1 if all bits of the input are set, and 0
    otherwise.
*   `or_reduce`: evaluates to bool:1 if any bit of the input is set, and 0
    otherwise.
*   `xor_reduce`: evaluates to bool:1 if there is an odd number of bits set in
    the input, and 0 otherwise.

These functions return the identity element of the respective operation for
trivial (0 bit wide) inputs:

```dslx
#[test]
fn test_trivial_reduce() {
  assert_eq(and_reduce(bits[0]:0), true);
  assert_eq(or_reduce(bits[0]:0), false);
  assert_eq(xor_reduce(bits[0]:0), false);
}
```

### `update`

`update(array, index, new_value)` returns a copy of `array` where `array[index]`
has been replaced with `new_value`, and all other elements are unchanged. Note
that this is *not* an in-place update of the array, it is an "evolution" of
`array`. It is the compiler's responsibility to optimize by using mutation
instead of copying, when it's safe to do. The compiler makes a best effort to do
this, but can't guarantee the optimization is always made.

### `assert_eq`, `assert_lt`

In a unit test pseudo function all valid DSLX code is allowed. To evaluate test
results DSLX provides the `assert_eq` primitive (we'll add more of those in the
future). Here is an example of a `divceil` implementation with its corresponding
tests:

```dslx
fn divceil(x: u32, y: u32) -> u32 {
  (x-u32:1) / y + u32:1
}

#[test]
fn test_divceil() {
  assert_eq(u32:3, divceil(u32:5, u32:2));
  assert_eq(u32:2, divceil(u32:4, u32:2));
  assert_eq(u32:2, divceil(u32:3, u32:2));
  assert_eq(u32:1, divceil(u32:2, u32:2));
}
```

`assert_eq` cannot currently be synthesized into equivalent Verilog. Because of
that it is recommended to use it within `test` constructs (interpretation) only.

### `zero!<T>`

DSLX has a macro for easy creation of zero values, even from aggregate types.
Invoke the macro with the type parameter as follows:

```dslx
struct MyPoint {
  x: u32,
  y: u32,
}

enum MyEnum : u2 {
  ZERO = u2:0,
  ONE = u2:1,
}

#[test]
fn test_zero_macro() {
  assert_eq(zero!<u32>(), u32:0);
  assert_eq(zero!<MyPoint>(), MyPoint{x: u32:0, y: u32:0});
  assert_eq(zero!<MyEnum>(), MyEnum::ZERO);
}
```

The `zero!<T>` macro can also be used with the struct update syntax to
initialize a subset of fields to zero. In the example below all fields except
`foo` are initialized to zero in the struct returned by `f`.

```dslx
struct MyStruct {
  foo: u1,
  bar: u2,
  baz: u3,
  bat: u4,
}

fn f() -> MyStruct {
  MyStruct{foo: u1:1, ..zero!<MyStruct>()}
}
```

### `trace_fmt!`

DSLX supports printf-style debugging via the `trace_fmt!` builtin, which allows
dumping of current values to stdout. For example:

```dslx
// Note: to see `trace_fmt!` output you need to be seeing `INFO` level logging,
// enabled by adding the '--alsologtostderr' flag to the command line (among
// other means). For example:
// bazel run -c opt //xls/dslx:interpreter_main  /path/to/dslx/file.x -- --alsologtostderr

fn shifty(x: u8, y: u3) -> u8 {
  trace_fmt!("x: {:x} y: {}", x, y);
  // Note: y looks different as a negative number when the high bit is set.
  trace_fmt!("y as s8: {}", y as s2);
  x << y
}

#[test]
fn test_shifty() {
  assert_eq(shifty(u8:0x42, u3:4), u8:0x20);
  assert_eq(shifty(u8:0x42, u3:7), u8:0);
}
```

would produce the following output, with each trace being annotated with its
corresponding source position:

```
[...]
[ RUN UNITTEST  ] test_shifty
I0510 14:31:17.516227 1247677 bytecode_interpreter.cc:994] x: 42 y: 4
I0510 14:31:17.516227 1247677 bytecode_interpreter.cc:994] y as s8: 4
I0510 14:31:17.516227 1247677 bytecode_interpreter.cc:994] x: 42 y: 7
I0510 14:31:17.516227 1247677 bytecode_interpreter.cc:994] y as s8: -1
[            OK ]
[...]
```

Note: `trace!` currently exists as a builtin but is in the process of being
removed, as it provided the user with only a "global flag" way of specifying the
desired format for output values -- `trace_fmt!` is more powerful.

### `fail!` / `assert!`: assertion failure

The `fail!` builtin indicates a path that should not be reachable in practice.
Its general signature is:

```
fail!(label: u8[N], fallback_value: T) -> T
```

The `assert!` builtin is similar to fail, but takes a predicate, and does not
produce a value:

```
assert!(predicate: bool, label: u8[N]) -> ()
```

These can be thought of as "fatal assertions", and convert to
Verilog/SytemVerilog assertions in generated code.

Note: XLS hopes to permit users to optionally insert fatal-error-signaling
hardware that correspond to these operations. See
https://github.com/google/xls/issues/1352

`fail!` indicates a **control path that should not be reachable**, `assert!`
gives a **predicate that should always be true** when the statement is reached.

If triggered, these raise a fatal error in simulation (e.g. via a JIT-execution
failure status or a Verilog assertion when running in RTL simulation).

Assuming `fail!` will not be triggered minimizes its cost in synthesized form.
In this situation, **when it is "erased", it acts as the identity function**,
providing the `fallback_value`. This allows XLS to keep well defined semantics
even when fatal assertion hardware is not present.

**Example `assert!`:** if there is a function that should never be invoked with
the value `42` as a precondition:

```dslx
fn main(x: u32) -> u32 {
  assert!(x != u32:42, "x_never_forty_two");
  x - u32:42
}
```

**Example `fail!`:** if only these two enum values shown should be possible
(say, as a documented [precondition](https://en.wikipedia.org/wiki/Precondition)
for `main`):

```dslx
enum EnumType: u2 {
  FIRST = 0,
  SECOND = 1,
}

fn main(x: EnumType) -> u32 {
  match x {
    EnumType::FIRST => u32:0,
    EnumType::SECOND => u32:1,
    // This should not be reachable.
    // But, if we synthesize hardware, under this condition the function is
    // well-defined to give back zero.
    _ => fail!("unknown_EnumType", u32:0),
  }
}
```

The `fail!("unknown_EnumType", u32:0)` above indicates that a) that match arm
*should* not be reached (and if it is in the JIT or RTL simulation it will cause
an error status or assertion failure respectively), but b) provides a fallback
value to use (of the appropriate type) in case it were to happen in synthesized
gates which did not insert fatal-error-indicating hardware.

The associated label (e.g. the first argument to `fail!`) must be a valid
Verilog identifier and is used for identifying the failure when lowered to
SystemVerilog. At higher levels in the stack, it's unused.

### `cover!`

NOTE: Currently, `cover!` has no effect in RTL simulators supported in XLS open
source (i.e. iverilog). See
[google/xls#436](https://github.com/google/xls/issues/436).

The `cover!` builtin tracks how often some condition is satisfied. It desugars
into SystemVerilog cover points. Its signature is:

```
cover!(<name>, <condition>);
```

Where `name` is a function-unique literal string identifying the coverpoint and
`condition` is a boolean element. When `condition` is true, a counter with the
given name is incremented that can be inspected upon program termination.
Coverpoints can be used to give an indication of code "coverage", i.e. to see
what paths of a design are exercised in practice. The name of the coverpoint
must begin with either a letter or underscore, and its remainder must consist of
letters, digits, underscores, or dollar signs.

### `gate!`

The `gate!` built-in is used for operand gating, of the form:

```
let gated_value = gate!(<pass_value>, <value>);
```

This will generally use a special Verilog macro to avoid the underlying
synthesis tool doing boolean optimization, and will turn `gated_value` to `0`
when the predicate `pass_value` is `false`. This can be used in attempts to
manually avoid toggles based on the gating predicate.

It is expected that XLS will grow facilities to inserting gating ops
automatically, but manual user insertion is a practical step in this direction.
Additionally, it is expected that if, in the resulting Verilog, gating occurs on
a value that originates from a flip flop, the operand gating may be promoted to
register-based load-enable gating.

## `import std`

### Bits Type Properties

#### `std::?_min_value`

```dslx-snippet
pub fn unsigned_min_value<N: u32>() -> uN[N]
pub fn signed_min_value<N: u32>() -> sN[N]
```

Returns the minimum signed or unsigned value contained in N bits.

#### `std::?_max_value`

```dslx-snippet
pub fn unsigned_max_value<N: u32>() -> uN[N];
pub fn signed_max_value<N: u32>() -> sN[N];
```

Returns the maximum signed or unsigned value contained in N bits.

#### `std::sizeof`

```dslx-snippet
pub fn sizeof<S: bool, N: u32>(x : xN[S][N]) -> u32
```

Returns the number of bits (`sizeof`) of unsigned or signed bit value. The
signature above is parameterized on the signedness of the input type.

### Bit Manipulation Functions

#### `std::to_signed` & `std::to_unsigned`

```dslx-snippet
pub fn to_signed<N: u32>(x: uN[N]) -> sN[N]
pub fn to_unsigned<N: u32>(x: sN[N]) -> uN[N] {
```

Convenience helper that converts an unsigned bits argument to its same-sized
signed type, or visa-versa. This is morally equivalent to (but slightly more
convenient than) a pattern like:

```dslx-snippet
    x as sN[std::sizeof(x)]
```

#### `std::lsb`

```dslx-snippet
pub fn lsb<N: u32>(x: uN[N]) -> u1
```

Extracts the LSb (Least Significant bit) from the value `x` and returns it.

#### `std::msb`

```dslx-snippet
pub fn msb<N: u32>(x: uN[N]) -> u1
```

Extracts the MSb (Most Significant bit) from the value `x` and returns it.

#### `std::convert_to_bits_msb0`

```dslx-snippet
pub fn convert_to_bits_msb0<N: u32>(x: bool[N]) -> uN[N]
```

Converts an array of `N` bools to a `bits[N]` value.

**Note well:** the boolean value at **index 0** of the array becomes the **most
significant bit** in the resulting bit value. Similarly, the last index of the
array becomes the **least significant bit** in the resulting bit value.

```dslx
import std;

#[test]
fn convert_to_bits_test() {
  let _ = assert_eq(u3:0b001, std::convert_to_bits(bool[3]:[false, false, true]));
  let _ = assert_eq(u3:0b100, std::convert_to_bits(bool[3]:[true, false, false]));
  ()
}
```

There's always a source of confusion in these orderings:

* Mathematically we often indicate the least significant digit as "digit 0"
* *But*, in a number as we write the digits from left-to-right on a piece of
  paper, if you made an array from the written characters, the digit at "array
  index 0" would be the most significant bit.

So, it's somewhat ambiguous whether "index 0" in the array would become the
least significant bit or the most significant bit. This routine uses the "as it
looks on paper" conversion; e.g. `[true, false, false]` becomes `0b100`.

#### `std::convert_to_bools_lsb0`

```dslx-snippet
pub fn fn convert_to_bools_lsb0<N:u32>(x: uN[N]) -> bool[N]
```

Convert a "word" of bits to a corresponding array of booleans.

**Note well:** The least significant bit of the word becomes index 0 in the
array.

#### `std::mask_bits`

```dslx-snippet
pub fn mask_bits<X: u32>() -> bits[X]
```

Returns a value with X bits set (of type bits[X]).

#### `std::concat3`

```dslx-snippet
pub fn concat3<X: u32, Y: u32, Z: u32, R: u32 = X + Y + Z>(x: bits[X], y: bits[Y], z: bits[Z]) -> bits[R]
```

Concatenates 3 values of arbitrary bitwidths to a single value.

#### `std::rrot`

```dslx-snippet
pub fn rrot<N: u32>(x: bits[N], y: bits[N]) -> bits[N]
```

Rotate `x` right by `y` bits.

#### `std::popcount`

```dslx-snippet
pub fn popcount<N: u32>(x: bits[N]) -> bits[N]
```

Counts the number of bits in `x` that are '1'.

#### `std::extract_bits`

```dslx-snippet
pub fn extract_bits<from_inclusive: u32, to_exclusive: u32, fixed_shift: u32,
                    N: u32>(x : bits[N]) -> bits[std::max(0, to_exclusive - from_inclusive)] {
    let x_extended = x as uN[max(unsigned_sizeof(x) + fixed_shift, to_exclusive)];
    (x_extended << fixed_shift)[from_inclusive:to_exclusive]
}
```

Extracts a bit-slice from x shifted left by fixed_shift.  This function behaves as-if x as
resonably infinite precision so that the shift does not drop any bits and that the bit slice
will be in-range.

If `to_exclusive <= from_excsuive`, the result will be a zero-bit `bits[0]`.

#### `std::vslice`

```dslx-snippet
pub fn vslice<MSB: u32, LSB: u32>(x: bits[IN]) -> bits[OUT]
```

Similar to `extract_bits` above, but corresponds directly to the "part-select"
Verilog syntax. That is:

```verilog
y <= x[3:0];
```

Corresponds to:

```dslx-snippet
let y: u4 = vslice<u32:3, u32:0>(x);
```

This is useful when porting code literally as a way to avoid transcription
errors. For new code the
[DSLX first-class slicing syntax](https://google.github.io/xls/dslx_reference/#bit-slice-expressions)
(either range-slicing or width-slicing) is preferred.

### Mathematical Functions

#### `std::bounded_minus_1`

```dslx-snippet
pub fn bounded_minus_1<N: u32>(x: uN[N]) -> uN[N]
```

Returns the value of `x - 1` with saturation at `0`.

#### `std::abs`

```dslx-snippet
pub fn abs<BITS: u32>(x: sN[BITS]) -> sN[BITS]
```

Returns the absolute value of `x` as a signed number.

#### `std::is_pow2`

```dslx-snippet
pub fn is_pow2<N: u32>(x: uN[N]) -> bool
```

Returns true when x is a non-zero power-of-two.

#### `std::?add`

```dslx-snippet
pub fn uadd<N: u32, M: u32, R: u32 = umax(N,M) + 1>(x: uN[N], y: uN[M]) -> uN[R]
pub fn sadd<N: u32, M: u32, R: u32 = umax(N,M) + 1>(x: sN[N], y: sN[M]) -> sN[R]
```

Returns sum of `x` (`N` bits) and `y` (`M` bits) as a `umax(N,M)+1` bit value.

#### `std::?mul`

```dslx-snippet
pub fn umul<N: u32, M: u32, R: u32 = N + M>(x: uN[N], y: uN[M]) -> uN[R]
pub fn smul<N: u32, M: u32, R: u32 = N + M>(x: sN[N], y: sN[M]) -> sN[R]
```

Returns product of `x` (`N` bits) and `y` (`M` bits) as an `N+M` bit value.

#### `std::iterative_div`

```dslx-snippet
pub fn iterative_div<N: u32, DN: u32 = N * u32:2>(x: uN[N], y: uN[N]) -> uN[N]
```

Calculate `x / y` one bit at a time. This is an alternative to using the
division operator '/' which may not synthesize nicely.

#### `std::div_pow2`

```dslx-snippet
pub fn div_pow2<N: u32>(x: bits[N], y: bits[N]) -> bits[N]
```

Returns `x / y` where `y` must be a non-zero power-of-two.

#### `std::mod_pow2`

```dslx-snippet
pub fn mod_pow2<N: u32>(x: bits[N], y: bits[N]) -> bits[N]
```

Returns `x % y` where `y` must be a non-zero power-of-two.

#### `std::ceil_div`

```dslx-snippet
pub fn ceil_div<N: u32>(x: uN[N], y: uN[N]) -> uN[N]
```

Returns the ceiling of (x divided by y).

#### `std::round_up_to_nearest`

```
pub fn round_up_to_nearest(x: u32, y: u32) -> u32
```

Returns `x` rounded up to the nearest multiple of `y`.

#### std::round_up_to_nearest_pow2_?

Returns `x` rounded up to the nearest multiple of `y`, where `y` is a known
positive power of 2. This functionality is the same as `std::round_up_to_nearest`
but optimized when `y` is a power of 2.

#### `std::?pow`

```dslx-snippet
pub fn upow<N: u32>(x: uN[N], n: uN[N]) -> uN[N]
pub fn spow<N: u32>(x: sN[N], n: uN[N]) -> sN[N]
```

Performs integer exponentiation as in Hacker's Delight, Section 11-3. Only
non-negative exponents are allowed, hence the uN parameter for spow.

#### `std::clog2`

```dslx-snippet
pub fn clog2<N: u32>(x: bits[N]) -> bits[N]
```

Returns `ceiling(log2(x))`, with one exception: When `x = 0`, this function
differs from the true mathematical function: `clog2(0) = 0` where as
`ceil(log2(0)) = -infinity`

This function is frequently used to calculate the number of bits required to
represent `x` possibilities. With this interpretation, it is sensible to define
`clog2(0) = 0`.

Example: `clog2(7) = 3`.

#### `std:flog2`

```dslx-snippet
pub fn flog2<N: u32>(x: bits[N]) -> bits[N]
```

Returns `floor(log2(x))`, with one exception:

When x=0, this function differs from the true mathematical function: `flog2(0) =
0` where as `floor(log2(0)) = -infinity`

This function is frequently used to calculate the number of bits required to
represent an unsigned integer `n` to define `flog2(0) = 0`, so that `flog(n)+1`
represents the number of bits needed to represent the `n`.

Example: `flog2(7) = 2`, `flog2(8) = 3`.

#### `std::?max`

```dslx-snippet
pub fn smax<N: u32>(x: sN[N], y: sN[N]) -> sN[N]
pub fn umax<N: u32>(x: uN[N], y: uN[N]) -> uN[N]
```

Returns the maximum of two integers.

#### `std::?min`

```dslx-snippet
pub fn smin<N: u32>(x: sN[N], y: sN[N]) -> sN[N]
pub fn umin<N: u32>(x: uN[N], y: uN[N]) -> uN[N]
```

Returns the minimum of two unsigned integers.

#### `std::uadd_with_overflow`

```dslx-snippet
pub fn uadd_with_overflow<V: u32>(x: uN[N], y: uN[M]) -> (bool, uN[V])
```

Returns a 2-tuple indicating overflow (boolean) and a sum `(x + y) as uN[V]`.
An overflow occurs if the result does not fit within a `uN[V]`.

#### `std::umul_with_overflow`

```dslx-snippet
pub fn umul_with_overflow<V: u32>(x: uN[N], y: uN[M]) -> (bool, uN[V])
```

Returns a 2-tuple indicating overflow (boolean) and a product `(x * y) as uN[V]`.
An overflow occurs if the result does not fit within a `uN[V]`.

### Misc Functions

#### `Signed comparison - std::{sge, sgt, sle, slt}`

```dslx-snippet
pub fn sge<N: u32>(x: uN[N], y: uN[N]) -> bool
pub fn sgt<N: u32>(x: uN[N], y: uN[N]) -> bool
pub fn sle<N: u32>(x: uN[N], y: uN[N]) -> bool
pub fn slt<N: u32>(x: uN[N], y: uN[N]) -> bool
```

**Explicit signed comparison** helpers for working with unsigned values, can be
a bit more convenient and a bit more explicit intent than doing casting of left
hand side and right hand side.

#### `std::find_index`

```dslx-snippet
pub fn find_index<BITS: u32, ELEMS: u32>( array: uN[BITS][ELEMS], x: uN[BITS]) -> (bool, u32)
```

Returns (`found`, `index`) given an array and the element to find within the
array.

Note that when `found` is false, the `index` is `0` -- `0` is provided instead
of a value like `-1` to prevent out-of-bounds accesses from occurring if the
index is used in a match expression (which will eagerly evaluate all of its
arms), to prevent it from creating an error at simulation time if the value is
ultimately discarded from the unselected match arm.

## `import acm_random`

Port of
[ACM random](https://github.com/google/or-tools/blob/66b8d230798f9b8a3c98c26a997daf04974400b8/ortools/base/random.cc#L35)
number generator to DSLX.

DO NOT use `acm_random.x` for any application where security -- unpredictability
of subsequent output and previous output -- is needed. ACMRandom is in *NO*
*WAY* a cryptographically secure pseudorandom number generator, and using it
where recipients of its output may wish to guess earlier/later output values
would be very bad.

### `acm_random::rng_deterministic_seed`

```dslx-snippet
pub fn rng_deterministic_seed() -> u32
```

Returns a fixed seed for use in the random number generator.

### `acm_random::rng_new`

```dslx-snippet
pub fn rng_new(seed: u32) -> State
```

Create the state for a new random number generator using the given seed.

### `acm_random::rng_next`

```dslx-snippet
pub fn rng_next(s: State) -> (State, u32)
```

Returns a pseudo-random number in the range `[1, 2^31-2]`.

Note that this is one number short on both ends of the full range of
non-negative 32-bit integers, which range from `0` to `2^31-1`.

### `acm_random::rng_next64`

```dslx-snippet
pub fn rng_next(s: State) -> (State, u64)
```

Returns a pseudo random number in the range `[1, (2^31-2)^2]`.

Note that this does not cover all non-negative values of int64, which range from
`0` to `2^63-1`. **The top two bits are ALWAYS ZERO**.
