## Task overview

The main idea is to condense the internal representation of keys and per-key aggregation states in the DqHashAggregate operator.
The implementation is in `ydb/library/yql/dq/comp_nodes/dq_hash_combine.cpp`; key and state packaging is being done in
`TBaseAggregationState` (with maybe some details leaking to its descendants).

### Old layout

Simply an array of TUnboxedValuePod keys immediately followed by an array of TUnboxedValuePod state items. The aggregation
hashmap stores pointers to these merged arrays. Allocation is being done from a paged arena, so resizing is nearly impossible without
wasting memory or without the arena turning into a generic allocator.

BUT: there's also a Dehydrated representation for states which consist of a subset of numeric data slot types. It's exclusively used for 
aggregation states.

### Layout restrictions

State is fixed-size per concrete typed operator instance (and has to remain fixed- or bounded-size during updates because we lack resizing).
Key can be variable-sized since it doesn't change after inserting into the hashmap.

### New layout proposal

Let's split the entire family of possible types into "native" and "unboxed values". Native-representable types are these for which
we can build a more compact representation than a TUnboxedValue wrapper. Unboxed values are, well, those that fall back to TUnboxedValue for storage.
Let's start with 32- and 64-bit integral and FP types (UInt32/Int32/UInt64/Int64/Float/Double) including optional variants as a start,
and extend type support onwards in later steps until we provide native implementations for every shorter-than-16-byte value.

Memory layout should be precomputed from the static type info to be used as efficiently as possible at run time. It should include enough information to 
quickly convert an entire inbound tuple of TUnboxedValues into the new layout, as well as converting it back.

The physical layout for an tuple of typed values:
- an array of 128-bit TUnboxedValue for unboxed values
- an array of 64-bit-sized native values
- a number of 32-bit words which contain a validity bitmap for optional/nullable values of native types
- an array of 32-bit-sized native values

It's not clear to me where we should place the validity bitmap for best performance. I don't want to sacrifice more RAM than necessary, and I don't want to break
alignment as well. Your input is welcome.

The layout should be applied to both keys and values. Looks like it would be beneficial to keep key column values together, so we are going to end up with two
adjacent layouts for the key and for the state, separated possibly by alignment padding. Add a compile-time choice between rounding these boundaries to 8 bytes
(up to 7 bytes of padding) and to 16 bytes (up to 15 bytes of padding); both variants will need comprehensive performance comparison later.

Changing the layout of the key also implies building a layout-specific Equals function.

The "dehydrated" mode should be thrown out completely; let's just use this new layout unconditionally, converting back and forth to call aggregation lambdas
when necessary. This is OK because we're going to throw out lambda-based aggregations next.

## Testing

Tests are going to take a lot of lines of code, so let's create a new file in `ydb/library/yql/dq/comp_nodes/ut`.

## Plan

Please explore the current implementation, all the references when necessary, and write an implementation and test plan for switching to the new layout here before writing actual code. Please also highlight
any caveats/underspecified/nonsensical parts to the user.

## Current implementation details

### Stored records and the hash table

`TBaseAggregationState` owns a `TSegmentedArena` and a robin-hood hash set. The
hash set stores one pointer per key; the pointed-to arena record contains the key
followed by the aggregation state. The hash set keeps the upper 32 bits of the
already-computed 64-bit key hash. When spilling is enabled, bits 16..22 of the
same hash select one of 128 arena/spilling buckets.

The current record size is:

```
sizeof(TUnboxedValuePod) * key_width + sum(IAggregation::GetStateSize())
```

There is currently one `TGenericAggregation`. Its state is either an array of
`TUnboxedValuePod` or the special dehydrated array of 64-bit words. Dehydration
is enabled only when every state item is a non-optional `Uint64` or `Double`.
Keys are never dehydrated.

`TSegmentedArena` allocates fixed-stride records from 64-KB-ish pages. Page
starts are sufficiently aligned, but the current stride is not rounded, so only
the first record necessarily has the page's alignment. The new record stride
must be rounded according to a compile-time 8-or-16-byte alignment selection;
the arena page size (`64_KB - 64`) is a multiple of both.

### Key lookup and ownership

The key lambda produces a temporary array of unboxed values. The existing
`TWideUnboxedHasherFib<true>` hashes that array before insertion. The map's
equality functor then compares the temporary array to an arena array using the
static key slots and optional flags.

There are two key extraction paths with different ownership rules:

* Computed keys add a reference to every temporary key. A new map entry takes
  ownership of those references through a byte copy; a duplicate key releases
  them.
* Passthrough keys initially borrow values directly from the input row. A new
  entry adds references after copying, while a duplicate adds none. This path
  feeds the software-prefetch batch as well.

The packed-key transition must make these rules explicit. In particular, a
temporary packed key used only for lookup should be borrowed. Copying it into a
persistent arena record should add references only for its unboxed slots. This
retains the passthrough optimization and lets the existing computed-key
temporary be released uniformly after lookup.

### Aggregation state lifecycle

The generic aggregation moves stored state values into the update or finish
lambda's external nodes. Update results are moved back into the record. Finish
consumes the state. On teardown, any records that have not been consumed are
swept and their state and key references are released.

This lifecycle also applies when a query stops early, including while a
spilling coroutine is suspended. A recent regression test specifically covers
double release during a mid-spill teardown. The new layout therefore needs
separate borrowed, copy-and-ref, move, and destroy operations; plain `memcpy`
must not be the ownership API.

### Spilling

Spill storage is typed as the original logical `TMultiType`, not as the internal
record bytes. Before writing a state record, the current code hydrates the
dehydrated state into a complete unboxed key+state tuple. When reading it back,
it extracts such a tuple and dehydrates the state again. Input rows accumulated
after spilling starts are stored separately as ordinary unboxed tuples.

The spill format should remain unchanged. Packed key and state layouts must be
unpacked into the logical unboxed tuple before `WriteWideItem`, and values read
by `ExtractWideItem` must be packed into a cleared arena record. Clearing the
unboxed slots before an asynchronous read is important because arena pages can
be reused and teardown can run while the read is pending.

### Draining and memory estimation

Both wide and block draining currently read keys directly from the record into
the finish-key external nodes, invoke `ExtractState`, and then release the arena
key references. With the new representation, both key and state should instead
be moved out through the layout abstraction before invoking the finish lambda.

Combine uses static type bounds where possible and otherwise samples actual key
and state memory. The estimate includes the inline record plus allocations
owned by unboxed values (strings and boxed values). The new fixed record stride
must replace the old `16 * key_width + state_size` base, while recursive dynamic
size estimation is needed only for fallback unboxed slots. Otherwise the
smaller representation would not change row-limit selection, or its padding
would be omitted from accounting.

### Existing packed tuple implementation

`hash_join_utils/tuple.*` already has an `NPackedTuple::TTupleLayout`, but it is
not a good fit here. It converts batches between columnar and row layouts,
stores variable-width data in a separate overflow buffer, includes a stored
hash, and implements join key null semantics (null keys do not match). The
aggregation layout is per-record, retains owning `TUnboxedValuePod` fallbacks,
and requires nulls to group together. Reusing the hash-join representation would
therefore add adapters while still requiring different ownership and equality
machinery.

## Proposed layout design

Introduce a small aggregation-specific tuple-layout class in
`dq_hash_combine_layout.h/.cpp`. Keeping it outside the large operator source
makes the byte layout and ownership operations directly unit-testable. It is an
internal C++ interface and does not change the serialized MKQL callable.

The constructor consumes the logical `TType*` list once and builds descriptors
grouped by physical storage class. Each descriptor records its logical tuple
index, byte offset, data slot, and (when applicable) validity-bit index.

Initially, the native classes are:

* 64 bit: `Uint64`, `Int64`, and `Double`;
* 32 bit: `Uint32`, `Int32`, and `Float`;
* the same six types under exactly one `Optional` layer.

All other types, including nested optionals, remain full unboxed values. This is
important because one validity bit cannot represent the three states of
`Optional<Optional<T>>`.

For one logical tuple the byte order is:

```
16-byte unboxed slots
8-byte native slots
4-byte validity words
4-byte native slots
```

There is no internal padding in this order: the unboxed section leaves an
8-byte-aligned offset, the 64-bit section leaves a 4-byte-aligned offset, and
both remaining sections are 4-byte units. Only optional native fields get a
validity bit, packed densely into `ceil(optional_native_count / 32)` `ui32`
words. Payload bytes for an absent native value are written as zero to avoid
retaining uninitialized/stale arena data.

The key layout begins at record offset zero. Layout composition must have a
compile-time alignment parameter constrained to either 8 or 16 bytes. The state
layout begins at `AlignUp(key_layout_size, alignment)`, and the arena stride is
`AlignUp(state_offset + state_layout_size, alignment)`, with a minimum non-zero
stride for the fully zero-width case. Both alternatives preserve the natural
8-byte alignment needed by unboxed and 64-bit native slots; the 16-byte variant
additionally enables aligned 128-bit transfers at the cost of more padding.
This must not be a per-record or runtime setting: the selected layout type and
its offsets should remain compile-time-specializable in hot paths.

The layout API should provide these operations:

* pack borrowed logical values into lookup scratch storage;
* copy a packed tuple into persistent storage and reference only fallback
  unboxed slots;
* pack owning `TUnboxedValue` values into a record by move;
* unpack by copy/ref or by move, reconstructing native optionals as proper
  `TUnboxedValuePod` optional values;
* clear/initialize fallback slots before operations that can suspend;
* destroy a live packed tuple by releasing and clearing fallback slots;
* estimate static and actual memory, including fallback-owned allocations;
* compare two packed keys according to typed aggregation-key semantics.

The packed equality implementation must not use bytewise comparison for
floating-point fields: `+0` and `-0` compare equal, and the existing YQL key
semantics consider all NaNs equal. Optional native fields first compare their
validity bits; two nulls are equal. Fallback unboxed fields continue to use the
existing typed YQL comparison.

Keep `TWideUnboxedHasherFib<true>` and compute the hash from the logical
temporary key before packing it. This preserves all current type-specific hash
semantics, including NaN normalization, and preserves spilling-bucket selection.
The robin-hood table stores the supplied hash and does not need to hash packed
records during growth. Thus only equality has to become layout-specific in this
change.

## Implementation plan

1. Add `dq_hash_combine_layout.h/.cpp` and register the source in
   `ya.make.inc`. Implement layout construction, native conversions, packed
   equality, explicit ownership operations, and memory estimation. Parameterize
   record composition by a compile-time alignment value, with a `static_assert`
   restricting it to 8 or 16. Provide a single production alias selected by a
   compile-time build definition so otherwise-identical binaries can be built
   for later benchmarking.
2. Add separate key and state layout members to `TBaseAggregationState` and
   derive the state offset and record stride from the selected compile-time
   alignment. Replace the map equality functor with the packed-key equality
   functor. Keep logical key values for lambda evaluation and hashing, but use
   a suitably aligned byte buffer as the map probe.
3. Change new-key insertion to copy-and-ref from borrowed packed scratch into
   the arena. Release computed logical keys after every lookup; passthrough keys
   remain borrowed. Update prefetch processing to pack its saved logical key
   immediately before the actual lookup.
4. Make `TGenericAggregation` use the state layout unconditionally. Init packs
   lambda results by move, update moves the old state out and packs update
   results back, finish moves the state into finish external nodes, and teardown
   destroys only still-live records.
5. Remove `IsDehydrated`, its slot conversion helpers, hydrate/dehydrate
   branches, `DisableStateDehydration`, and the corresponding test parameter.
   Rename or remove the existing explicitly-non-dehydrated test as appropriate.
6. Convert state spilling to move both layouts into a logical unboxed tuple
   before writing. On readback, clear a newly allocated record, hash the logical
   key, pack key and state by move, and then insert the packed key pointer.
   Preserve the mid-spill and pending-read teardown invariants.
7. Convert wide and block draining to move keys and states out through the
   layouts. Replace all direct pointer arithmetic/casts that assume an unboxed
   key prefix, including teardown and sampled memory estimation.
8. Base static row limits and sampled memory usage on the aligned record stride
   plus external allocations reachable from fallback unboxed slots.
9. Compile and run direct layout correctness tests with both the 8-byte and
   16-byte template instantiations, regardless of which one the production
   alias selects.
10. Build first, run the focused new tests, run the existing DQ hash-combine
   suite, and finally run the KQP hash-combine replacement suite.

No feature flag should be needed: the callable/TRuntimeNode interface and spill
serialization stay unchanged, and packed bytes never leave the executing
process.

## Test plan

Add `ydb/library/yql/dq/comp_nodes/ut/dq_hash_combine_layout_ut.cpp` to the
existing unit-test target.

Direct layout tests:

* verify exact offsets, section ordering, bitmap word count, key/state
  separation, and record stride for both 8-byte and 16-byte alignment modes,
  covering empty, narrow, mixed, and more than 32 optional-native fields;
* round-trip all six native types, their present/null optional forms, and mixed
  fallback values in a tuple whose physical order differs from logical order;
* include signed extrema, integer zero, infinities, `+0`, `-0`, and multiple NaN
  bit patterns;
* verify packed equality for equal/different native values, two nulls versus
  null/present, NaNs, signed zero, and fallback strings;
* exercise borrowed pack plus persistent copy/ref, move pack/unpack, destroy,
  and reuse of dirty storage with reference-counted strings/boxed values;
* verify static/actual memory estimates do not double-count the 16-byte fallback
  slot and do include external payloads and the padding selected by each
  alignment mode.

Operator tests in the same new file:

* aggregate duplicate and distinct mixed native keys, including optional nulls,
  with mixed 32/64-bit native states and at least one fallback state;
* exercise both passthrough and computed keys so lookup scratch ownership is
  covered;
* exercise DqHashCombine batch draining and DqHashAggregate final draining in
  stream/flow and wide/block variants (including the existing LLVM matrix where
  applicable);
* force aggregation spilling and verify the hydrated spill round trip;
* retain the existing early-stop and suspended-spill teardown tests as
  refcount/double-release regression coverage;
* retain zero-state-width coverage and add a zero-key/fully-zero-width layout
  test even if the program builder does not currently emit that operator shape.

The 8-versus-16 choice also needs a later performance campaign rather than a
microbenchmark-only decision. Build otherwise-identical binaries in both modes
and compare aggregation throughput, CPU counters where available, peak memory,
keys retained before a memory-limit drain/spill, and end-to-end spill volume.
The workload matrix should include narrow records where padding dominates,
wide all-native records where 128-bit transfers could help, mixed fallback
records, high and low key cardinality, and both DqHashCombine and
DqHashAggregate. The current `combiner_perf` block mode does not exercise early
aggregation and has limited scalar coverage, so the harness will likely need
the previously described WideFromBlocks/WideToBlocks wrapping and an early
aggregation option before these results are comprehensive.

Suggested verification commands, all from the working-copy root:

```bash
./ya make --build relwithdebinfo ydb/library/yql/dq/comp_nodes/ut
./ya make --build relwithdebinfo -tA ydb/library/yql/dq/comp_nodes/ut -F 'TDqHashCombineLayoutTest::*' 2>&1 | tail -100
./ya make --build relwithdebinfo -tA ydb/library/yql/dq/comp_nodes/ut -F 'TDqHashCombineTest::*' 2>&1 | tail -120
./ya make --build relwithdebinfo -ttt ydb/core/kqp/ut/opt -F 'KqpHashCombineReplacement::*' 2>&1 | tail -140
```

## Caveats and decisions to confirm

* The recommended bitmap position is the one in the proposal: after 64-bit
  native values and before 32-bit values. It introduces no internal padding;
  moving it cannot reduce either alignment mode's boundary padding.
* Native support applies to a direct data type or one optional layer. Nested
  optionals must fall back unless the format is extended to store optional
  depth, not just validity.
* Eight-byte mode adds at most 7 bytes at each key/state or record boundary;
  16-byte mode adds at most 15. For example, a single fallback key plus a
  currently-dehydrated 64-bit state remains a 24-byte arena stride in 8-byte
  mode but grows to 32 bytes in 16-byte mode. Wider native tuples are where the
  intended savings appear.
* No currently inspected `TUnboxedValuePod` operation requires 16-byte
  alignment: its ordinary representations are accessed as 64-bit pieces, with
  embedded strings handled specially. The 16-byte option is retained to test
  whether aligned SIMD-sized transfers compensate for its extra memory use.
* Hashing is deliberately left on the logical unboxed key; only equality reads
  the packed layout. A packed-key hash can be added later, but doing it now adds
  semantic risk without reducing the per-row key-lambda/unboxed conversion.
* Packing/unpacking native state around every lambda update adds conversion
  work. This is expected to be temporary until lambda-based aggregations are
  removed, but benchmarks should distinguish memory savings from that CPU cost.
* The new direct tests require the layout to live in an internal header/source
  pair rather than entirely in the anonymous namespace of
  `dq_hash_combine.cpp`.


## Plan review verdict

I've reviewed the proposed implementation and the caveats/decisions section. My remarks:

 - we can use existing C++ value comparison operations from `yql/essentials/public/udf/udf_type_ops.h` - such as EquateFloats.

 - a good test type for refcounted UnboxedValues is a string longer than 14 bytes. It doesn't fit into a TUnboxedValue and is thus managed as a heap (or, more precisely, as a MKQL allocator)
allocation. The relwithdebinfo build should track reference counting errors such as double-free attempts and memory leaks.

 - operator tests in different files would have to share certain machinery (like input-generating streams, graph building, graph running, result comparison...). It can be extracted into a separate support library (or a header),
but more extensive type dispatch needs to be added anyway to support generating/validating types other than ui64/string.
Or - we can just improve existing tests to cover more complex key/value types instead of adding a new operator test file. 
I suggest doing just that: an existing test run function (like `RunDqAggregateWideTest`) can do multiple runs of different key/value shape and complexity. 
This way we won't forget to add new kinds of tests to another file when adding them into `dq_hash_combine_ut.cpp`. These tests won't be very unit anymore but oh well. At least they won't make the CI 
test browser explode with a huge number of test instances. Unit tests for layout packing do need a different `_ut.cpp` file though

 - the Sum aggregation doesn't of course work on strings. We can use a Max aggregation for strings instead.

 - don't do performance/memory usage tests yet, just implement the unit test suite to assert correctness. We'll swith over to working on performance/RAM usage measurement suite when the inital version
of the new layout is functionally complete.
