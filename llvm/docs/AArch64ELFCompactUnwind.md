# AArch64 ELF Compact Unwind Tables

This document specifies compact unwind tables for AArch64 ELF, which replace `.eh_frame` and the
`.eh_frame_hdr` search table. It is for implementers of linkers, unwinders and other tools that read
or write unwind information.

The format extends Alexis Engelke's compact unwind proposal, designed with x86-64 in mind, with an
AArch64 frame descriptor that GCC and LLVM output can both use, and with support for asynchronous
unwinding. The aim is one format that both toolchains' linkers produce and that LLVM libunwind,
libgcc, debuggers and profilers read. Field positions and the version number are provisional until
both toolchains agree on them.

LLD produces the tables with `-z compact-unwind`, LLVM libunwind reads them, and
`llvm-readobj --unwind` dumps them. Unwinders that don't know the format, including libgcc's, find
no unwind information in binaries linked this way. The tables have been checked against the DWARF
CFI at every instruction and with the exception-handling test suites of both toolchains.

## Contents

1. [Overview](#overview)
2. [Locating the table](#locating-the-table)
3. [The table](#the-table)
4. [Descriptors](#descriptors)
5. [Where saved registers live](#where-saved-registers-live)
6. [Prologues and epilogues](#prologues-and-epilogues)
7. [Function-keyed side tables](#function-keyed-side-tables)
8. [Escape programs](#escape-programs)
9. [Reading and producing the table](#reading-and-producing-the-table)
10. [Optional encoder optimizations](#optional-encoder-optimizations)
11. [Coverage](#coverage)
12. [Results](#results)
13. [Recommendations for compilers](#recommendations-for-compilers)
14. [Alternatives considered](#alternatives-considered)
15. [Possible extensions](#possible-extensions)
16. [Open questions](#open-questions)

Appendix: [Worked examples](#appendix-worked-examples)

## Overview

### What unwinding needs

To step from a frame to its caller, an unwinder needs, for the current pc:

- the **CFA** (canonical frame address: the caller's sp);
- where the **return address** is, and whether it is signed (pointer authentication);
- where each **callee-saved register** was saved, if it was;
- for exceptions, the **personality routine** and the **LSDA** (language-specific data area).

DWARF CFI answers these with a small bytecode program per function (an FDE). That is fully general,
but large: in the AArch64 builds measured here, `.eh_frame` and its search table are 10–24% of the
size of `.text`, and every lookup interprets a program.

### What a compact table does instead

Almost all functions follow a few shapes that the compiler's frame-layout code produces. A **64-bit
descriptor** names the shape: which registers are saved, how big the frame is, whether there is a
frame pointer, how the return address is signed. The unwinder computes every slot from that.

- The table maps **regions** of code to descriptors.
- **Sync** tables describe the state at call sites only, which is all that exception handling and
  most stack walks need. A function usually needs one descriptor, and adjacent functions with the
  same descriptor **fold** into one region.
- **Async** tables are correct at every instruction, which profilers and crash handlers need. A few
  extra descriptor fields say that a region starts with a prologue or ends with an epilogue, and the
  unwinder derives their lengths from the frame shape
  ([Prologues and epilogues](#prologues-and-epilogues)). A region still usually covers one
  function, but no longer folds with its neighbours.
- Anything a descriptor can't express becomes an **escape**: a deduplicated program of DWARF CFA
  opcodes ([Escape programs](#escape-programs)). Escapes are rare (0.01% of functions in the GCC-built
  CentOS packages measured here), but they keep the format general.
- Per-function data that doesn't fit in a descriptor (LSDAs, mainly) goes into **side tables**
  ([Side tables](#function-keyed-side-tables)).

The table carries everything an unwinder needs, so the linker doesn't output `.eh_frame`.

### Design rules

- **The linker produces the table.** Compilers keep emitting ordinary CFI. The linker interprets the
  CFI, picks descriptors, and **verifies** that each descriptor reproduces the CFI exactly; it splits
  regions or escapes otherwise. A compiler that changes its frame layout therefore causes more
  escapes, never wrong unwind information.
- **Regions are self-contained.** A region is decoded on its own, with no state carried over from
  earlier regions and no backward search for the function start.
- **No relocations.** Every value is an offset fixed at link time; the table is read-only and needs
  no dynamic relocations.

## Locating the table

The table takes the place of the `.eh_frame_hdr` contents and is found the same way: through the
`PT_GNU_EH_FRAME` program header, which `dl_iterate_phdr` and `_dl_find_object` already report. A
version byte of 3 at its start identifies a compact unwind table; nothing else is shared with
version 1, the binary search table over `.eh_frame`. The table is 8-byte aligned.

Tools that find `.eh_frame` through `PT_GNU_EH_FRAME`, such as debuggers, profilers, crash
reporters and post-link optimizers like BOLT, need to learn the new version.

## The table

The table consists of a header, a two-level page index of descriptor entries, a global descriptor
table, a personality table, side tables and escape programs. The page structure follows Alexis
Engelke's proposal.

### Addressing

- All offsets are relative to the start of the table, as in the `.eh_frame_hdr` search table
  (`DW_EH_PE_datarel`), so the unwinder only needs the table's runtime address. There is no image
  base.
- Addresses in the image (pcs, LSDAs, personality slots) are signed 32-bit offsets from the table, so
  they reach ±2 GiB around it.
- Positions inside the table (pages, tables, escape programs) are unsigned 32-bit offsets.
- Pcs inside a page are deltas from the page's start, in instructions (4 bytes).

### Header

44 bytes:

| Field | Size | Meaning |
|---|---|---|
| version | u8 | format version |
| flags | u8 | bit 0: sync-only table (no async fields) |
| reserved | u16 | 0 |
| page count, first-level offset | u32, u32 | the descriptor page index |
| global descriptor count, offset | u32, u32 | at most 3755 descriptors |
| personality count, offset | u32, u32 | |
| side table count, directory offset | u32, u32 | [Side tables](#function-keyed-side-tables) |
| escape area offset, size | u32, u32 | [Escape programs](#escape-programs) |

### Descriptor pages

- **First level.** One (s32 page pc, u32 page offset) pair per page, sorted by pc, plus a sentinel
  whose pc is the end of the last region.
- **Pages** (at most 4 KiB): a u16 entry count, a u16 offset of the page's local descriptors, then
  4-byte entries, then (8-byte aligned) the local descriptors.
- **Entries.** Each entry is a 20-bit pc delta from the page pc (in instructions, so a page can span
  4 MiB) and a 12-bit descriptor index. Indices below the global count select a global descriptor;
  higher ones select the page's local descriptors. A full page can have 341 local descriptors, which
  is why there are at most 4096 − 341 = 3755 global ones.
- **Regions.** Each entry starts a region that runs to the next entry, or to the next page's pc for a
  page's last entry.

### Personality table

An array of s32 offsets, each pointing at a pointer-sized slot that holds a personality routine's
address. The indirection matches DWARF's `DW_EH_PE_indirect`, since the routine usually lives in
another module. When the CFI references a routine directly, the linker gives it a slot, such as a
GOT entry.

A descriptor's personality field of 1–6 selects entry 0–5; 7 means the index is in a side table.
The most personalities seen in one binary is 3 (`__gxx_personality_v0`, `__gcc_personality_v0` for
C with `-fexceptions`, and `rust_eh_personality`).

## Descriptors

A descriptor is one 64-bit value, stored in the target's byte order; bit 0 is the least significant
bit. No field straddles the two 32-bit halves. The low half says which registers are saved, how the
return address is handled and which personality routine applies; the high half holds offsets, sizes
and the async fields. Each field name in the layout links to its explanation.

### Layout

| Bits | Field | Values |
|---|---|---|
| 0–2 | [mode](#mode) | 0 NULL · 1 FP · 2 SP · 3 escape · 4 FP + PARTIAL · 5 SP + PARTIAL · 6 no unwind info · 7 reserved |
| 3–4 | [convention](#convention) | 0 LLVM · 1 LLVM with shadow call stack · 2 GCC, sp-anchored · 3 GCC, CFA-anchored |
| 5–7 | [RA scheme](#ra-scheme) | 0 unsigned · 1/2 A/B key · 3/4 A/B key with PC (hint instructions) · 5/6 A/B key with PC (non-hint instructions) |
| 8–19 | [x19–x30](#register-masks) | saved GPRs (bit 8 = x19, 18 = x29, 19 = x30) |
| 20 | [frame record](#frame-record) | x29 and x30 form a frame record |
| 21–28 | [d8–d15](#register-masks) | saved FP/SIMD registers (bit 21 = d8) |
| 29–31 | [personality](#personality) | 0 none · 1–6 personality table entry · 7 see side table |
| 32–35 | [O](#o-save-area-offset) | save-area offset ÷ 16 |
| 36–47 | [N](#n-size) | frame size ÷ 16, or another size ([details](#n-size)) |
| 48 | [has-prologue](#async-fields) | region starts with a prologue |
| 49 | [has-epilogue](#async-fields) | region ends with an epilogue |
| 50–53 | [PS override](#async-fields) | prologue size, if not the canonical one |
| 54–55 | [apos override](#async-fields) | lead-in length + 1, if not the canonical one |
| 56–59 | [tail length override](#async-fields) | the tail length, if not the canonical one |
| 60 | [tail signed to end](#async-fields) | the return address stays signed through the whole tail (`retaa`) |
| 61–63 | spare | 0 |

In PARTIAL descriptors, bits 48–59 hold the index of a [layout descriptor](#partial-descriptors)
instead of async fields, and the convention field only marks the shadow call stack; the others keep
their meaning.

**Escape descriptors** (mode 3) have their own layout:

| Bits | Field |
|---|---|
| 0–2 | mode = 3 |
| 5–7 | RA scheme (selects the key; the program tracks whether the address is signed) |
| 8, 9, 10 | DWARF CIE augmentations: `S` signal frame, `G` MTE-tagged frame, `N` non-hint pointer authentication |
| 29–31 | personality |
| 32–63 | offset of the program in the escape area |

**No unwind info** is the descriptor value 6 (mode 6, every other bit 0): code without CFI, such as
hand-written assembly. It differs from a NULL frame, which asserts that the return address is in
x30.

### Field reference

#### mode

How to compute the CFA.

- **NULL**: no frame. CFA = sp, the return address is in x30 and nothing is saved. This describes
  leaf functions and the instructions before a frame is set up or after it is torn down.
- **FP**: the frame is addressed through the frame pointer: CFA = x29 + the frame record's distance
  below the CFA, which follows from the save-area layout ([Where saved registers live](#where-saved-registers-live)).
  Functions with `alloca` or variable-length arrays need this, since their sp moves.
- **SP**: CFA = sp + 16·N. Functions without a frame pointer, and GCC functions whose CFA is
  sp-relative in the body.
- **PARTIAL** (FP or SP): some registers that have slots aren't saved at this point
  ([below](#partial-descriptors)).
- **Escape**: interpret a DWARF program ([Escape programs](#escape-programs)).

#### convention

Which compiler's rule assigns stack slots to the saved registers ([Where saved registers live](#where-saved-registers-live)).
The high bit selects the compiler, the low bit a variant. LLVM and GCC lay out the save area
differently; GCC has two placements, one of them used when the function has a stack-protector canary.

Value 1 is the LLVM convention for a function that pushes x30 onto Clang's shadow call stack
(`-fsanitize=shadow-call-stack`). In the body, x18 is the caller's x18 + 8, so the unwinder restores
it by subtracting 8. Only the LLVM convention needs this: GCC's shadow call stack emits no CFI for x18
and is [out of scope](#out-of-scope).

#### RA scheme

Whether the return address is signed in the body, and with what. With pointer authentication
(`-mbranch-protection=pac-ret`), the function signs x30 on entry (`paciasp`) with the A or B key and
sp as the modifier, and authenticates it before returning. The unwinder must authenticate the saved
return address the same way.

**PAuth_LR** (`pac-ret+pc`) adds the address of the signing instruction as a second modifier, so the
unwinder must also find that instruction ([the PAuth_LR signing instruction](#the-pauth_lr-signing-instruction)). Compilers emit
either the `PACM` hint before the usual hint instructions (values 3/4) or the non-hint `*PC`
instructions (values 5/6). The two need different authentication sequences.

#### Register masks

Which of the callee-saved registers x19–x30 and d8–d15 are saved, in that order. Their slots follow
from the convention, the frame record bit, O and the masks, with no per-register offsets.

x30 always holds the return address on entry; its mask bit only says whether, and so where, it was
saved.

#### frame record

x29 and x30 are saved as a frame record: the pair the frame pointer points to, which chains the
frames together. Both mask bits must be set; a frame record without both is reserved. FP mode
requires it.

A frame record isn't the same as "both registers saved". With a frame record, LLVM pairs x30 only
with x29, which changes where the padding hole goes ([LLVM convention](#llvm-convention)), and GCC puts the pair
first. Without one, x29 and x30 are ordinary registers in the save area:

| x29, x30, frame record | Used by |
|---|---|
| neither saved | |
| x30 saved | `-fomit-frame-pointer` |
| both saved, not a frame record | `-fomit-frame-pointer` (x29 as an ordinary register) |
| x29 saved | leaf functions that use x29 as a scratch register |
| frame record | frame-pointer functions |

#### O (save-area offset)

Moves the save area away from where the convention would otherwise put it, in 16-byte units. GCC
needs it: in its sp-anchored layout, the outgoing-argument area sits below the save area, and in its
CFA-anchored layout the varargs save area sits above it. Clang's frames normally have O = 0, but the
linker can use O for intermediate prologue states
([A.17](#a17-gcc-stack-protector-frame-cfa-anchored-convention)). 0 in PARTIAL descriptors.

#### N (size)

12 bits, so up to 64 KiB in 16-byte units. Its meaning depends on the mode:

- **SP mode**: the frame size; CFA = sp + 16·N. Larger frames escape.
- **FP mode with GCC's sp-anchored convention**: also the frame size, since that convention places
  the save area relative to sp.
- **FP mode with prologue or epilogue windows**: the frame size during the windows, where the CFA is
  sp-relative ([Prologues and epilogues](#prologues-and-epilogues)).
- **With a PC-based RA scheme, where N is otherwise unused**: the offset of the PAuth_LR signing
  instruction ([the PAuth_LR signing instruction](#the-pauth_lr-signing-instruction)).
- Otherwise 0.

#### personality

0 means no personality routine and no LSDA. 1–6 select personality table entry 0–5; the function
then has a row in the LSDA side table ([Side tables](#function-keyed-side-tables)). 7 means the
personality table index is in the personality side table.

#### Async fields

These say whether a region has a prologue and an epilogue, and override their lengths where the
canonical ones, which the unwinder derives from the frame shape, don't fit.
[Prologues and epilogues](#prologues-and-epilogues) defines the lengths and the canonical values.

- **has-prologue**, **has-epilogue**: the region starts with a prologue, or ends with an epilogue.
- **apos override**: 0 for the canonical lead-in length; otherwise the length + 1, so it can say 0,
  1 or 2.
- **PS override**: 0 for the canonical prologue size; otherwise the size, 1–15.
- **tail length override**: 0 for the canonical tail length; otherwise the length, 1–15.
- **tail signed to end**: the return address stays signed through the whole tail, because the
  function returns with a combined authenticate-and-return instruction (`retaa`).

### PARTIAL descriptors

Some functions have points where only some of their saved registers are actually in their slots.
GCC's CFI records each prologue save as it happens, so its prologues pass through such states, and
GCC's separate shrink-wrapping saves some registers only on the paths that need them.

A PARTIAL descriptor describes such a point with the help of a **layout descriptor**, which it names
by index in bits 48–59:

| From the layout descriptor | From the PARTIAL descriptor |
|---|---|
| convention, masks, frame record bit, O: these define every slot | mode, N, RA scheme, personality; its convention field is 1 for a shadow call stack, otherwise 0; its masks give the registers saved at this point, and its frame record bit is 0 |

A register is restored from its slot if and only if the PARTIAL descriptor's masks include it. The
layout index is resolved like an entry's descriptor index, in the page of the region being unwound,
so the layout descriptor can be global or one of the page's local descriptors. A global PARTIAL
descriptor can be used from any page, so its layout descriptor must be global. The layout descriptor
needn't be used by any region. PARTIAL descriptors never have prologue or epilogue windows, which
is what frees bits 48–59 for the index.

### Folding

Adjacent regions with identical descriptors fold into one, except when the descriptor:

- has prologue or epilogue flags,
- is an escape,
- or holds a PAuth_LR signing offset in N.

All of these describe positions relative to the region start, which folding would move.

## Where saved registers live

The slot rules match the compilers' frame-layout code: LLVM's `computeCalleeSaveRegisterPairs`
(`AArch64FrameLowering.cpp`) and GCC's `aarch64_layout_frame` (`aarch64.cc`, GCC 16).

### LLVM convention

This is conventions 0 and 1 (1 with a shadow call stack). Slots are consecutive 8-byte slots
going down from CFA − 16·O, in this order:

1. x19…x28, then x30 and x29 if they are saved but not a frame record
2. x30, x29 if they are a frame record
3. d8…d15

If the total number of saved registers is odd, there is one 8-byte hole directly above the unpaired
register. The unpaired register is the last one in whichever group (the GPRs or the d registers) has
an odd count. Locals may live in the hole.

This follows LLVM's algorithm: the callee-saved list is x19…x28, x30, x29, d8…d15, filled top-down
and paired greedily within a register class. With a frame record, x30 pairs only with x29; otherwise
it pairs with whatever precedes it (`stp x30, x19`). So with x19, x29 and x30 saved, a frame record
leaves x19 unpaired (hole above x19), while without one x19 pairs with x30 and x29 is unpaired (hole
above x29). [Examples A.1–A.5](#a1-clang-frame-pointer-pac-ret-the-fully-canonical-case) show each
case.

### GCC conventions

GCC's save area has the same internal order in both conventions: the GPRs go up consecutively, with a
frame record first, otherwise x30 first and x29 last; then the d registers start at the next 16-byte
boundary. If that leaves an 8-byte gap after the GPRs and the number of d registers is odd, the last
d register goes into the gap.

| Saved | GPR order, going up |
|---|---|
| frame record | x29, x30, x19…x28 |
| x30, x29 as ordinary registers | x30, x19…x28, x29 |
| x30 | x30, x19…x28 |
| x29 | x19…x28, x29 |

When GCC uses its shadow call stack without stack-clash protection, it puts x30 after x28 instead in
frames without a frame record; those layouts are [out of scope](#out-of-scope).

The two conventions differ in where the area sits:

- **sp-anchored (convention 2):** the area starts at sp + 16·O, with the locals above it. O is the
  outgoing-argument area.
- **CFA-anchored (convention 3):** the area ends 16·O below the CFA, with the locals below it. O is the
  varargs save area, usually 0. GCC uses this layout when the function has a stack-protector canary
  (the CVE-2023-4039 fix), so that an overflowing local array reaches the canary before the saved
  registers. Most Linux distributions build with `-fstack-protector-strong`, so this is a mainstream
  layout in distribution code ([example A.17](#a17-gcc-stack-protector-frame-cfa-anchored-convention)).

## Prologues and epilogues

Sync tables only need the state at call sites: the **body state** between prologue and epilogue,
which the descriptor's frame fields describe. Async tables must also be right inside prologues and
epilogues. Rather than describing those instruction by instruction, a descriptor says the region has
a prologue or epilogue, and the unwinder derives where each part starts from the frame shape.

### The parts of a region

```
 region start                                                         region end
 |<- lead-in ->|<-- rest of prologue -->|<----- body ----->|<- window ->|<- tail ->|
 |<--------------- PS ----------------->|
 |<-- apos --->|
```

| Part | Length | Unwinder's view |
|---|---|---|
| lead-in: the instructions before the stack allocation | apos (allocation position) | NULL frame. With a signed return address, the last lead-in instruction signs it, so the RA counts as signed from instruction apos onward |
| shadow-stack push | 1 with convention 1, else 0 | NULL frame; x18 = caller's x18 + 8 after the push |
| rest of the prologue, up to the first instruction where the body state holds | PS (prologue size) counts from the region start | CFA = sp + 16·N once the allocating instruction has executed. The saves are in progress, but each register still holds the caller's value, so it is restored from itself |
| body | | the descriptor's state |
| epilogue window: the restores before the frame is popped (LLVM convention, FP mode) | window length | CFA = sp + 16·N, and every saved register is still described in its slot: the restores read from the slots, so restoring again is harmless, even after x29 has been reloaded |
| tail: the return, its authentication, any frameless code after it, and padding before the next function | tail length | NULL frame. The return address is still signed for the first few instructions (the shadow-stack pop, PAuth_LR's address computation, `autiasp`), or to the end with the tail-signed-to-end bit |

SP-mode functions need no epilogue window: their body state is already sp-relative, and their
slots stay valid until the frame is popped.

### Canonical lengths

The unwinder computes every length that has no override from the frame shape. Two values from the
shape are used throughout:

- **groups**: the number of store/load instructions for the save area: register pairs among the GPRs,
  plus one for the frame record, plus d-register pairs (an odd register counts as a pair).
- **pre-index**: the allocation is folded into the first save (`stp …, [sp, #-size]!`) if and only if
  16·N equals the save-area size; otherwise there is a separate `sub sp`.

With a = apos (override or canonical) and s = 1 for convention 1 (shadow call stack):

| Length | Canonical value (LLVM) |
|---|---|
| apos | 1 for pac-ret (`paciasp`), 2 for PAuth_LR (`pacm; paciasp`), otherwise 0 |
| PS | a + s + (pre-index ? 0 : 1) + groups + (FP mode ? 1 : 0) |
| window length | FP mode: groups, plus 1 without pre-index (the instruction that pops the frame is inside the window); GCC conventions: none |
| tail length | 1 + s, plus for a signed return address: 1 (`autiasp`, unless `retaa`) and 3 more for PAuth_LR |
| signed instructions at the start of the tail | s + 1 (pac-ret) or s + 4 (PAuth_LR) |

[Example A.1](#a1-clang-frame-pointer-pac-ret-the-fully-canonical-case) shows a function where every
length is canonical. The overrides cover the common deviations, and anything beyond them splits the
region:

- **apos**: a lead-in of 0–2 instructions that isn't the canonical one: `bti c` in BTI-only builds,
  or a shrink-wrapped early exit before the prologue
  ([A.6](#a6-clang-shrink-wrap-early-exit-before-the-frame-island-after-the-ret),
  [A.7](#a7-bti-only-build-bti-c-as-the-lead-in)).
- **PS**: a prologue with other instructions scheduled into it. GCC needs this for most prologues
  ([A.8](#a8-gcc-pre-index-frame-with-an-interleaved-save-ps-override)).
- **tail length**: frameless code after the return, such as an early-exit block laid out after the
  epilogue, and alignment padding before the next function.

**GCC.** Only the allocating store is canonical (PS = a + 1). GCC sometimes stores the frame record
together with the allocation (`stp x29, x30, [sp, #-size]!`); the unwinder then treats the record as
saved from that instruction onward. This is derived, not encoded: it holds when O = 0 and the frame
is at most 504 bytes (sp-anchored), or when O = 0 and the whole frame is the save area of at most 512
bytes (CFA-anchored).

**Shadow call stack** (convention 1). The push follows the lead-in and the pop is the first tail
instruction. Clang's CFI restores the saved registers only after the pop, so at the pop they are still
described in their slots and x18 still has its rule; the unwinder reproduces exactly that.

### Windows follow the CFI

The linker uses a window only where every CFI state in it equals the window's view; it never
assumes anything about a register or slot that the CFI doesn't say.

- **Clang** records all the saves in one CFI row at the end of the prologue, since it doesn't touch
  those registers before then, and keeps them described in their slots until the frame is popped.
  These are exactly the prologue's and the epilogue window's views.
- **GCC** records each save as it happens, and may reuse a register right after saving it. The PS
  override therefore covers only the instructions before the first save row
  ([A.8](#a8-gcc-pre-index-frame-with-an-interleaved-save-ps-override)), and the states after it
  become regions of their own, with smaller layouts or PARTIAL descriptors
  ([A.13](#a13-gcc-prologue-saves-one-at-a-time-partial)). GCC's standard epilogue records all the
  restores at the final stack adjustment, so the body state holds until then and no window is
  needed. Restores of separately shrink-wrapped registers are recorded as they happen, and those
  states become regions too.

CFI must be correct on every path into an instruction, so the table is correct even where an
epilogue is also a merge point ([A.15](#a15-gcc-epilogue-as-a-merge-point)).
[Recommendations for compilers](#recommendations-for-compilers) suggests how GCC could record saves
and restores so that windows cover more of its prologues and epilogues.

### The PAuth_LR signing instruction

With PAuth_LR, authenticating the return address needs the address of the instruction that signed it.
For a region with a PC-based RA scheme, the unwinder finds it in the first of these that applies:

1. **From the prologue**: with a prologue window and apos ≥ 1, it is the last lead-in instruction
   (region start + apos − 1). The linker only uses such a window when that is the real signing site.
2. **From N**: when nothing else uses N (NULL mode, or FP mode without windows and without GCC's
   sp-anchored convention), N is the signing instruction's offset from the region start, in
   instructions. These descriptors don't fold ([folding](#folding)).
3. **From a side table row** ([Side tables](#function-keyed-side-tables)): when N is in use, or
   when N is 4095, which marks a signing instruction too far from the region start for N, or
   before it.

The farthest signing instruction seen was 155 instructions into a function (Clang) and 380 (GCC), both
shrink-wrapped prologues; almost all are at instruction 0, or 1 for PAuth_LR's `pacm; paciasp`.

## Function-keyed side tables

Per-function data that doesn't fit in descriptors lives in side tables. All side tables share one
format, laid out independently of the descriptor pages: descriptor pages can then span as much code as
folding allows, while side-table pages are as sparse as the functions that need rows.

### Format

- **Directory** (located by the header): one entry per table: kind (u8), pc-offset width (u8: 2 or
  4), payload width (u8: 2 or 4), reserved (u8), u32 offset of the table's first level, u32 page count.
- **First level**: (s32 page pc, u32 page offset) pairs plus a sentinel. A page's pc is the pc of its
  first row.
- **Page** (at most 4 KiB): a u16 row count, u16 flags, the rows' pc offsets from the page pc (in
  instructions), padding to the payload width, then the payloads. Offsets and payloads are separate
  arrays so that every load is aligned.
- **Rows**: a row applies from its pc up to the table's next row. The linker puts a row at the start
  of every function that needs the table (counting a split-off cold fragment as a function), and
  wherever the payload changes within one.
- **Page breaks**: when a page is full, or, with 2-byte pc offsets, when the next row is 256 KiB or
  more past the page pc.

The unwinder consults a side table only when a descriptor field says it applies, so a module that
needs no rows of some kind has no table of that kind.

### Kinds

| Kind | Rows required for | Payload |
|---|---|---|
| 1, LSDA | every function with personality ≠ 0 | s32 offset of the LSDA from the table, or 0 for a personality without an LSDA |
| 2, personality | every function whose personality field is 7 | u16 personality table index |
| 0x80, PAuth_LR signing instruction | every function with a PC-based RA region whose descriptor doesn't locate the signing instruction ([the PAuth_LR signing instruction](#the-pauth_lr-signing-instruction)) | s16 offset from the row's pc, in instructions |
| other 0x80–0xff | architecture-specific | |

### Lookup

1. Binary-search the first level for the last page whose pc ≤ pc.
2. Binary-search that page's pc offsets for the last row ≤ pc.

The page always contains such a row, because its pc is its first row's pc. The result is unambiguous
even when descriptors fold across functions: every function that needs the table has a row at its
start, and no other function's row lies between its start and any pc inside it.

### Compressed LSDA pages

Flag bit 0 marks a compressed LSDA page: an s32 base LSDA follows the flags, and the payloads are u16
offsets from it in 4-byte units (0xffff: no LSDA). The linker compresses a page when its LSDAs are
4-byte aligned and lie within 256 KiB of each other. This depends only on how far apart a page's LSDAs
are, not on their order, so it keeps working with order files and identical code folding.
Compressed pages make the LSDA table 21–33% smaller ([side table results](#side-tables)). Readers
must support both kinds of page.

## Escape programs

An escape descriptor points into the escape area at a program: a ULEB128 length followed by DWARF CFA
opcodes, without CIE or FDE headers.

- **Implied initial state**: code alignment 4, data alignment −8, return address in x30, CFA = sp,
  every register unchanged. The descriptor's RA scheme gives the signing key and its augmentation bits
  the rest of what a CIE would give.
- **Positions** are relative to the region start, so functions with the same shape share a program;
  the linker stores each distinct program once.
- **Personality and LSDA** come from the descriptor and side tables, as for any other region.
- **Anything DWARF can express is allowed**, including expressions. SVE and SME frames, unusual
  calling conventions and odd CFI therefore work without encodings designed for them up front.

The linker rewrites each FDE's instructions for the implied state: it rescales advances and factored
offsets (Clang's CIEs use code alignment 1 and data alignment −4, GCC's 4 and −8), picks the shortest
opcode forms and drops padding, so programs from both compilers deduplicate. The CIE's initial
instructions must be exactly `DW_CFA_def_cfa sp, 0`, which holds for both compilers on AArch64.

An unwinder can run these programs with its existing CFI interpreter, given a synthesized CIE.

## Reading and producing the table

### Unwinder

1. Binary-search the first level for the page, then the page's entries for the region containing the
   pc. Fetch the descriptor from the global or local table.
2. Escape: run the program with the DWARF interpreter ([Escape programs](#escape-programs)).
3. Otherwise, if the descriptor has a prologue or epilogue, compute the lengths
   ([Prologues and epilogues](#prologues-and-epilogues)); if the pc falls outside the body, use
   that part's state.
4. In the body: compute the CFA and restore each saved register from its slot
   ([Where saved registers live](#where-saved-registers-live)). For a PARTIAL descriptor, take the
   slots from its layout descriptor.
5. Authenticate the return address according to the RA scheme and signing state.
6. For exceptions, look up the LSDA (and, if needed, the personality) in the side tables.

LLVM's decoder is in `llvm/lib/BinaryFormat/ELFCompactUnwind.cpp`: LLD uses it to check every
descriptor it emits, and `llvm-readobj --unwind` to print the unwind rows each region implies.
libunwind has its own copy of about 280 lines (`libunwind/src/AArch64CompactUnwind.hpp`); its table
lookup and the rest of its reader add about 290.

### Linker

LLD writes the table with `-z compact-unwind=async` (or just `-z compact-unwind`) and
`-z compact-unwind=sync`. It interprets each function's CFI into a list of states, one per CFI row,
and chooses regions and descriptors that reproduce every state exactly.

- **Sync**: each run of call sites with the same state gets a descriptor; async fields stay zero, so
  descriptors fold across functions. A Clang-built binary typically needs only about 50 distinct
  descriptors. Which states the call sites see depends on the CFI:
  - Clang's sync-only CFI (`-fno-asynchronous-unwind-tables`, or `-Oz`) optionally signs the
    return address and pushes to the shadow call stack, then describes the whole frame in one row
    that holds to the end. Such CFI is only meant to be right from that row on, so the whole function gets that
    row's descriptor. Calls before the row go to prologue helpers, which never unwind
    ([A.11](#a11-homogeneous-prologues--oz-sync-only-cfi)).
  - For any other CFI, the linker finds call sites by decoding the instructions (`bl`, `blr` and
    their pointer-authenticated forms), which is exact with AArch64's fixed-width encoding. GCC
    emits the same CFI on AArch64 with and without `-fasynchronous-unwind-tables`; encoding all of
    its states would make sync tables 3–4× larger.
- **Async**: the linker cuts one region per frame episode (a run of framed states between frameless
  ones). The frameless code before the frame becomes the prologue's lead-in when it fits, and the
  frameless code after it the epilogue's tail; a frame re-entered after an early return gets a
  region with no prologue. The body, window lengths, lead-in and save-area offset follow from the
  CFI. An episode that doesn't fit one region, typically a GCC prologue that records each save, is
  split greedily at its state changes.
- **Verification**: every chosen region is checked against every CFI state it covers, including
  the derived lengths, slot positions and return-address signing state. Functions that can't be
  matched become escapes.
- **Gaps between functions** get a no-unwind-info entry, unless they hold only padding that the
  previous region can cover ([padding after epilogues](#padding-after-epilogues)).

## Optional encoder optimizations

These are linker techniques that don't affect the format: an encoder may skip any of them and only
produce more regions or escapes. LLD implements all of them.

### Union layouts

With separate shrink-wrapping, no single CFI state may have every register saved: one path saves
x22–x25, another d10–d15. GCC still gives every register one slot for the whole function. So the
linker can use the union of all states' (register, slot) pairs as the PARTIAL layout, provided each
register always appears at the same offset, and it still verifies every state.

This rescues 0–4 functions in each 15–17K-function GCC 16 build of the LLVM subset, and 36 of 884K
in the CentOS packages. The rescued functions are large escapes, so sync tables shrink by up to 2%. It
costs about 20 lines of linker code.

### Padding after epilogues

Functions are often followed by alignment padding: GCC aligns functions to 32 bytes at -O2 and -O3.
A region without an epilogue simply extends over the padding. A region with an epilogue can't,
because its epilogue is anchored at the region's end, so the linker grows its tail over the
padding instead, up to the override's 15 instructions. Padding is never executed, so what the tail
says about it doesn't matter.

This removes almost every no-unwind-info entry after GCC functions (3,249 → 28 in a GCC 16 -O3
build of the LLVM subset) and makes GCC -O2/-O3 async tables 1–2.7% smaller. Clang leaves no gaps
after its AArch64 functions, so it is unaffected.

### Lead-ins from the previous region

A frameless instruction at the end of a function (a `ret` after an epilogue, say) can become the lead-in
of a following region rather than a NULL region of its own ([A.5](#a5--fomit-frame-pointer-with-x29-as-an-ordinary-register),
[A.15](#a15-gcc-epilogue-as-a-merge-point)).

### Choosing global descriptors

Putting the most used descriptors in the global table keeps local descriptor tables small. Very
large async binaries have more than the 3755 that fit and spill the rest into pages. A layout
descriptor counts the uses of the PARTIAL descriptors that name it, and a PARTIAL descriptor is only
made global together with its layout descriptor; one that stays local brings its layout descriptor
into each page that uses it.

## Coverage

### One descriptor per function

- **Clang**: frame-pointer frames of any size, including stack-clash probe loops; `-fomit-frame-pointer`
  frames, including x29 as an ordinary register; canonical prologues and epilogues, including pac-ret,
  PAuth_LR, BTI and shadow call stack.
- **GCC**: frames where every save precedes any reuse (with the PS override), frames with an
  outgoing-argument area, stack-protector frames, `alloca` frames (FP mode), frames up to 64 KiB.
- **Frameless code**: leaf functions (NULL), frameless functions with stack (SP mode, nothing saved).
- **Hardening and instrumentation**: MTE stack tagging, HWASan and ASan, XRay and patchable-entry
  sleds, `-pg`, `-finstrument-functions`.
- **Other shapes**: stack realignment, varargs, noreturn tails, basic-block-section fragments.

### Several descriptors per function

| Case | Why | Example |
|---|---|---|
| epilogue in the middle of a function | the epilogue must end a region, and the code after it starts a new one | [A.9](#a9-clang-mid-function-epilogues-switch-with-12-returns) |
| long frameless code before a shrink-wrapped prologue | becomes its own NULL region; short lead-ins fit the apos override | [A.6](#a6-clang-shrink-wrap-early-exit-before-the-frame-island-after-the-ret) |
| frameless code after a return, beyond the tail's reach | own NULL region, or the next region's lead-in | [A.5](#a5--fomit-frame-pointer-with-x29-as-an-ordinary-register) |
| GCC prologue saves recorded one at a time | regions with smaller layouts or PARTIAL descriptors for the intermediate states | [A.13](#a13-gcc-prologue-saves-one-at-a-time-partial) |
| GCC separate shrink-wrapping | different saved sets at different call sites, so **even sync** needs several descriptors (12% of the CentOS functions have PARTIAL regions, 2.5% need them at call sites) | [A.14](#a14-gcc-separate-shrink-wrapping-sync-needs-two-descriptors) |
| several stack allocations (frames over 4 KiB without a frame pointer, stack-clash probes) | one SP region per step | |
| `pac-ret+leaf` | the signing state toggles at every return | [A.10](#a10-pac-retleaf-the-signing-state-toggles-per-return) |
| lengths beyond the override fields | region split | |

### Escapes

| Case | Why |
|---|---|
| SVE callee-saves, vector calling convention | CFA expressions and 128-bit saves ([A.16](#a16-escapes)) |
| SME hazard padding | a 1 KiB gap between the GPR and FP/SIMD saves |
| `preserve_most` and other conventions that save x9–x15 | registers outside the masks |
| frames over 64 KiB without a frame pointer | N's range; Clang's probe loops also move the CFA to x9 |
| GCC stack slots allocated for registers that are never saved | no convention matches |
| GCC save areas more than 240 bytes above sp (large outgoing-argument or varargs areas) | O's range |
| `DW_CFA_register`, unknown opcodes, CFA in other registers | not expressible |
| signal trampolines | CFA expressions |
| GCC shadow call stack | [out of scope](#out-of-scope) |
| PAuth_LR functions mixing `DW_CFA_AARCH64_set_ra_state` with `negate_ra_state_with_pc` | not expressible (51 of 19K functions in a Clang build) |
| CFI that contradicts the code | not expressible |

Escapes are rare in practice: only glibc's `_start` in Clang built by Clang, 46 more of 258K
functions in Clang built by GCC 16, none in the Clang C++ projects, and 93 of 884K functions (0.01%)
in the CentOS packages (41 large frames, 48 saves of other registers, 4 unusual CFAs).

### Out of scope

**GCC's shadow call stack** (`-fsanitize=shadow-call-stack`) is out of scope. GCC documents it as
designed for the Linux kernel, without userspace runtime support and incompatible with exception
handling, and emits no CFI for x18, so an unwinder couldn't restore the shadow stack pointer anyway.
Its functions are encoded where they fit (with frame pointers, the frame record comes first, so most
do) and escape otherwise: 60% of functions in a GCC shadow-call-stack build of the LLVM subset
without frame pointers. Encoding the rest would need two more layouts (x30 after x28, used unless
stack-clash protection is on) and an escape for its epilogue, whose CFI still places the return
address in the popped frame for one instruction.

## Results

### Summary

Without exceptions, sync tables are 4.2–5.5% of the size of the DWARF they replace for Clang-built
code and 6–9.4% for GCC-built code. Async tables are 9–13% for Clang and 17–35% for GCC, whose
separate shrink-wrapping gives functions many frame states
([GCC and async tables](#gcc-and-async-tables)). C++ code with exceptions adds about 3% for the
LSDA side table. Throwing an exception is about 1.8× faster than with libunwind's DWARF lookup, and
linking Chrome takes 5–8% longer ([Speed](#speed)).

### Corpora

Linked by LLD:

- **Chrome for Android**: `libchrome.so` for arm64, as a release build (Clang `-O2`) and as an
  official ThinLTO build. The ThinLTO link used LLVM's default inliner in place of Chromium's ML
  inliner, which changes `.text` by less than 0.1%.
- **Clang**: the `clang` binary (Release, all targets) from LLVM's main branch, compiled by trunk
  Clang and by GCC 16 with the flags in the table.
- **Flang**: `flang` from a Release build of LLVM, compiled by Clang 21.
- **C++ projects built from source**: DuckDB (shell and unit tests), Z3 (`libz3.so`), Poco (8
  libraries) and yaml-cpp (its test binary), compiled by trunk Clang with CMake's Release flags
  (`-O3`) and exceptions.
- **glibc 2.42**, built with GCC 16.
- **LLVM subset**: 310 LLVM source files built with Clang and GCC 16 at several optimization levels,
  and with pac-ret, PAuth_LR, BTI, shadow call stack, SVE and basic-block sections, for the
  experiments that need many builds or variants.
- **A large Android app** (210 shared libraries, 162K functions with LSDAs), for the LSDA table.

Prebuilt binaries, measured with a model of the encoder:

- **Clang C++ projects**: trunk Clang `-O2` builds of DuckDB, Z3, OpenCV, Poco, Botan, cppcheck,
  xerces-c, yaml-cpp and Boost: 463K functions, 192K with an LSDA.
- **CentOS Stream 9**: 55 AArch64 packages, 427 binaries, 884K functions, 252K with an LSDA. GCC 11
  with the distribution's flags (`-O2 -flto -fexceptions -fstack-protector-strong
  -fasynchronous-unwind-tables`). Includes LibreOffice, MySQL, MariaDB, CMake, OpenCV, GDAL, Z3, GDB,
  Ceph, Boost, Poco, Botan, xerces-c, Inkscape, and the distribution's Rust toolchain, cargo and
  ripgrep (built by LLVM).

### Table sizes

Each table's size, and its share of the DWARF it replaces: `.eh_frame` + `.eh_frame_hdr` (FDE bytes
and search table; LSDAs and other data the CFI points to are the same in both).

#### Measured

Each build was linked by LLD with and without `-z compact-unwind`. The table is the output's
`.eh_frame_hdr`, so its size includes the side tables (mostly LSDA rows, in compressed pages); the
last column gives their size. Escapes are counted in the async table; sync tables have as many or
fewer.

| Build | FDEs | DWARF | Escapes | Sync | Async | Side tables |
|---|---|---|---|---|---|---|
| Chrome for Android (Clang -O2) | 588K | 33.9 MiB | 1889† | 1.67 MiB (4.9%) | 3.50 MiB (10.3%) | 1.0 KiB |
| … with ThinLTO | 408K | 24.8 MiB | 1623† | 1.20 MiB (4.8%) | 2.71 MiB (10.9%) | 10 KiB |
| Clang, built by Clang -O3 | 285K | 16.2 MiB | 1‡ | 719 KiB (4.3%) | 1.70 MiB (10.5%) | 0 |
| … pac-ret | 283K | 16.7 MiB | 1‡ | 719 KiB (4.2%) | 1.62 MiB (9.7%) | 0 |
| … -fomit-frame-pointer | 285K | 13.8 MiB | 11‡ | 783 KiB (5.5%) | 1.80 MiB (13.0%) | 0 |
| … -Os | 345K | 17.0 MiB | 1‡ | 921 KiB (5.3%) | 1.48 MiB (8.7%) | 0 |
| Clang, built by GCC 16 -O3 | 258K | 14.2 MiB | 47‡ | 1.06 MiB (7.5%) | 4.48 MiB (31.6%) | 0 |
| … -Os | 326K | 14.5 MiB | 20‡ | 902 KiB (6.1%) | 2.52 MiB (17.4%) | 0 |
| … -fomit-frame-pointer | 258K | 13.6 MiB | 91‡ | 1.11 MiB (8.2%) | 4.73 MiB (34.7%) | 0 |
| Flang, built by Clang 21 -O3 | 418K | 24.1 MiB | 13\* | 1.03 MiB (4.3%) | 2.61 MiB (10.9%) | 0 |
| DuckDB (shell, unit tests) | 100K | 6.12 MiB | 2 | 450 KiB (7.2%) | 865 KiB (13.8%) | 162 KiB |
| Z3 (`libz3.so`) | 49K | 2.90 MiB | 0 | 216 KiB (7.3%) | 395 KiB (13.3%) | 77 KiB |
| Poco (8 libraries) | 11K | 581 KiB | 0 | 46 KiB (7.8%) | 88 KiB (15.1%) | 15 KiB |
| yaml-cpp (tests) | 8.9K | 472 KiB | 1 | 40 KiB (8.5%) | 71 KiB (15.0%) | 15 KiB |
| glibc 2.42 (GCC 16) | 4.4K | 0.23 MiB | 15 | 22 KiB (9.4%) | 79 KiB (34.2%) | 0.4 KiB |
| LLVM subset, Clang -O3, basic-block sections | 412K | 19.4 MiB | 0 | 166 KiB (0.8%) | 295 KiB (1.5%) | 0 |

† Nearly all (1833 and 1566) are functions with Clang's `preserve_most` calling convention, which
Chrome uses for slow paths and which saves x9–x15; another 50 save the SVE vector length (VG).

‡ One in each is glibc's `_start`, whose CFI marks the return address undefined. The others are
frames over 64 KiB, and in the GCC builds also save areas further from sp than O reaches and save
areas with slots for registers GCC never saves.

\* All but `_start` come from an LLVM bug, since fixed, that emits CFI contradicting the code after
tail-duplicated prologues; Clang 21 predates the fix.

Without their side tables, the C++ projects' sync tables are 4.6–5.3% of the DWARF size, in line
with the code built without exceptions. Basic-block sections show folding at its best: 412K tiny
FDEs cost 19 MiB of DWARF, while adjacent fragments with the same descriptor fold into one entry,
for a 166 KiB sync table.

#### Modeled

These are prebuilt binaries, which weren't relinked: the tables come from a model of the encoder
run on each binary's `.eh_frame`, which matches LLD where both were run. The model doesn't grow
tails over padding ([padding after epilogues](#padding-after-epilogues)), which makes GCC -O2/-O3
async tables 1–2.7% smaller on the LLVM subset. Its sizes leave out the LSDA side table, which with
uncompressed pages adds 1.46 MiB (3.0% of the DWARF) for the CentOS packages and 1.11 MiB (3.9%)
for the Clang C++ projects ([Side tables](#side-tables)).

| Build | Functions | DWARF | Escapes | Sync | Async |
|---|---|---|---|---|---|
| Clang C++ projects | 463K | 28.5 MiB | 0 | 1.33 MiB (4.7%) | 3.21 MiB (11.3%) |
| CentOS packages (GCC 11) | 884K | 49.0 MiB | 93 | 3.83 MiB (7.8%) | 16.1 MiB (32.9%) |
| … of which Rust (LLVM) | 123K | 7.4 MiB | 3 | 0.30 MiB (4–5%) | 1.01 MiB (12–17%) |

### GCC and async tables

As a share of DWARF, GCC's sync tables are 1.2–1.9× the size of Clang's, and its async tables
2.4–3.9×. Modeled on the same projects from the CentOS packages and the Clang C++ projects:

| Project (same source) | DWARF, GCC / Clang | GCC sync | Clang sync | GCC async | Clang async |
|---|---|---|---|---|---|
| OpenCV 4.6 (core) | 1.56 / 1.38 MiB | 9.3% | 5.3% | 46% | 12% |
| Botan 2.19 | 436 / 671 KiB | 8.0% | 5.2% | 35% | 12% |
| Boost 1.75 | 421 / 346 KiB | 9.6% | 5.6% | 40% | 14% |
| cppcheck 2.9 | 281 / 316 KiB | 8.4% | 5.2% | 44% | 12% |
| xerces-c 3.2 | 312 / 389 KiB | 7.0% | 5.8% | 26% | 11% |
| yaml-cpp 0.6 | 28 / 36 KiB | 13% | 6.9% | 48% | 16% |

(Measured with an earlier version of the encoder; the GCC columns are within half a point of the
current one.)

The async cost comes from **separate shrink-wrapping**: GCC saves and restores each register only
around the code that needs it, so a function has many different frame states, and each needs its own
region. That is information the table has to carry, not encoding waste. On the LLVM subset at -O3:

| GCC 16 flags | DWARF | Async | Regions per function | Sync |
|---|---|---|---|---|
| default | 1.03 MiB | 421 KiB (40%) | 5.3 | 98 KiB (9.3%) |
| `-fno-schedule-insns2` | 1.04 MiB | 390 KiB (37%) | 5.0 | 98 KiB (9.2%) |
| `-fno-shrink-wrap-separate` | 0.86 MiB | 224 KiB (25%) | 2.7 | 58 KiB (6.6%) |
| both | 0.86 MiB | 187 KiB (21%) | 2.3 | 58 KiB (6.6%) |

Alexis Engelke's x86-64 measurements (async tables 13% of DWARF with Clang and 29% with GCC 15, on
libLLVM.so) predate GCC's separate shrink-wrapping for x86-64, which arrived in GCC 16.

### Side tables

The LSDA table costs 3.0% of the DWARF size for the CentOS packages (1.46 MiB) and 3.9% for the
Clang C++ projects (1.11 MiB). Compressed pages shrink it to 67% on Clang builds of Z3, DuckDB,
Poco, yaml-cpp and libc++, and to 79% on the Android app (959 KiB to 758 KiB), whose order files
scatter LSDAs more: LSDA order follows code order for about 99% of neighbouring functions without
order files, but only for 62% in the Android app.

With PAuth_LR (Clang `pac-ret+pc`, LLVM subset), locating signing instructions from the prologue or
from N first, and only then from side-table rows, takes the sync table from 121 KiB to 75 KiB (9.9%
to 6.1% of the 1.20 MiB of DWARF) and the async table from 205 KiB to 188 KiB (16.7% to 15.4%). In
async, most functions still need a row, for a later PC-signed region such as a mid-function
epilogue.

### Speed

- **Unwinding**: throwing an exception is about 1.8× faster than with libunwind's DWARF lookup.
- **Linking**: with either table, LLD on 192 threads links Chrome for Android (186 MiB of text, 588K
  FDEs) in 6.9 s instead of 6.4 s (7% longer). Without debug info, it takes 5.8 s (async, 8%
  longer) or 5.6 s (sync, 5%) instead of 5.3 s. Most of the extra time is encoding each function's
  CFI and laying out the table, partly offset by not writing `.eh_frame`. In Chrome's ThinLTO link,
  the table's 0.35 s is lost in a link of over four minutes. Links that do little else take
  proportionally longer: a Release Clang without debug info links 44% slower with an async table and
  37% slower with a sync one (on 72 threads).

## Recommendations for compilers

The format works with today's CFI, but a few compiler changes would make tables smaller or let more
functions avoid escapes. None of them is required, and none changes the format. Sizes below are
async tables as a share of DWARF on the LLVM subset built with GCC 16. They are upper bounds,
measured by letting the linker accept the states these changes would produce, with an earlier
version of the encoder, so the baselines differ slightly from [Table sizes](#table-sizes).

**Record prologue saves in one CFI row where possible.** GCC records each save as it happens, so
its prologue passes through a state per save, and each becomes a region of its own
([Windows follow the CFI](#windows-follow-the-cfi)). Delaying the rows to the end of the saves, or
to the first instruction that overwrites a saved register, would let one prologue window cover
them, as it does for Clang:

| GCC 16 build | Today | Batched save rows |
|---|---|---|
| -O3 | 39.8% | 36.2% |
| -O2 | 36.7% | 32.3% |
| -O3 `-fno-shrink-wrap-separate` | 25.3% | 20.4% |
| -Os | 20.9% | 15.1% |
| glibc | 34.3% | 31.0% |

The DWARF itself shrinks too, since it has fewer rows.

**Record shrink-wrapped restores at the final pop where it is safe.** Clang, and GCC's standard
epilogue, keep the saved registers described in their slots until the frame is popped. GCC records
restores of separately shrink-wrapped registers as they happen. Where such an epilogue isn't a branch
target reached with a different set of saved registers, delaying those rows to the pop would save up
to another 2–2.4 points at -O2 and -O3. That bound includes merge-point
epilogues like [A.15](#a15-gcc-epilogue-as-a-merge-point), where delaying would be wrong, so the
achievable part is smaller.

**Keep frames within the slot conventions.** The linker escapes any frame whose layout matches no
convention, such as GCC's stack slots allocated for registers that are never saved. Unusual but
conforming shapes cost descriptor space instead: LLVM saves an unused callee-saved register as a
scavenging scratch register in frames over 255 bytes (16K of 367K functions in the Clang C++
projects), and such leaf functions are the main users of "x29 saved without x30".

**Describe the shadow call stack register.** Code that pushes to a shadow call stack should describe
x18 in its CFI, as Clang does, so that unwinding through it restores the shadow stack pointer.

**Use one PAuth_LR CFI form per function.** Functions that mix `DW_CFA_AARCH64_set_ra_state` with
`DW_CFA_AARCH64_negate_ra_state_with_pc` escape (51 of 19K functions in a Clang `pac-ret+pc`
build).

**Emit CFI that matches the code.** The linker checks descriptors against the CFI, not against the
instructions, so wrong CFI yields wrong tables or escapes. An LLVM bug with tail-duplicated
prologues, since fixed, causes 12 of the 13 escapes in Flang built by Clang 21.

## Alternatives considered

Each feature of the descriptor was kept because the corpora above need it. The measurements below
come from the encoder at the time each decision was made, so their baselines differ slightly.

### What each feature buys

| Feature | Without it |
|---|---|
| GCC conventions | GCC sync tables 7–9× larger (most GCC functions escape) |
| CFA-anchored GCC convention | 3% of CentOS functions escape (26.5K instead of 1K); sync 13.3% instead of 8.3% |
| O field | GCC sync tables 2–2.4× larger |
| PARTIAL descriptors | GCC sync tables 1.6–3.3× larger |
| x30 saved without a frame record | nearly every non-leaf `-fomit-frame-pointer` function escapes |
| x29 saved without x30 | 140 Clang functions escape in the C++ projects |
| convention 1 (LLVM with shadow call stack) | every Clang function that uses the shadow call stack escapes, because of the x18 rule |
| N of 12 bits rather than 10 | 222 instead of 94 CentOS escapes (SP frames between 16 and 64 KiB) |
| prologue/epilogue windows | Clang async tables 1.7–1.9× larger |
| apos override | BTI-only builds need an extra region per function (+27% async) |
| PS and tail length overrides rather than explicit lengths | explicit 17-bit lengths measured within ±1.5% of the derived ones |
| 4-bit rather than 3-bit tail length override | with padding absorption, GCC -O2/-O3 async tables 0.1–0.6 points larger (0.3 typical) |
| u16 pc offsets in side tables | LSDA table 33% larger |
| shell-less escape programs | escape bytes 1.5–2.8× larger (FDEs); re-encoding for the implied state saves another 6–17% |
| PAuth_LR sites from the prologue or N | PAuth_LR sync tables 1.6× larger |

The personality side table (kind 2) has never been needed: no binary had more than 3 personalities.
It costs nothing when unused and keeps more than 6 personalities possible.

### PARTIAL: layout references vs. live masks

PARTIAL descriptors could instead carry a mask of the registers not yet saved, in the 12 async bits plus
spare bits:

| PARTIAL encoding | CentOS escapes | Sync |
|---|---|---|
| per-pair d-register mask (15 bits) | 997 | 8.3% |
| layout-indexed mask (16–17 bits) | 497–548 | 8.0% |
| **layout reference (12 bits)** | **222** | **7.8%** |

The layout reference covers every saved set and leaves the spare bits free. Taking the layout from the
function's first full descriptor, found by walking backward, doesn't work: 83% of functions with
PARTIAL regions have one before any region with the full layout, and the walk would need a
function-start marker.

### LSDA side table layouts

On the Android app (162K LSDA functions, sizes in KiB):

| Layout | Size | Drawback |
|---|---|---|
| flat rows: u32 pc + u32 LSDA | 1267 | |
| **4 KiB pages, u16 pc + s32 LSDA** | **959** | |
| 4 KiB pages, u32 pc + s32 LSDA | 1275 | |
| fixed 256 KiB fences + u16 pc + s32 LSDA | 957 | gaps between sections cost entries; needs a base address |
| first-row column in the descriptor page index | 954 | couples the side table to the descriptor pages |
| 4 KiB pages, u16 pc + u16 LSDA delta from the previous row | 1206 | LSDAs aren't in code order |
| 20-bit pc + 12-bit LSDA packed | ~721 | needs 4-byte aligned LSDAs (LLVM only); 4 MiB reach |
| **4 KiB pages, compressed where possible** | **758** | |

2-byte pc offsets lose only where a 256 KiB window holds fewer than about 6 LSDA rows, where the table
is tiny either way: a library with 127 MiB of code and only 201 functions with LSDAs needs 1.5 KiB
with pages and 3.1 KiB with fences. Across corpora, u16 offsets give 75–76% of the u32 size despite
twice as many pages.

### Other alternatives

- **A separate async table** on top of a sync table: about twice the size of the integrated async
  descriptors for Clang.
- **Apple-style register-pair masks** instead of convention-derived slots: without compiler changes,
  sync tables 3.9–9.4× larger.
- **Treating restores as no-ops until the frame is popped** (fewer epilogue regions for GCC): unsafe,
  because an epilogue can be a merge point reached before some saves happened
  ([A.15](#a15-gcc-epilogue-as-a-merge-point)). A version the linker can check needs a scan of all
  branch targets and saves only about 3.5%. The compiler knows which epilogues are safe
  ([Recommendations for compilers](#recommendations-for-compilers)).
- **Sync "don't care" for functions without calls**: rejected because crash handlers need correct
  frames for leaf functions.
- **Windows-style unwind codes** (one code per prologue instruction): require compiler directives,
  since a linker can't derive them from DWARF.
- **Escapes as pointers to FDEs**: simpler, but requires keeping `.eh_frame`, and shell-less programs
  are 1.5–2.8× smaller.
- **Per-function inline programs** for functions with a few odd states: at most 87 functions in any
  corpus would benefit.
- **The PS override as a delta from the canonical prologue size** rather than the total: as an
  unsigned delta, Clang tables without frame pointers or with a shadow call stack shrink 1.6–2.1%,
  but `-Oz` builds grow about 20%, because their prologues are often shorter than canonical. A signed
  delta (−8 to +7) changes nothing measurable; GCC is unaffected either way, since its canonical
  prologue is only the allocating store.

## Possible extensions

- **Spare bits**: 61–63 and mode 7 are reserved, as is a frame record without both x29 and x30. One
  bit could mean "an extension word follows", for example a compact encoding for SVE save areas,
  should SVE frames become common.
  So far they are rare even in auto-vectorized code (28 of 25K functions in an OpenCV build for
  armv9-a).
- **More saved registers**: the same extension word could hold a mask for x9–x15, which functions
  with Clang's `preserve_most` convention save (nearly all of Chrome's 1.9K escapes), and SME's
  saved vector length (VG) and hazard padding.
- **`pac-ret+leaf`**: allowing prologue and epilogue windows on NULL descriptors with a signed
  return address (lead-in `paciasp`, tail `autiasp; ret`) would cut such leaf functions from one
  region per signing change to one per return path ([A.10](#a10-pac-retleaf-the-signing-state-toggles-per-return)).

## Open questions

- **The version number.** 3 is provisional, pending agreement with the GNU toolchain, which also
  needs a reader in libgcc.
- **Modules larger than ±2 GiB**: a wide-offset variant, or out of scope.
- **x86-64**: the container and side tables are architecture-neutral, but 1-byte code units give only
  64 KiB per side-table page with 2-byte offsets; x86-64 may prefer 4-byte offsets (not measured).

## Appendix: Worked examples

All examples are real encoder output.

**How to read the listings.**
- The column after the instruction is the CFI state *before* that instruction executes.
- `x19@-8` means x19 is saved at CFA−8.
- `region [0x30, 0x84)` is the region's extent as byte offsets from the function start, like the
  offsets in the left column: from 0x30 up to, but not including, 0x84. Region lines show the fields
  the descriptor stores; bracketed values are what the unwinder computes, in instructions.
  "Canonical" means the field is 0 and the unwinder derives the value.
- PARTIAL regions are shown as the full layout plus `live={…}`, the registers that still hold their
  own values. The descriptor itself stores the saved set (the layout minus `live`) and references a
  descriptor holding the full layout.

### A.1 Clang, frame pointer, pac-ret: the fully canonical case

```
region [0, 0x48): FP LLVM record=frame x19,x20 d8 N=3 ra=pac  has-prologue has-epilogue
               [derived: apos=1 PS=5 window=3 tail=2]
   0  paciasp                         ; no frame
   4  str  d8, [sp, #-48]!            ; no frame, RA signed           <- allocation at apos=1
   8  stp  x29, x30, [sp, #16]        ; CFA=sp+48                      (saves in progress)
   c  stp  x20, x19, [sp, #32]
  10  add  x29, sp, #0x10
  14  fmov s8, s0                     ; CFA=x29+32 x19@-8 x20@-16 x30@-24 x29@-32 d8@-48   <- body (PS=5)
  ...
  34  ldp  x20, x19, [sp, #32]        ; CFA=sp+48, slots valid         <- epilogue window (3)
  38  ldp  x29, x30, [sp, #16]
  3c  ldr  d8, [sp], #48              ;                                <- pop (pre-index frame)
  40  autiasp                         ; no frame, RA signed            <- tail (2)
  44  ret                             ; no frame
```

- **Lengths:** all canonical. Three groups (d8, the frame record, x19/x20); pre-index, because
  16·N equals the save-area size.
- **Hole:** 5 registers is odd, so there is a hole above d8 (at −40). Nothing is encoded for it.
- **Sync descriptor:** the same with N = 0 and no flags, so it folds with any identically shaped
  neighbour.

### A.2 LLVM hole at the top: the odd register is x19, not a d register

```
region [0, 0x58): FP LLVM record=frame x19 d8,d9 N=3  has-prologue has-epilogue [apos=0 PS=4 window=3 tail=1]
   0  stp  d9, d8, [sp, #-48]!        ; no frame
   4  stp  x29, x30, [sp, #16]        ; CFA=sp+48
   8  str  x19, [sp, #32]
   c  add  x29, sp, #0x10
  10  str  w0, [x29, #28]             ; CFA=x29+32 x19@-16 x30@-24 x29@-32 d8@-40 d9@-48
```

- **The hole:** the GPRs have one register, so the hole goes above x19, at the top (−8). Clang puts a
  4-byte local there (`str w0, [x29, #28]`).
- **Contrast:** A.1 has two GPRs and one d register, so its hole sits above d8. The same rule covers
  both.

### A.3 `sub sp` first (locals below the save area)

```
region [0, 0x4c): FP LLVM record=frame x19 d8 N=3  has-prologue has-epilogue [apos=0 PS=5 window=4 tail=1]
   0  sub  sp, sp, #0x30              ; no frame                       <- allocation, not a store
   4  str  d8, [sp, #16]              ; CFA=sp+48
   8  stp  x29, x30, [sp, #24]
   c  str  x19, [sp, #40]
  10  add  x29, sp, #0x18
  14  ...                             ; CFA=x29+24 x19@-8 x30@-16 x29@-24 d8@-32
  38  ldp  x29, x30, [sp, #24]        ; CFA=sp+48                      <- window=4 (3 loads + add sp)
  3c  ldr  x19, [sp, #40]
  40  ldr  d8, [sp, #16]
  44  add  sp, sp, #0x30
  48  ret
```

- **Allocation:** 16·N (48) differs from the save-area size (32), so the unwinder knows the allocation
  is a separate `sub sp`. That adds one instruction to both PS and the epilogue window.
- **No hole:** 4 registers is even. The frame record sits misaligned at −24/−16, as the consecutive-slot
  rule predicts.

### A.4 `-fomit-frame-pointer`: x30 among the GPRs

```
region [0, 0x38): SP LLVM x19,x30 d8 N=2  has-prologue has-epilogue [apos=0 PS=2 window=0 tail=1]
   0  str  d8, [sp, #-32]!            ; no frame
   4  stp  x30, x19, [sp, #16]        ; CFA=sp+32                      <- x30 paired with x19
   8  str  w0, [sp, #12]              ; CFA=sp+32 x19@-8 x30@-16 d8@-32
```

- **Hole:** the GPRs [x19, x30] are even, so the hole goes above d8 (at −24). Clang stores a 4-byte
  argument there (`[sp, #12]` = CFA−20).
- **SP mode:** there is no epilogue window, because the body state is already sp-relative.

### A.5 `-fomit-frame-pointer` with x29 as an ordinary register

```
region [0, 0x208): SP LLVM x19,x29,x30 N=21  has-prologue [apos=0 PS=3]
   0  sub  sp, sp, #0x150             ; no frame
   4  str  x29, [sp, #304]            ; CFA=sp+336
   8  stp  x30, x19, [sp, #320]
   c  mov  w19, w0                    ; CFA=sp+336 x19@-8 x30@-16 x29@-32
```

- **Pairing:** the GPRs [x19, x30, x29] pair x19 with x30 and leave x29 unpaired, with the hole above
  it (−24). This is why the frame record bit is separate from the x29 and x30 mask bits.
- **The `ret` at the end:** the encoder makes a second, 2-instruction region that treats the frameless
  `ret` as a lead-in. That costs one entry but needs no NULL region.

### A.6 Clang shrink-wrap: early exit before the frame, island after the ret

```
region [0, 0x40): FP LLVM record=frame x19,x20 N=2  has-prologue apos-override=3 has-epilogue tail-override=3
               [apos=2 PS=5 window=2 tail=3]
   0  sdiv w8, w0, w1                 ; no frame                       <- lead-in (2 instructions)
   4  cbz  w8, 38
   8  stp  x29, x30, [sp, #-32]!                                       <- allocation at apos=2
   c  stp  x20, x19, [sp, #16]        ; CFA=sp+32
  10  mov  x29, sp
  14  ...                             ; CFA=x29+32 x19@-8 x20@-16 x30@-24 x29@-32
  2c  ldp  x20, x19, [sp, #16]        ; CFA=sp+32 (epilogue window)
  30  ldp  x29, x30, [sp], #32
  34  ret                             ; no frame                       <- tail: ret + island (3)
  38  mov  w0, wzr                                                      (early-exit island)
  3c  ret
```

- **Overrides:** the apos override absorbs the frameless lead-in, and PS follows from it. The tail
  length override absorbs the early-exit island laid out after the epilogue.
- **Without them:** 3 regions (a NULL lead-in, the body, a NULL island).

### A.7 BTI-only build: `bti c` as the lead-in

```
region [0, 0x24): FP LLVM record=frame N=1  has-prologue apos-override=2 has-epilogue [apos=1 PS=3 window=1 tail=1]
   0  bti  c                          ; no frame
   4  stp  x29, x30, [sp, #-16]!
   8  mov  x29, sp                    ; CFA=sp+16
   c  ...                             ; CFA=x29+16 x30@-8 x29@-16
  1c  ldp  x29, x30, [sp], #16        ; CFA=sp+16
  20  ret                             ; no frame
```

- **Without the apos override:** every function in a BTI-only build needs an extra region (+27% async).
- **Why pac-ret builds don't need it:** `paciasp` is both the canonical lead-in and the BTI landing pad.

### A.8 GCC: pre-index frame with an interleaved save (PS override)

```
region [0, 0x30): SP GCC record=frame x19 N=16  has-prologue PS-override=4 has-epilogue [apos=0 PS=4 tail=1]
   0  stp  x29, x30, [sp, #-256]!     ; no frame                       <- allocation + frame record store
   4  mov  x29, sp                    ; CFA=sp+256 x30@-248 x29@-256    (record already in its slot)
   8  add  x0, sp, #0x20                                                (body instruction moved into the prologue)
   c  str  x19, [sp, #16]
  10  bl   0                          ; CFA=sp+256 x19@-240 x30@-248 x29@-256
  ...
  28  ldp  x29, x30, [sp], #256
  2c  ret
```

- **GCC layout:** the frame record at the bottom (sp), x19 directly above it, locals above that: the
  sp-anchored convention with O = 0.
- **Frame record during the prologue:** x29 is overwritten (`mov x29, sp`) before the body starts, so
  the unwinder must read it from its slot already. That follows from the shape (a pre-index record
  store, frame ≤ 504 bytes).
- **PS override:** the canonical GCC rule only covers the allocating store, so the scheduled-in
  `add x0, sp, #0x20` needs PS = 4.
- **Epilogue:** no epilogue window. GCC's CFA stays sp-relative, and GCC records all the restores at
  the pop, so the body state holds until then; the slots are still intact.

### A.9 Clang mid-function epilogues (switch with 12 returns)

```
region [0, 0x48):  FP ... has-prologue has-epilogue [PS=3 window=2 tail=1]
   0  stp  x29, x30, [sp, #-32]!
   ...
  2c  br   x10                        (jump table)
  30  mov  w0, w1
  34  bl   0
  38  lsl  w0, w0, #1
  3c  ldp  x20, x19, [sp, #16]        ; epilogue window
  40  ldp  x29, x30, [sp], #32
  44  ret                             ; no frame
region [0x48, 0x64): FP ... has-epilogue [window=2 tail=1]      <- the body resumes, with its own epilogue
  48  add  w0, w1, #0xb               ; CFA=x29+32 ...
  ...
  60  ret
region [0x64, 0x80): ...                                  (13 regions in total)
```

- **Cost:** each return has a full epilogue, so each case is its own region: 13 entries, but only 2
  distinct descriptors.
- **GCC** merges these tails into about 3 regions.
- **DWARF** spends a remember/restore pair plus an advance per epilogue.

### A.10 `pac-ret+leaf`: the signing state toggles per return

```
region [0, 0x4):   NULL                 0  paciasp
region [0x4, 0x30):  NULL ra=pac          ...  2c autiasp
region [0x30, 0x34): NULL                 30  ret
region [0x34, 0x100): NULL ra=pac          ...
region [0x100, 0x104): NULL                100  ret
```

- **Cost:** 5 regions for a leaf function that would otherwise need 1.
- **Possible fix:** prologue and epilogue windows on NULL descriptors with a signed return address
  ([Possible extensions](#possible-extensions)).

### A.11 Homogeneous prologues (`-Oz`), sync-only CFI

```
async region [0, 0x84): FP LLVM record=frame x19..x26  has-prologue PS-override=3 [apos=0 PS=3]
   0  stp  x29, x30, [sp, #-80]!      ; no frame (sync CFI: no row until the body)
   4  bl   OUTLINED_FUNCTION_PROLOG_FRAME0_x19x20...                 <- helper stores x19..x26
   8  sub  sp, sp, #0x20
   c  ...                             ; CFA=x29+80 x19@-8 ... x30@-72 x29@-80
sync descriptor: FP record=frame x19..x26
```

- **Async:** the prologue absorbs the helper call, because the CFI has no row until after it.
- **Sync:** the CFI describes the frame in one row after the helper call, so the whole function gets
  that row's descriptor. The helper call is a call only in name: the helper's own FDE is empty, and
  it never unwinds.

### A.12 GCC outgoing-argument area (O)

```
region [0, 0x50): SP GCC record=frame O=1 N=5  has-prologue PS-override=7 has-epilogue [apos=0 PS=7 tail=1]
   0  sub  sp, sp, #0x50              ; no frame
   4  mov  x8, x3                     ; CFA=sp+80
   ...                                (4 body instructions moved into the prologue)
  18  stp  x29, x30, [sp, #16]                                         <- record at sp+16 (O=1)
  1c  add  x29, sp, #0x10             ; CFA=sp+80 x30@-56 x29@-64
  ...
  44  ldp  x29, x30, [sp, #16]
  48  add  sp, sp, #0x50
  4c  ret
```

- **O:** 16 bytes of outgoing-argument space sit below the frame record, so O = 1 puts the record at
  sp+16.
- **Without O:** about 1.3K GCC functions in the LLVM subset would escape.

### A.13 GCC prologue saves one at a time (PARTIAL)

```
region [0, 0xc): SP GCC record=frame N=3  has-prologue [apos=0 PS=1]
   0  stp  x29, x30, [sp, #-48]!      ; no frame
   4  mov  x29, sp                    ; CFA=sp+48 x30@-40 x29@-48
   8  str  d15, [sp, #32]
region [0xc, 0x14): SP GCC record=frame x19,x20 d15 N=3  PARTIAL live={x19,x20}
   c  fmov s15, s0                    ; CFA=sp+48 d15@-16 x30@-40 x29@-48   <- d15 reused immediately
  10  stp  x19, x20, [sp, #16]
region [0x14, 0x40): SP GCC record=frame x19,x20 d15 N=3  has-epilogue [tail=1]
  14  mov  w20, w0                    ; CFA=sp+48 d15@-16 x20@-24 x19@-32 x30@-40 x29@-48
  ...
```

- **Why it splits:** from 0xc, GCC's CFI says d15 is in its slot while x19 and x20 aren't saved yet.
  The linker can't treat d15 as still holding its own value, and here it indeed doesn't: `fmov s15,
  s0` overwrites it at once. So the state is described exactly, as a PARTIAL descriptor: saved set
  {x29, x30, d15}, referencing the full layout (the next region's descriptor).
- **Cost:** 3 regions instead of Clang's 1.

### A.14 GCC separate shrink-wrapping (sync needs two descriptors)

```
region [0, 0xc):   SP GCC record=frame N=3  has-prologue [PS=1]
   0  stp  x29, x30, [sp, #-48]!
   4  mov  x29, sp
   8  stp  x21, x22, [sp, #32]
region [0xc, 0x34):  SP GCC record=frame x19..x22 N=3  PARTIAL live={x19,x20}
   c  adrp x22, 0                     ; CFA=sp+48 x22@-8 x21@-16 x30@-40 x29@-48
  14  bl   0                          <- call with x19/x20 NOT saved
  18  cbnz w0, 78
  ...
  30  stp  x19, x20, [sp, #16]        <- late save (separate shrink-wrapping)
region [0x34, 0x68): SP GCC record=frame x19..x22 N=3                       <- fully saved
  ...
  58  bl   0
  64  ldp  x19, x20, [sp, #16]        <- early restore
region [0x68, 0x74): PARTIAL live={x19,x20}
  ...
  70  ldp  x29, x30, [sp], #48
region [0x74, 0x78): NULL                  74  b  0   (tail call)
region [0x78, 0x7c): PARTIAL live={x19,x20} 78  stp x19, x20, [sp, #16]   (the cold path saves late too)
region [0x7c, 0x80): full                   7c  bl 0
sync descriptors: PARTIAL live={x19,x20} | full
```

- **What's going on:** GCC saves and restores x19 and x20 only around the loop that uses them. Calls
  happen in both states, so even a sync table needs two descriptors.
- **PARTIAL:** all descriptors share the same layout (slots for x19–x22). The PARTIAL descriptors store
  the saved set {x21, x22, frame record} and reference the fully saved descriptor for the slots.

### A.15 GCC epilogue as a merge point

```
region [0x24, 0x30):  x19..x22                     (x21/x22 saved at 0x20)
region [0x30, 0x84): x19..x23                     (x23 saved at 0x2c; body and loop)
  ...
  80  ldp  x21, x22, [sp, #32]
region [0x84, 0x88): x19..x23 PARTIAL live={x21,x22}
  84  ldr  x23, [sp, #48]
region [0x88, 0x90): x19,x20                      <- 0x88 is ALSO the target of `cbz x2, 88` at 0x1c,
  88  ldp  x19, x20, [sp, #16]                  reached before x21..x23 were ever saved
  8c  ldp  x29, x30, [sp], #64
region [0x90, 0xa8): x19..x23 has-prologue [PS=1] <- the epilogue's frameless ret becomes this region's lead-in
  90  ret
  94  ...
```

- **Why the regions look like this:** they follow GCC's restore states exactly. At 0x88 the CFI says
  x21–x23 hold their own values, which is correct on both incoming paths.
- **The unsafe shortcut:** treating them as "still in their slots until the pop" would read slots that
  were never written on the path from 0x1c.

### A.16 Escapes

| Function | Why it escapes |
|---|---|
| SVE: `svint32_t g(svint32_t)` (Clang `-march=armv8-a+sve`) | the z8/z9 saves are described with CFI expressions |
| SME streaming function (Clang `-march=armv9-a+sme`) | hazard padding puts d8 1 KiB below the GPR saves; no convention matches |
| over 64 KiB of locals, GCC | the SP-mode frame size exceeds N's 64 KiB |
| over 64 KiB of locals, Clang `-fomit-frame-pointer -fstack-clash-protection` | the probe loop moves the CFA to x9 |

### A.17 GCC stack-protector frame (CFA-anchored convention)

From `/usr/bin/cmake` in CentOS Stream 9 (GCC 11, `-fstack-protector-strong`); the descriptor shown is
the body descriptor.

```
body descriptor: SP GCC-CFA-anchored record=frame x19,x20,x21 O=0 N=25
   0  sub  sp, sp, #0x190             ; no frame                       <- whole frame allocated first
   4  adrp x3, ...                    ; CFA=sp+400
   8  stp  x29, x30, [sp, #352]
   c  add  x29, sp, #0x160            ; CFA=sp+400 x29@-48 x30@-40      <- save area at the top
  10  ldr  x3, [x3, #4056]                                              (&__stack_chk_guard)
  14  stp  x19, x20, [sp, #368]
  18  mov  x20, x2                    ; ... x19@-32 x20@-24
  1c  str  x21, [sp, #384]
  20  ...                             ; ... x21@-16                     (8-byte alignment gap at -8)
  30  ldr  x0, [x3]
  34  str  x0, [sp, #344]                                               <- canary at CFA-56, just below
                                                                           the saves; locals below it
```

- **Layout:** GCC's save order (x29, x30, x19, x20, x21 going up, padded to 48 bytes), ending at the
  CFA. With the sp-anchored convention the frame record would need O = 22, beyond O's 4 bits.
- **Why GCC does this:** an overflowing local array hits the canary before the saved registers
  (CVE-2023-4039).
- **Async:** 3 regions. The allocation and frame record form a prologue (PS = 3) whose body state is
  a lone frame record at CFA−48, which an LLVM-convention descriptor with O = 2 describes. Then x19
  and x20 are saved: CFA-anchored with O = 1 (the 32-byte area for x29, x30, x19, x20 ends 16 bytes
  below the CFA). Then the body above, with a canonical epilogue. The intermediate states are ordinary
  descriptors with smaller layouts, not PARTIAL ones, because each intermediate saved set
  forms a valid layout of its own.
- **Without the CFA-anchored convention:** 3% of all functions in the CentOS packages would escape,
  almost all of them with this shape.
