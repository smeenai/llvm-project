# `call unwindabort`: status of this branch

This file describes the state of the `unwindabort` work on this branch, for
whoever picks it up next (human or agent). It's written as current facts;
`git log` has the history. It's not meant to be upstreamed; drop this commit
before sending anything for review.

## Goal

Make C++ `noexcept` (and other terminate scopes) cheap in Clang/LLVM, as it is
in GCC. Clang currently implements a terminate scope as an `invoke` to a
landing pad that catches everything and calls `__clang_call_terminate`, which
costs code size and forces `invoke` for every potentially-throwing call. The
Itanium LSDA can express "terminate" directly: a call site with no entry in the
call-site table makes a C++ personality call `std::terminate`. This series adds
LLVM IR to express that.

- RFC: https://discourse.llvm.org/t/rfc-add-call-unwindabort-to-llvm-ir/62543
- Original patches (James Y Knight, 2023): D141913 (call), D141914 (resume),
  D141915 (catchswitch), D141916 (IR transforms), D141917 (DWARF codegen),
  D141918 (Clang).

## Semantics

- `call unwindabort`: if the call unwinds, the personality aborts instead of
  unwinding to the caller. It is unspecified whether cleanups between the throw
  and the `unwindabort` frame run first (C++ [except.terminate] allows either).
- `resume unwindabort`: always aborts. Lowered to
  `call unwindabort @_Unwind_Resume`, so the personality terminates the same
  way, with no terminate call synthesized in the backend. (This relies on the
  unwinder re-running the personality for the frame that called
  `_Unwind_Resume`, which the Itanium ABI requires.)
- `catchswitch ... unwindabort`: aborts if no handler matches. (Funclet EH;
  codegen not implemented.)
- Only meaningful with a personality that treats a missing call-site entry as
  terminate: `__gxx_personality_v0` (incl. ARM EHABI) and NeXT
  `__objc_personality_v0`. `__gcc_personality_v0` treats it as "keep
  unwinding", so `unwindabort` doesn't work there. The verifier only requires
  *a* personality.

## Invariants (any change must keep these)

1. **The marker is semantic, not an attribute.** Dropping it is a miscompile
   (an exception that must terminate propagates and can be caught upstack)
   unless the callee provably can't unwind.
2. **Any rebuilt call must carry it.** It lives in instruction subclass data,
   which operand/attribute/bundle/metadata copying doesn't cover. Prefer the
   builder default (`IRBuilderBase::setDefaultUnwindAbort` with
   `UnwindAbortGuard`) when a transform builds new calls to replace an
   `unwindabort` call, and `CallInst::copyCallFlagsFrom` for 1:1 rebuilds that
   already copy the tail-call kind.
3. **Any equality or hash over instructions must compare it**
   (`Instruction::hasSameSpecialState`, GVN/NewGVN call keys,
   FunctionComparator, StructuralHash, and any new such key).
4. **`nounwind` on a function containing `call unwindabort` is legitimate**
   (FunctionAttrs/Attributor deduce it: the exception can't leave the function).
   So a call inside such a function must not be marked `nounwind` just because
   the function is (InstCombine, inliner).
5. **`unwindabort` is incompatible with `musttail`** (verifier rejects it; the
   frame that would hold the call-site gap is gone).
6. **Don't add the marker to calls that can't unwind.** It's inert there but
   forces an LSDA onto the function, and linkers won't ICF-fold functions with
   exception tables.

## Design decisions taken

- **Marker on `call`, not a `landingpad` clause.** Keeps the call a
  non-terminator, so a `noexcept` function has the same CFG as under
  `-fno-exceptions`. John McCall preferred defining `call unwindabort` as
  shorthand for an `invoke` to a landingpad with an `unwindabort`/terminate
  clause, so the IR could represent "catch X, else terminate" after inlining.
  That's unresolved in the RFC thread. This branch implements James's
  alternative from the thread instead (next point); no LSDA encoding exists for
  a terminate action combined with other actions.
- **Inlining an invoke with catch/filter clauses through `call unwindabort`**:
  the inlined landingpad gets a `cleanup` clause and its `resume`s become
  `resume unwindabort`. A non-matching exception then enters the landingpad in
  phase 2 and terminates. The cost is losing the "terminate before unwinding"
  backtrace benefit in that case only.
- **Inlining cleanup-only landingpads through `call unwindabort`**: the invoke
  becomes `call unwindabort` and the cleanup is deleted (allowed by the
  semantics; also what libc++abi does for the un-inlined code). Skipped if the
  cleanup can reach a `ret` (it might swallow the exception).
- **Clang emits the marker only** for `__gxx_personality_v0` and NeXT
  `__objc_personality_v0`, and never on wasm targets (Emscripten EH uses the
  Itanium personality but not its call-site tables).
- **ClangIR decides in CFG flattening, not in CIRGen.** CIRGen doesn't pick
  unwind destinations: a terminate scope is a `cir.try` whose catch-all handler
  is `cir.eh.terminate`, and FlattenCFG later turns calls into `cir.try_call`s.
  So FlattenCFG marks a `cir.call` `unwindabort` wherever its unwind
  destination would be such a handler, and the EH ABI lowering does the same
  for the catch-parameter copy. The CIR attribute only means "terminate if an
  exception escapes"; the Itanium EH lowering keeps it for the same
  personalities as classic codegen and otherwise turns the call back into an
  invoke of a terminate landing pad. Marking late also means the passes that
  rebuild calls before flattening (e.g. calling-convention lowering) never see
  the attribute.
- **ClangIR skips EH cleanups on the way to a terminate scope**, as classic
  codegen always has. It used to run them, which would also have kept calls in
  any `noexcept` function with an RAII local from becoming `unwindabort`.

## Commit map

1-6 are James's original patches, ported; each message lists what changed from
the original. 7-15 are follow-ups.

1. IR syntax for `call` (+ fix: printer emitted `unwindabort` after fast-math
   flags, which the parser rejects).
2. IR syntax for `resume`.
3. IR syntax for `catchswitch`.
4. IR transforms/analyses: inliner, SimplifyCFG (`RemoveUnwindEdgeMode`),
   InstCombine, FunctionAttrs/Attributor, EscapeEnumerator.
5. DWARF codegen: EH labels in SelectionDAG/FastISel/GlobalISel, resume
   lowering, no tail calls (+ fix: use-after-free in DwarfEHPrepare).
6. Clang emission, and all Clang test updates (+ fix: Emscripten).
7. Inliner: every unwind path through an inlined callee terminates (cleanup
   clause; invoke-only callees; inlining into an invoke).
8. `unwindabort` is part of instruction identity (equality/hash keys).
9. `unwindabort` is preserved across call rebuilds and lowering (builder
   default, `copyCallFlagsFrom`, statepoints).
10. Inliner doesn't mark calls that can't unwind (the size fix).
11. Inliner drops inlined cleanup-only landingpads.
12. Debugging aid: `-verify-unwindabort-preserved` / `-unwindabort-stress`,
    plus `llvm/utils/unwindabort/` sweep scripts. Probably not upstream
    material as-is.
13. MLIR: `unwind_abort` on `llvm.call`, with translation to and from LLVM IR.
14. ClangIR: skip EH cleanups on the way to a terminate scope, as classic
    codegen does. Independent of `unwindabort`, but needed for it to apply to
    `noexcept` functions with RAII locals.
15. ClangIR emission of `unwindabort`.

## Testing

Every commit builds and passes `check-llvm` and `check-clang` on its own
(validated with a Release+assertions build of clang;lld, targets
AArch64;ARM;X86, `CLANG_ENABLE_CIR=ON`). Commits 13-15 don't touch `llvm/`;
14 and 15 were validated with `check-clang` and `check-mlir` (with `mlir` in
`LLVM_ENABLE_PROJECTS`), and 13's MLIR files are unchanged by them.

Tests specific to this work:

- `llvm/test/Verifier/unwindabort-personality.ll`, `Bitcode/compatibility.ll`
- `llvm/test/CodeGen/{X86,AArch64}/call-unwindabort-codegen.ll`,
  `.../unwindabort-inline-asm-codegen.ll`, `CodeGen/X86/eh-callsites.ll`,
  `CodeGen/X86/unwindabort-inline-cleanup.ll`,
  `CodeGen/AArch64/GlobalISel/call-translator-tail-call.ll`
- `llvm/test/Transforms/*/unwindabort.ll` (ArgumentPromotion, Attributor,
  DeadArgElim, DeadStoreElimination, ExpandVariadics, GVN, InstCombine,
  MergeFunc, NewGVN, RewriteStatepointsForGC, SimplifyCFG),
  `Transforms/Inline/inline-unwindabort.ll`,
  `Transforms/Coroutines/coro-*-unwindabort.ll`,
  `Transforms/LoopVectorize/AArch64/unwindabort-call-variant.ll`,
  `Transforms/PreISelIntrinsicLowering/X86/objc-arc-unwindabort.ll`,
  `Analysis/StructuralHash/unwindabort.ll`,
  `Instrumentation/AllocToken/unwindabort.ll`
- Cases added to existing tests: FunctionAttrs/{nounwind,willreturn}.ll,
  Attributor/{nounwind,willreturn}.ll, ThreadSanitizer/eh.ll,
  InstCombine/nothrow.ll
- Clang: `clang/test/CXX/except/except.spec/p9-{noexcept,dynamic}.cpp`, plus
  many updated codegen tests.
- ClangIR: `clang/test/CIR/Transforms/{flatten-eh-spec,flatten-throwing-in-cleanup,eh-abi-lowering-construct-catch,eh-abi-lowering-unwindabort}.cir`,
  `CIR/Lowering/call-unwindabort.cir`, `CIR/IR/call.cir`,
  `CIR/CodeGen/ehspec-noexcept.cpp`, plus the CIR codegen tests whose LLVM
  checks now match classic codegen again.
- MLIR: `mlir/test/Dialect/LLVMIR/{roundtrip,invalid,inlining}.mlir`,
  `mlir/test/Target/LLVMIR/llvmir.mlir`,
  `mlir/test/Target/LLVMIR/Import/instructions.ll`.

### Finding dropped markers

```
opt -unwindabort-stress -verify-unwindabort-preserved -passes=... in.ll
clang ... -mllvm -unwindabort-stress -mllvm -verify-unwindabort-preserved
python3 llvm/utils/unwindabort/stress_sweep.py --opt build/bin/opt \
    --out sweep.json llvm/test/Transforms llvm/test/Instrumentation
python3 llvm/utils/unwindabort/triage_sweep.py sweep.json
```

Reports look like `unwindabort-check: pass 'P' made 'F' able to unwind to its
caller via: <inst>`. On `llvm/test`, nearly all remaining reports are noise:
passes creating new runtime calls whose declarations lack `nounwind`
(sanitizers, profiling, OpenMP GPU state machines, atomic libcalls, ObjC ARC /
statepoint lowering of `nounwind` intrinsics), outliners calling new functions
that can't actually unwind, `LowerInvoke` (drops EH by design), and the known
Windows funclet `catchswitch` imprecision in `removeUnwindEdge`. Known real
drops are listed under gaps below. The check can't see a marked call being
merged with an unmarked one (invariant 3); those need dedicated tests.

## Measured effect (verified, on an LLVM 20-based branch)

On large Android apps (arm64, `-Oz`, `-funwind-tables
-fno-asynchronous-unwind-tables`, linker ICF), with commits through 10 (before
inlined-cleanup dropping):

- `.gcc_except_table` about -12%, `.eh_frame` about -0.3%, `.text` about -0.1%;
  total EH metadata about -1.4%.
- One app's `.text` grew about 0.5%: `noexcept` functions that lost their
  cleanup code became small enough to inline more often. Treated as an
  acceptable size-for-speed trade, not a regression.
- Without commit 10, `.text` grew about 0.7% and `.eh_frame` about 3.6%:
  inert markers on inlined `operator delete` calls gave thousands of small
  identical functions an LSDA, which defeated ICF.

Not yet measured: the effect of commit 11.

## Known gaps and open questions

- **ClangIR: `throw` directly in a terminate scope** (verified) still unwinds
  to a terminate landing pad, since `cir.throw` has no marker; classic codegen
  emits `call unwindabort @__cxa_throw`. Such a throw always terminates and
  Clang warns about it, so it's rare.
- **MLIR's LLVM-dialect inliner** refuses to inline through an `unwind_abort`
  call instead of marking the inlined calls. It already refuses callees with a
  personality, so supporting it would only take marking each inlined call that
  can throw. Clang's ClangIR pipeline doesn't use that inliner.
- **Non-DWARF EH backends** (verified): funclet/WinEH, SjLj and WebAssembly
  codegen don't implement `unwindabort`; Clang won't emit it for them. For SjLj
  the RFC notes call-site index 0 already means terminate. Inlining through a
  funclet `catchswitch` doesn't preserve the marker (known, left alone).
- **Coroutines** (verified behavior; semantics question open): Clang emits
  `call unwindabort @llvm.coro.await.suspend.handle` for the final suspend of a
  `noexcept` coroutine with a symmetric-transfer final awaiter. CoroSplit's
  resume call for the transfer doesn't inherit the marker, consistent with
  upstream treating the transfer as outside the await_suspend call's EH scope;
  in resume clones it becomes `musttail`, which can't carry the marker.
- **Multi-range LSDAs** (unverified for unwindabort): with
  `-fbasic-block-sections` / `-split-machine-functions`, upstream emits every
  range's `cs_len` up to a single shared action table, so earlier ranges'
  call-site tables appear oversized. The 2023 codegen patch called this out as
  something `unwindabort` would trip over, since it relies on gaps. Upstream
  `main` still has the shared table as of late 2026, judging by
  `EHStreamer.cpp`. Whether unwindabort terminates correctly at runtime with
  these flags hasn't been tested.
- **32-bit ARM (EHABI)**: the LSDA was checked with `llc` and has the expected
  gap (verified); not run on a device.
- **Known marker drops not fixed**: the inliner's `@llvm.experimental.deoptimize`
  rebuild when caller and callee return types differ, guard lowering
  (`MakeGuardsExplicit`, `LowerGuardIntrinsic`), and `HipStdParMathFixup`
  (managed-runtime or GPU only); the IR outliner moves `call unwindabort` into
  an outlined function without a personality (verifier error; the pass is
  slated for removal upstream).
- **`landingpad unwindabort` clause**: see design decisions; needs an LSDA
  encoding for "terminate" combined with other actions (options discussed in
  the RFC thread: null landing pad, a fake type_info, an invalid type_info
  pointer, or a new personality).
- **Inline cost** (open): removing cleanups makes `noexcept` functions cheaper
  to inline, which changes inlining decisions (see measured effect).
- **Sharing empty LSDAs** across functions (RFC "further ideas") isn't done.
