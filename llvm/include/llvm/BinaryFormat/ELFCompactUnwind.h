//===- llvm/BinaryFormat/ELFCompactUnwind.h ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compact unwind tables for AArch64 ELF, which replace .eh_frame and are
// stored in .eh_frame_hdr: the table layout, and what a descriptor says about
// the frame at each instruction of its region. See
// llvm/docs/AArch64ELFCompactUnwind.md.
//
// libunwind decodes descriptors with its own copy of this logic
// (libunwind/src/AArch64CompactUnwind.hpp), which must agree with this one.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BINARYFORMAT_ELFCOMPACTUNWIND_H
#define LLVM_BINARYFORMAT_ELFCOMPACTUNWIND_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"
#include <cstddef>
#include <cstdint>
#include <optional>

namespace llvm {
namespace ELFCompactUnwind {

/// The .eh_frame_hdr version of a compact unwind table.
inline constexpr uint8_t Version = 3;

enum : uint8_t {
  /// The table describes call sites only.
  FlagSyncOnly = 1,
};

/// Byte offsets of the header fields. Offsets of parts of the table are u32
/// and relative to its start; addresses are s32 and relative to its start.
enum : unsigned {
  HdrVersion = 0,
  HdrFlags = 1,
  HdrPageCount = 4,
  HdrFirstLevelOffset = 8,
  HdrGlobalCount = 12,
  HdrGlobalOffset = 16,
  HdrPersonalityCount = 20,
  HdrPersonalityOffset = 24,
  HdrSideTableCount = 28,
  HdrSideTableDirOffset = 32,
  HdrEscapeOffset = 36,
  HdrEscapeSize = 40,
  HeaderSize = 44,
};

/// Descriptor pages and side table pages are at most this large.
inline constexpr size_t PageSize = 4096;

/// A page entry holds the region's pc delta from the page in instructions,
/// shifted left by this, and a descriptor index.
inline constexpr unsigned EntryIndexBits = 12;
inline constexpr unsigned MaxDescriptorIndex = (1u << EntryIndexBits) - 1;

/// The global descriptors leave enough indices for a page whose entries all
/// have local descriptors.
inline constexpr unsigned MaxGlobalDescriptors = 4096 - 341;

enum SideTableKind : uint8_t {
  /// The function's LSDA (s32, 0 for none).
  SideTableLSDA = 1,
  /// The function's personality table index (u16), for personality field 7.
  SideTablePersonality = 2,
  /// The PAuth_LR signing instruction, in instructions from the row (s16).
  SideTablePAuthLRSite = 0x80,
};

/// Side table directory entries: u8 kind, u8 pc offset width, u8 payload
/// width, u8 0, u32 first-level offset, u32 page count.
inline constexpr unsigned SideTableDirEntrySize = 12;

enum : uint16_t {
  /// An LSDA page whose payloads are u16 offsets, in 4-byte units, from an s32
  /// base LSDA that follows the flags.
  SidePageCompressed = 1,
};

/// The payload of a compressed LSDA page row without an LSDA.
inline constexpr uint16_t CompressedNoLSDA = 0xffff;

namespace AArch64 {

enum Mode : unsigned {
  ModeNull = 0,
  ModeFP = 1,
  ModeSP = 2,
  ModeEscape = 3,
  ModePartialFP = 4,
  ModePartialSP = 5,
  ModeNoInfo = 6,
};

/// Slot conventions. The 2-bit field stores 0 for LLVM, 1 for LLVM with a
/// shadow call stack, 2 for GCC sp-anchored and 3 for GCC CFA-anchored.
enum Convention : unsigned { ConvLLVM, ConvGCC, ConvGCCTop };

/// Callee-saved registers are numbered x19-x28 = 0-9, x29 = 10, x30 = 11,
/// d8-d15 = 12-19, which is also their order in a descriptor's masks.
enum : int { NumCSRs = 20, CSRX29 = 10, CSRX30 = 11 };
inline constexpr uint32_t FrameRecordCSRs = (1u << CSRX29) | (1u << CSRX30);

/// Descriptor field positions.
enum : unsigned {
  ConvShift = 3,
  RAShift = 5,
  RegsShift = 8,         // x19-x30
  FrameRecordShift = 20, // x29 and x30 form a frame record
  DRegsShift = 21,       // d8-d15
  PersonalityShift = 29,
  OShift = 32,
  NShift = 36,
  HasPrologueShift = 48,
  HasEpilogueShift = 49,
  PSShift = 50,
  AposShift = 54,
  TailLenShift = 56,
  TailSignedShift = 60,
  /// PARTIAL descriptors: the global index of the layout descriptor.
  LayoutShift = 48,
  /// Escape descriptors: the CIE augmentations S (signal frame), G (MTE-tagged
  /// frame) and N (non-hint PAuth instructions), and the program's offset in
  /// the escape area.
  EscSignalShift = 8,
  EscMTEShift = 9,
  EscNonHintShift = 10,
  EscOffsetShift = 32,
};

inline constexpr unsigned MaxN = 4095, MaxO = 15, MaxPS = 15, MaxEpiLen = 15,
                          MaxTailLen = 15;

/// Where saved registers live, as offsets from the CFA.
struct SaveSlots {
  uint32_t Mask = 0;
  int32_t Offsets[NumCSRs] = {};
  void set(int Reg, int32_t Offset) {
    Mask |= 1u << Reg;
    Offsets[Reg] = Offset;
  }
};

/// A descriptor's frame shape, slots and prologue and epilogue lengths, in
/// instructions.
struct Shape {
  unsigned Mode = ModeNull;
  Convention Conv = ConvLLVM;
  bool Partial = false, FP = false, RA = false, WithPC = false, SCS = false,
       TailSigned = false, FrameRecord = false;
  unsigned N = 0, O = 0;
  uint32_t LayoutRegs = 0; // registers with slots
  uint32_t Saved = 0;      // registers saved in the body
  SaveSlots Slots;
  int Area = 0; // bytes from the lowest slot to the CFA
  // Canonical lengths for this shape, with PS computed from the effective
  // lead-in.
  unsigned CanonApos = 0, CanonPS = 0, CanonTailLen = 0;
  // Effective lengths, with overrides applied. EpiLen and AKind are always
  // canonical.
  bool HasPrologue = false, HasEpilogue = false, AKind = false;
  unsigned Apos = 0, PS = 0, EpiLen = 0, TailLen = 0;
};

/// Decodes the frame shape of a NULL, FP, SP or PARTIAL descriptor. Layout is
/// the layout descriptor of a PARTIAL descriptor and is ignored otherwise.
/// Returns false for other modes and invalid descriptors.
LLVM_ABI bool decodeShape(uint64_t Desc, uint64_t Layout, Shape &S);

/// Recomputes the lengths of a decoded shape from Desc's prologue and epilogue
/// fields, which an encoder may change without changing the shape.
LLVM_ABI void computeLengths(uint64_t Desc, Shape &S);

/// The rules at an instruction of a region.
struct Frame {
  bool CFAIsFP = false; // CFA = x29 + CFAOffset, else sp + CFAOffset
  int32_t CFAOffset = 0;
  uint32_t Saved = 0; // registers saved at their slots
  bool RASigned = false;
  bool X18 = false; // x18 = the caller's x18 + 8 (shadow call stack pushed)
};

/// The rules at instruction I of an N-instruction region.
LLVM_ABI Frame frameAt(const Shape &S, unsigned N, unsigned I);

/// The instructions of an N-instruction region at which frameAt's rules may
/// change, in increasing order and starting with 0. The rules are the same
/// from each of them up to the next.
LLVM_ABI SmallVector<unsigned, 8> frameChanges(const Shape &S, unsigned N);

/// For a region with a PC-based RA scheme, the offset of the PAuth_LR signing
/// instruction from the region start in instructions, if the descriptor says
/// where it is; otherwise it is in the PAuth_LR side table.
LLVM_ABI std::optional<unsigned> pauthLRSite(uint64_t Desc, uint64_t Layout);

/// The DWARF register number of a callee-saved register.
inline unsigned dwarfReg(int CSR) {
  return CSR < CSRX29    ? 19 + CSR
         : CSR <= CSRX30 ? 29 + CSR - CSRX29
                         : 72 + CSR - 12;
}

} // namespace AArch64
} // namespace ELFCompactUnwind
} // namespace llvm

#endif // LLVM_BINARYFORMAT_ELFCOMPACTUNWIND_H
