//===- ELFCompactUnwindPrinter.cpp - ELF compact unwind table printer -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ELFCompactUnwindPrinter.h"
#include "llvm-readobj.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/BinaryFormat/ELFCompactUnwind.h"
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/DebugInfo/DWARF/DWARFCFIPrinter.h"
#include "llvm/DebugInfo/DWARF/DWARFUnwindTablePrinter.h"
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFCFIProgram.h"
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFDataExtractorSimple.h"
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFUnwindTable.h"
#include "llvm/Object/Error.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ScopedPrinter.h"

using namespace llvm;
using namespace llvm::ELFCompactUnwind;
using namespace llvm::ELFCompactUnwind::AArch64;
using dwarf::UnwindLocation;
using dwarf::UnwindRow;

namespace {
class TablePrinter {
public:
  TablePrinter(ScopedPrinter &W, ArrayRef<uint8_t> Table, uint64_t Address,
               bool IsLittleEndian, StringRef FileName)
      : W(W), Table(Table), Address(Address), IsLittleEndian(IsLittleEndian),
        FileName(FileName), DE(Table, IsLittleEndian) {}

  void print();

private:
  [[noreturn]] void error(const Twine &Msg) {
    reportError(object::createError("compact unwind table: " + Msg), FileName);
  }
  uint64_t read(uint64_t Off, unsigned Size) {
    if (Off > Table.size() || Table.size() - Off < Size)
      error("offset 0x" + Twine::utohexstr(Off) + " is out of range");
    return DE.getUnsigned(&Off, Size);
  }
  uint16_t u16(uint64_t Off) { return read(Off, 2); }
  uint32_t u32(uint64_t Off) { return read(Off, 4); }
  uint64_t u64(uint64_t Off) { return read(Off, 8); }
  // An s32 address field.
  uint64_t addr(uint64_t Off) { return Address + int64_t(int32_t(u32(Off))); }

  // A page of a two-level index: its pc, the next page's pc and its offset.
  struct Page {
    uint64_t PC, NextPC, Offset;
  };
  Page page(uint64_t FirstLevel, uint32_t K) {
    return {addr(FirstLevel + 8 * K), addr(FirstLevel + 8 * (K + 1)),
            u32(FirstLevel + 8 * K + 4)};
  }

  struct SideTable {
    uint8_t Kind, PCWidth, PayloadWidth;
    uint32_t FirstLevel, PageCount;
  };
  SideTable sideTable(uint32_t I);
  // The last row of the side table of the given kind at or before PC: the
  // row's pc and its payload (for a compressed LSDA page, the LSDA's offset
  // from the table, or 0).
  std::optional<std::pair<uint64_t, uint32_t>> findSideRow(uint8_t Kind,
                                                           uint64_t PC);

  void printHeader();
  void printPersonalities();
  void printRegions();
  void printRegion(uint64_t Start, uint64_t End, uint64_t Desc, bool Global,
                   unsigned Index);
  void printDescriptor(uint64_t Start, uint64_t End, uint64_t Desc, bool Global,
                       unsigned Index);
  void printEscape(uint64_t Start, uint64_t End, uint64_t Desc);
  void printRows(SmallVectorImpl<UnwindRow> &Rows, uint64_t End);
  void printSideTables();

  ScopedPrinter &W;
  ArrayRef<uint8_t> Table;
  uint64_t Address;
  bool IsLittleEndian;
  StringRef FileName;
  DataExtractor DE;
  uint32_t GlobalCount = 0, GlobalOffset = 0;
  // The offset of the current page's local descriptors.
  uint32_t LocalsOffset = 0;
};
} // namespace

void TablePrinter::print() {
  DictScope D(W, "CompactUnwindTable");
  if (Table.size() < HeaderSize)
    error("the header is truncated");
  GlobalCount = u32(HdrGlobalCount);
  GlobalOffset = u32(HdrGlobalOffset);
  printHeader();
  printPersonalities();
  {
    ListScope L(W, "GlobalDescriptors");
    for (uint32_t I = 0; I < GlobalCount; ++I)
      W.startLine() << format("%u: 0x%016" PRIx64 "\n", I,
                              u64(GlobalOffset + 8 * I));
  }
  printRegions();
  printSideTables();
}

void TablePrinter::printHeader() {
  W.printNumber("Version", Table[HdrVersion]);
  SmallVector<FlagEntry, 1> Flags;
  if (Table[HdrFlags] & FlagSyncOnly)
    Flags.emplace_back("SyncOnly", unsigned(FlagSyncOnly));
  W.printFlags("Flags", Table[HdrFlags], Flags);
  W.printNumber("PageCount", u32(HdrPageCount));
  W.printHex("FirstLevelOffset", u32(HdrFirstLevelOffset));
  W.printNumber("GlobalDescriptorCount", GlobalCount);
  W.printHex("GlobalDescriptorOffset", GlobalOffset);
  W.printNumber("PersonalityCount", u32(HdrPersonalityCount));
  W.printHex("PersonalityOffset", u32(HdrPersonalityOffset));
  W.printNumber("SideTableCount", u32(HdrSideTableCount));
  W.printHex("SideTableDirectoryOffset", u32(HdrSideTableDirOffset));
  W.printHex("EscapeOffset", u32(HdrEscapeOffset));
  W.printHex("EscapeSize", u32(HdrEscapeSize));
}

void TablePrinter::printPersonalities() {
  ListScope L(W, "Personalities");
  uint32_t Offset = u32(HdrPersonalityOffset);
  for (uint32_t I = 0, E = u32(HdrPersonalityCount); I < E; ++I)
    W.startLine() << format("%u: 0x%" PRIx64 "\n", I,
                            Address + int64_t(int32_t(u32(Offset + 4 * I))));
}

void TablePrinter::printRegions() {
  ListScope L(W, "Regions");
  uint32_t FirstLevel = u32(HdrFirstLevelOffset);
  for (uint32_t K = 0, E = u32(HdrPageCount); K < E; ++K) {
    Page P = page(FirstLevel, K);
    uint32_t Count = u16(P.Offset);
    uint32_t LocalOffset = u16(P.Offset + 2);
    for (uint32_t J = 0; J < Count; ++J) {
      uint32_t Entry = u32(P.Offset + 4 + 4 * J);
      uint64_t Start = P.PC + 4 * uint64_t(Entry >> EntryIndexBits);
      uint64_t End =
          J + 1 < Count
              ? P.PC + 4 * uint64_t(u32(P.Offset + 8 + 4 * J) >> EntryIndexBits)
              : P.NextPC;
      if (End < Start)
        error("regions are out of order at 0x" + Twine::utohexstr(Start));
      uint32_t Index = Entry & MaxDescriptorIndex;
      bool Global = Index < GlobalCount;
      LocalsOffset = P.Offset + LocalOffset;
      uint64_t Desc = Global ? u64(GlobalOffset + 8 * Index)
                             : u64(LocalsOffset + 8 * (Index - GlobalCount));
      printRegion(Start, End, Desc, Global,
                  Global ? Index : Index - GlobalCount);
    }
  }
}

void TablePrinter::printRegion(uint64_t Start, uint64_t End, uint64_t Desc,
                               bool Global, unsigned Index) {
  DictScope D(W, "Region");
  W.printHex("Start", Start);
  W.printHex("End", End);
  W.startLine() << format("Descriptor: 0x%016" PRIx64 " (%s %u)\n", Desc,
                          Global ? "global" : "local", Index);
  switch (Desc & 7) {
  case ModeNoInfo:
    W.printString("Mode", "NoInfo");
    return;
  case 7:
    W.printString("Mode", "Reserved");
    return;
  case ModeEscape:
    printEscape(Start, End, Desc);
    return;
  default:
    printDescriptor(Start, End, Desc, Global, Index);
    return;
  }
}

static std::string registerList(uint32_t Regs) {
  std::string S;
  raw_string_ostream OS(S);
  ListSeparator LS;
  for (int R = 0; R < NumCSRs; ++R) {
    if (!(Regs >> R & 1))
      continue;
    OS << LS;
    if (R < CSRX29)
      OS << "x" << 19 + R;
    else if (R <= CSRX30)
      OS << "x" << 29 + R - CSRX29;
    else
      OS << "d" << 8 + R - 12;
  }
  return S;
}

static StringRef raName(unsigned Field) {
  static const char *const Names[] = {"",
                                      "A",
                                      "B",
                                      "A, PC",
                                      "B, PC",
                                      "A, PC, non-hint",
                                      "B, PC, non-hint",
                                      "reserved"};
  return Names[Field];
}

// Unsigned and unchanged registers and unspecified rules need no row entry.
// Removing them makes rows from DWARF programs and from descriptors agree.
static void normalize(UnwindRow &Row) {
  dwarf::RegisterLocations &Locs = Row.getRegisterLocations();
  for (uint32_t Reg : Locs.getRegisters()) {
    UnwindLocation L = *Locs.getRegisterLocation(Reg);
    if (L.getLocation() == UnwindLocation::Same ||
        L.getLocation() == UnwindLocation::Unspecified ||
        (Reg == 34 && L.getLocation() == UnwindLocation::Constant &&
         L.getConstant() == 0))
      Locs.removeRegisterLocation(Reg);
  }
}

void TablePrinter::printRows(SmallVectorImpl<UnwindRow> &Rows, uint64_t End) {
  SmallVector<UnwindRow, 8> Out;
  for (UnwindRow &Row : Rows) {
    if (Row.getAddress() >= End)
      break;
    normalize(Row);
    if (!Out.empty() && Out.back().getAddress() == Row.getAddress())
      Out.pop_back();
    if (!Out.empty() && Out.back().getCFAValue() == Row.getCFAValue() &&
        Out.back().getRegisterLocations() == Row.getRegisterLocations())
      continue;
    Out.push_back(Row);
  }
  ListScope L(W, "Rows");
  for (const UnwindRow &Row : Out)
    W.startLine() << Row;
}

void TablePrinter::printDescriptor(uint64_t Start, uint64_t End, uint64_t Desc,
                                   bool Global, unsigned Index) {
  unsigned Mode = Desc & 7;
  bool Partial = Mode == ModePartialFP || Mode == ModePartialSP;
  uint64_t Layout = 0;
  unsigned LayoutIndex = (Desc >> LayoutShift) & MaxDescriptorIndex;
  bool GlobalLayout = LayoutIndex < GlobalCount;
  if (Partial) {
    // Any page can use a global descriptor, so only a global layout index
    // means the same in all of them.
    if (Global && !GlobalLayout)
      error("global descriptor " + Twine(Index) + " has local layout " +
            "descriptor " + Twine(LayoutIndex - GlobalCount));
    Layout = GlobalLayout ? u64(GlobalOffset + 8 * LayoutIndex)
                          : u64(LocalsOffset + 8 * (LayoutIndex - GlobalCount));
  }
  static const char *const ModeNames[] = {"NULL", "FP",         "SP",
                                          "",     "PARTIAL FP", "PARTIAL SP"};
  W.printString("Mode", ModeNames[Mode]);
  Shape S;
  if (!decodeShape(Desc, Layout, S)) {
    W.printString("Error", "invalid descriptor");
    return;
  }
  if (Partial)
    W.startLine() << "Layout: " << (GlobalLayout ? "global " : "local ")
                  << (GlobalLayout ? LayoutIndex : LayoutIndex - GlobalCount)
                  << "\n";
  static const char *const ConvNames[] = {"LLVM", "GCC", "GCC CFA-anchored"};
  W.printString("Convention", ConvNames[S.Conv]);
  if (S.SCS)
    W.printString("ShadowCallStack", "Yes");
  if (Mode != ModeNull) {
    W.printString("Registers", registerList(S.LayoutRegs));
    if (Partial)
      W.printString("Saved", registerList(S.Saved));
    if (S.FrameRecord)
      W.printString("FrameRecord", "Yes");
    W.printNumber("O", S.O);
  }
  W.printNumber("N", S.N);
  if (unsigned RA = (Desc >> RAShift) & 7)
    W.printString("RA", raName(RA));
  if (unsigned Pers = (Desc >> PersonalityShift) & 7)
    W.printNumber("Personality", Pers);
  if (S.HasPrologue)
    W.startLine() << "Prologue: apos=" << S.Apos << " PS=" << S.PS << "\n";
  if (S.HasEpilogue)
    W.startLine() << "Epilogue: window=" << S.EpiLen << " tail=" << S.TailLen
                  << (S.TailSigned ? " tail-signed" : "") << "\n";
  if (S.WithPC) {
    if (std::optional<unsigned> Site = pauthLRSite(Desc, Layout))
      W.printHex("PAuthLRSite", Start + 4 * uint64_t(*Site));
    else if (auto Row = findSideRow(SideTablePAuthLRSite, Start))
      W.printHex("PAuthLRSite",
                 Row->first + 4 * int64_t(int16_t(uint16_t(Row->second))));
    else
      W.printString("PAuthLRSite", "missing");
  }

  // The rules change only in the prologue and the epilogue.
  uint64_t N = (End - Start) / 4;
  SmallVector<UnwindRow, 8> Rows;
  for (uint64_t I = 0; I < N; ++I) {
    if (I > S.PS + 1 && I + S.EpiLen + S.TailLen + 1 < N)
      I = N - S.EpiLen - S.TailLen - 1;
    Frame F = frameAt(S, N, I);
    UnwindRow Row;
    Row.setAddress(Start + 4 * I);
    Row.getCFAValue() = UnwindLocation::createIsRegisterPlusOffset(
        F.CFAIsFP ? 29 : 31, F.CFAOffset);
    dwarf::RegisterLocations &Locs = Row.getRegisterLocations();
    for (int R = 0; R < NumCSRs; ++R)
      if (F.Saved >> R & 1)
        Locs.setRegisterLocation(
            dwarfReg(R),
            UnwindLocation::createAtCFAPlusOffset(S.Slots.Offsets[R]));
    if (F.X18) {
      // DW_CFA_val_expression x18, DW_OP_breg18 -8
      static const uint8_t Expr[] = {dwarf::DW_OP_breg18, 0x78};
      Locs.setRegisterLocation(
          18, UnwindLocation::createIsDWARFExpression(DWARFExpression(
                  DataExtractor(ArrayRef(Expr), IsLittleEndian), 8)));
    }
    if (F.RASigned)
      Locs.setRegisterLocation(
          34, UnwindLocation::createIsConstant(S.WithPC ? 2 : 1));
    Rows.push_back(std::move(Row));
  }
  printRows(Rows, End);
}

void TablePrinter::printEscape(uint64_t Start, uint64_t End, uint64_t Desc) {
  W.printString("Mode", "Escape");
  if (unsigned RA = (Desc >> RAShift) & 7)
    W.printString("RA", raName(RA));
  std::string Aug;
  if ((Desc >> EscSignalShift) & 1)
    Aug += 'S';
  if ((Desc >> EscMTEShift) & 1)
    Aug += 'G';
  if ((Desc >> EscNonHintShift) & 1)
    Aug += 'N';
  if (!Aug.empty())
    W.printString("Augmentation", Aug);
  if (unsigned Pers = (Desc >> PersonalityShift) & 7)
    W.printNumber("Personality", Pers);

  // A ULEB128 length and DWARF CFA instructions for the implied initial state:
  // code alignment 4, data alignment -8, CFA = sp, nothing saved.
  uint64_t EscapeOffset = u32(HdrEscapeOffset);
  uint64_t EscapeEnd = EscapeOffset + u32(HdrEscapeSize);
  uint64_t Offset = EscapeOffset + (Desc >> EscOffsetShift);
  if (EscapeEnd > Table.size() || Offset >= EscapeEnd)
    error("escape program at 0x" + Twine::utohexstr(Offset) +
          " is out of range");
  DataExtractor::Cursor C(Offset);
  uint64_t Length = DE.getULEB128(C);
  if (!C || C.tell() + Length > EscapeEnd) {
    consumeError(C.takeError());
    error("escape program at 0x" + Twine::utohexstr(Offset) +
          " is out of range");
  }
  W.printHex("ProgramOffset", Offset);
  uint64_t ProgramStart = C.tell();
  DWARFDataExtractorSimple Data(Table.slice(0, ProgramStart + Length),
                                IsLittleEndian, 8);
  dwarf::CFIProgram Program(4, -8, Triple::aarch64);
  if (Error E = Program.parse(Data, &ProgramStart, ProgramStart + Length))
    reportError(std::move(E), FileName);
  W.startLine() << "Program:\n";
  W.indent();
  DIDumpOptions DumpOpts;
  DumpOpts.IsEH = true;
  dwarf::printCFIProgram(Program, W.getOStream(), DumpOpts, W.getIndentLevel(),
                         Start);
  W.unindent();

  UnwindRow Row;
  Row.setAddress(Start);
  Row.getCFAValue() = UnwindLocation::createIsRegisterPlusOffset(31, 0);
  dwarf::RegisterLocations Initial;
  SmallVector<UnwindRow, 8> Rows;
  if (Error E =
          dwarf::parseRows(Program, Row, &Initial,
                           [&](const UnwindRow &R) { Rows.push_back(R); }))
    reportError(std::move(E), FileName);
  Rows.push_back(Row);
  printRows(Rows, End);
}

TablePrinter::SideTable TablePrinter::sideTable(uint32_t I) {
  uint64_t Entry = u32(HdrSideTableDirOffset) + SideTableDirEntrySize * I;
  SideTable T{uint8_t(read(Entry, 1)), uint8_t(read(Entry + 1, 1)),
              uint8_t(read(Entry + 2, 1)), u32(Entry + 4), u32(Entry + 8)};
  if ((T.PCWidth != 2 && T.PCWidth != 4) ||
      (T.PayloadWidth != 2 && T.PayloadWidth != 4))
    error("side table " + Twine(I) + " has unsupported widths");
  return T;
}

std::optional<std::pair<uint64_t, uint32_t>>
TablePrinter::findSideRow(uint8_t Kind, uint64_t PC) {
  for (uint32_t I = 0, E = u32(HdrSideTableCount); I < E; ++I) {
    SideTable T = sideTable(I);
    if (T.Kind != Kind)
      continue;
    std::optional<std::pair<uint64_t, uint32_t>> Found;
    for (uint32_t K = 0; K < T.PageCount; ++K) {
      Page P = page(T.FirstLevel, K);
      if (P.PC > PC)
        break;
      uint32_t Count = u16(P.Offset);
      bool Compressed = u16(P.Offset + 2) & SidePageCompressed;
      uint64_t PCs = P.Offset + (Compressed ? 8 : 4);
      uint64_t Payloads =
          Compressed ? PCs + 2 * Count
                     : alignTo(PCs + T.PCWidth * Count, T.PayloadWidth);
      for (uint32_t J = 0; J < Count; ++J) {
        uint64_t RowPC = P.PC + 4 * read(PCs + T.PCWidth * J, T.PCWidth);
        if (RowPC > PC)
          break;
        uint32_t Payload;
        if (Compressed) {
          uint16_t Delta = u16(Payloads + 2 * J);
          Payload = Delta == CompressedNoLSDA
                        ? 0
                        : u32(P.Offset + 4) + 4 * uint32_t(Delta);
        } else {
          Payload = read(Payloads + T.PayloadWidth * J, T.PayloadWidth);
        }
        Found = {RowPC, Payload};
      }
    }
    return Found;
  }
  return std::nullopt;
}

void TablePrinter::printSideTables() {
  ListScope L(W, "SideTables");
  for (uint32_t I = 0, E = u32(HdrSideTableCount); I < E; ++I) {
    SideTable T = sideTable(I);
    DictScope D(W, "SideTable");
    StringRef Name = T.Kind == SideTableLSDA          ? "LSDA"
                     : T.Kind == SideTablePersonality ? "Personality"
                     : T.Kind == SideTablePAuthLRSite ? "PAuthLRSite"
                                                      : "Unknown";
    W.startLine() << format("Kind: %s (0x%x)\n", Name.data(), T.Kind);
    W.printNumber("PCWidth", T.PCWidth);
    W.printNumber("PayloadWidth", T.PayloadWidth);
    ListScope PL(W, "Pages");
    for (uint32_t K = 0; K < T.PageCount; ++K) {
      Page P = page(T.FirstLevel, K);
      DictScope PD(W, "Page");
      W.printHex("Address", P.PC);
      uint32_t Count = u16(P.Offset);
      bool Compressed = u16(P.Offset + 2) & SidePageCompressed;
      if (Compressed)
        W.printHex("BaseLSDA", addr(P.Offset + 4));
      uint64_t PCs = P.Offset + (Compressed ? 8 : 4);
      uint64_t Payloads =
          Compressed ? PCs + 2 * Count
                     : alignTo(PCs + T.PCWidth * Count, T.PayloadWidth);
      ListScope RL(W, "Rows");
      for (uint32_t J = 0; J < Count; ++J) {
        uint64_t RowPC = P.PC + 4 * read(PCs + T.PCWidth * J, T.PCWidth);
        W.startLine() << format("0x%" PRIx64 ": ", RowPC);
        if (Compressed) {
          uint16_t Delta = u16(Payloads + 2 * J);
          if (Delta == CompressedNoLSDA)
            W.getOStream() << "none\n";
          else
            W.getOStream() << format("0x%" PRIx64 "\n",
                                     addr(P.Offset + 4) + 4 * uint64_t(Delta));
          continue;
        }
        uint64_t Payload = read(Payloads + T.PayloadWidth * J, T.PayloadWidth);
        if (T.Kind == SideTableLSDA) {
          if (Payload == 0)
            W.getOStream() << "none\n";
          else
            W.getOStream() << format("0x%" PRIx64 "\n",
                                     Address + int64_t(int32_t(Payload)));
        } else if (T.Kind == SideTablePAuthLRSite) {
          W.getOStream() << format("0x%" PRIx64 "\n",
                                   RowPC +
                                       4 * int64_t(int16_t(uint16_t(Payload))));
        } else {
          W.getOStream() << Payload << "\n";
        }
      }
    }
  }
}

void llvm::printELFCompactUnwindTable(ScopedPrinter &W, ArrayRef<uint8_t> Table,
                                      uint64_t Address, bool IsLittleEndian,
                                      StringRef FileName) {
  TablePrinter(W, Table, Address, IsLittleEndian, FileName).print();
}
