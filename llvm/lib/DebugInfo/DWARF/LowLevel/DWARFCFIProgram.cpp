//===- DWARFCFIProgram.cpp - Parsing the cfi-portions of .debug_frame -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/DWARF/LowLevel/DWARFCFIProgram.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cinttypes>
#include <cstdint>

using namespace llvm;
using namespace dwarf;

StringRef CFIProgram::callFrameString(unsigned Opcode) const {
  return dwarf::CallFrameString(Opcode, Arch);
}

const char *CFIProgram::operandTypeString(CFIProgram::OperandType OT) {
#define ENUM_TO_CSTR(e)                                                        \
  case e:                                                                      \
    return #e;
  switch (OT) {
    ENUM_TO_CSTR(OT_Unset);
    ENUM_TO_CSTR(OT_None);
    ENUM_TO_CSTR(OT_Address);
    ENUM_TO_CSTR(OT_Offset);
    ENUM_TO_CSTR(OT_FactoredCodeOffset);
    ENUM_TO_CSTR(OT_SignedFactCodeOffset);
    ENUM_TO_CSTR(OT_SignedFactDataOffset);
    ENUM_TO_CSTR(OT_UnsignedFactDataOffset);
    ENUM_TO_CSTR(OT_Register);
    ENUM_TO_CSTR(OT_AddressSpace);
    ENUM_TO_CSTR(OT_Expression);
    ENUM_TO_CSTR(OT_RAState);
  }
  return "<unknown CFIProgram::OperandType>";
}

llvm::Expected<uint64_t>
CFIProgram::Instruction::getOperandAsUnsigned(const CFIProgram &CFIP,
                                              uint32_t OperandIdx) const {
  if (OperandIdx >= MaxOperands)
    return createStringError(errc::invalid_argument,
                             "operand index %" PRIu32 " is not valid",
                             OperandIdx);
  OperandType Type = CFIP.getOperandTypes()[Opcode][OperandIdx];
  uint64_t Operand = Ops[OperandIdx];
  switch (Type) {
  case OT_Unset:
  case OT_None:
  case OT_Expression:
    return createStringError(errc::invalid_argument,
                             "op[%" PRIu32 "] has type %s which has no value",
                             OperandIdx, CFIProgram::operandTypeString(Type));

  case OT_Offset:
  case OT_SignedFactDataOffset:
  case OT_UnsignedFactDataOffset:
  case OT_SignedFactCodeOffset:
    return createStringError(
        errc::invalid_argument,
        "op[%" PRIu32 "] has OperandType OT_Offset which produces a signed "
        "result, call getOperandAsSigned instead",
        OperandIdx);

  case OT_Address:
  case OT_Register:
  case OT_AddressSpace:
  case OT_RAState:
    return Operand;

  case OT_FactoredCodeOffset: {
    const uint64_t CodeAlignmentFactor = CFIP.codeAlign();
    if (CodeAlignmentFactor == 0)
      return createStringError(
          errc::invalid_argument,
          "op[%" PRIu32 "] has type OT_FactoredCodeOffset but code alignment "
          "is zero",
          OperandIdx);
    return Operand * CodeAlignmentFactor;
  }
  }
  llvm_unreachable("invalid operand type");
}

llvm::Expected<int64_t>
CFIProgram::Instruction::getOperandAsSigned(const CFIProgram &CFIP,
                                            uint32_t OperandIdx) const {
  if (OperandIdx >= MaxOperands)
    return createStringError(errc::invalid_argument,
                             "operand index %" PRIu32 " is not valid",
                             OperandIdx);
  OperandType Type = CFIP.getOperandTypes()[Opcode][OperandIdx];
  uint64_t Operand = Ops[OperandIdx];
  switch (Type) {
  case OT_Unset:
  case OT_None:
  case OT_Expression:
    return createStringError(errc::invalid_argument,
                             "op[%" PRIu32 "] has type %s which has no value",
                             OperandIdx, CFIProgram::operandTypeString(Type));

  case OT_Address:
  case OT_Register:
  case OT_AddressSpace:
  case OT_RAState:
    return createStringError(
        errc::invalid_argument,
        "op[%" PRIu32 "] has OperandType %s which produces an unsigned result, "
        "call getOperandAsUnsigned instead",
        OperandIdx, CFIProgram::operandTypeString(Type));

  case OT_Offset:
    return (int64_t)Operand;

  case OT_FactoredCodeOffset:
  case OT_SignedFactDataOffset: {
    const int64_t DataAlignmentFactor = CFIP.dataAlign();
    if (DataAlignmentFactor == 0)
      return createStringError(errc::invalid_argument,
                               "op[%" PRIu32 "] has type %s but data "
                               "alignment is zero",
                               OperandIdx, CFIProgram::operandTypeString(Type));
    return int64_t(Operand) * DataAlignmentFactor;
  }

  case OT_SignedFactCodeOffset: {
    const int64_t CodeAlignmentFactor = CFIP.codeAlign();
    if (CodeAlignmentFactor == 0)
      return createStringError(errc::invalid_argument,
                               "op[%" PRIu32 "] has type %s but code "
                               "alignment is zero",
                               OperandIdx, CFIProgram::operandTypeString(Type));
    return int64_t(Operand) * CodeAlignmentFactor;
  }

  case OT_UnsignedFactDataOffset: {
    const int64_t DataAlignmentFactor = CFIP.dataAlign();
    if (DataAlignmentFactor == 0)
      return createStringError(errc::invalid_argument,
                               "op[%" PRIu32
                               "] has type OT_UnsignedFactDataOffset but data "
                               "alignment is zero",
                               OperandIdx);
    return Operand * DataAlignmentFactor;
  }
  }
  llvm_unreachable("invalid operand type");
}

ArrayRef<CFIProgram::OperandType[CFIProgram::MaxOperands]>
CFIProgram::getOperandTypes() {
  static OperandType OpTypes[DW_CFA_restore + 1][MaxOperands];
  // Filled once, safely when called from several threads.
  [[maybe_unused]] static bool Initialized = [] {
#define DECLARE_OP3(OP, OPTYPE0, OPTYPE1, OPTYPE2)                             \
  do {                                                                         \
    OpTypes[OP][0] = OPTYPE0;                                                  \
    OpTypes[OP][1] = OPTYPE1;                                                  \
    OpTypes[OP][2] = OPTYPE2;                                                  \
  } while (false)
#define DECLARE_OP2(OP, OPTYPE0, OPTYPE1)                                      \
  DECLARE_OP3(OP, OPTYPE0, OPTYPE1, OT_None)
#define DECLARE_OP1(OP, OPTYPE0) DECLARE_OP2(OP, OPTYPE0, OT_None)
#define DECLARE_OP0(OP) DECLARE_OP1(OP, OT_None)
    DECLARE_OP1(DW_CFA_set_loc, OT_Address);
    DECLARE_OP1(DW_CFA_advance_loc, OT_FactoredCodeOffset);
    DECLARE_OP1(DW_CFA_advance_loc1, OT_FactoredCodeOffset);
    DECLARE_OP1(DW_CFA_advance_loc2, OT_FactoredCodeOffset);
    DECLARE_OP1(DW_CFA_advance_loc4, OT_FactoredCodeOffset);
    DECLARE_OP1(DW_CFA_MIPS_advance_loc8, OT_FactoredCodeOffset);
    DECLARE_OP2(DW_CFA_def_cfa, OT_Register, OT_Offset);
    DECLARE_OP2(DW_CFA_def_cfa_sf, OT_Register, OT_SignedFactDataOffset);
    DECLARE_OP1(DW_CFA_def_cfa_register, OT_Register);
    DECLARE_OP3(DW_CFA_LLVM_def_aspace_cfa, OT_Register, OT_Offset,
                OT_AddressSpace);
    DECLARE_OP3(DW_CFA_LLVM_def_aspace_cfa_sf, OT_Register,
                OT_SignedFactDataOffset, OT_AddressSpace);
    DECLARE_OP1(DW_CFA_def_cfa_offset, OT_Offset);
    DECLARE_OP1(DW_CFA_def_cfa_offset_sf, OT_SignedFactDataOffset);
    DECLARE_OP1(DW_CFA_def_cfa_expression, OT_Expression);
    DECLARE_OP1(DW_CFA_undefined, OT_Register);
    DECLARE_OP1(DW_CFA_same_value, OT_Register);
    DECLARE_OP2(DW_CFA_offset, OT_Register, OT_UnsignedFactDataOffset);
    DECLARE_OP2(DW_CFA_offset_extended, OT_Register, OT_UnsignedFactDataOffset);
    DECLARE_OP2(DW_CFA_offset_extended_sf, OT_Register,
                OT_SignedFactDataOffset);
    DECLARE_OP2(DW_CFA_val_offset, OT_Register, OT_UnsignedFactDataOffset);
    DECLARE_OP2(DW_CFA_val_offset_sf, OT_Register, OT_SignedFactDataOffset);
    DECLARE_OP2(DW_CFA_register, OT_Register, OT_Register);
    DECLARE_OP2(DW_CFA_expression, OT_Register, OT_Expression);
    DECLARE_OP2(DW_CFA_val_expression, OT_Register, OT_Expression);
    DECLARE_OP1(DW_CFA_restore, OT_Register);
    DECLARE_OP1(DW_CFA_restore_extended, OT_Register);
    DECLARE_OP0(DW_CFA_remember_state);
    DECLARE_OP0(DW_CFA_restore_state);
    DECLARE_OP0(DW_CFA_GNU_window_save);
    DECLARE_OP0(DW_CFA_AARCH64_negate_ra_state_with_pc);
    DECLARE_OP2(DW_CFA_AARCH64_set_ra_state, OT_RAState,
                OT_SignedFactCodeOffset);
    DECLARE_OP1(DW_CFA_GNU_args_size, OT_Offset);
    DECLARE_OP0(DW_CFA_nop);

#undef DECLARE_OP0
#undef DECLARE_OP1
#undef DECLARE_OP2
    return true;
  }();

  return ArrayRef<OperandType[MaxOperands]>(&OpTypes[0], DW_CFA_restore + 1);
}

namespace {
/// Writes the instructions of a CFIProgram for a CIE with other alignment
/// factors (see CFIProgram::encode).
class CFIEncoder {
public:
  CFIEncoder(const CFIProgram &P, raw_ostream &OS, uint64_t CodeAlign,
             int64_t DataAlign, endianness Endian, uint8_t AddressSize)
      : P(P), OS(OS), CodeAlign(CodeAlign), DataAlign(DataAlign),
        Endian(Endian), AddressSize(AddressSize) {}

  Error encode(const CFIProgram::Instruction &I);

private:
  Error notMultiple(const CFIProgram::Instruction &I, int64_t Value,
                    int64_t Factor) {
    return createStringError(
        errc::invalid_argument, "%s: %" PRId64 " is not a multiple of %" PRId64,
        P.callFrameString(I.Opcode).str().c_str(), Value, Factor);
  }

  /// Operand Idx of I, which is factored by the old code or data alignment
  /// factor, factored by the new one instead.
  Expected<int64_t> refactor(const CFIProgram::Instruction &I, unsigned Idx,
                             int64_t Factor) {
    Expected<int64_t> Bytes = I.getOperandAsSigned(P, Idx);
    if (!Bytes)
      return Bytes.takeError();
    if (*Bytes % Factor)
      return notMultiple(I, *Bytes, Factor);
    return *Bytes / Factor;
  }

  Error encodeAdvance(const CFIProgram::Instruction &I);
  Error encodeOffset(const CFIProgram::Instruction &I);
  Error encodeCFARule(const CFIProgram::Instruction &I);
  Error encodeOperands(const CFIProgram::Instruction &I);

  const CFIProgram &P;
  raw_ostream &OS;
  const uint64_t CodeAlign;
  const int64_t DataAlign;
  const endianness Endian;
  const uint8_t AddressSize;
};
} // namespace

Error CFIEncoder::encode(const CFIProgram::Instruction &I) {
  switch (I.Opcode) {
  case DW_CFA_nop:
    return Error::success();
  case DW_CFA_advance_loc:
  case DW_CFA_advance_loc1:
  case DW_CFA_advance_loc2:
  case DW_CFA_advance_loc4:
  case DW_CFA_MIPS_advance_loc8:
    return encodeAdvance(I);
  case DW_CFA_offset:
  case DW_CFA_offset_extended:
  case DW_CFA_offset_extended_sf:
  case DW_CFA_val_offset:
  case DW_CFA_val_offset_sf:
    return encodeOffset(I);
  case DW_CFA_def_cfa:
  case DW_CFA_def_cfa_sf:
  case DW_CFA_def_cfa_offset:
  case DW_CFA_def_cfa_offset_sf:
  case DW_CFA_LLVM_def_aspace_cfa:
  case DW_CFA_LLVM_def_aspace_cfa_sf:
    return encodeCFARule(I);
  case DW_CFA_restore:
  case DW_CFA_restore_extended:
    if (I.Ops[0] < 0x40) {
      OS << char(DW_CFA_restore | I.Ops[0]);
    } else {
      OS << char(DW_CFA_restore_extended);
      encodeULEB128(I.Ops[0], OS);
    }
    return Error::success();
  default:
    return encodeOperands(I);
  }
}

Error CFIEncoder::encodeAdvance(const CFIProgram::Instruction &I) {
  Expected<uint64_t> Bytes = I.getOperandAsUnsigned(P, 0);
  if (!Bytes)
    return Bytes.takeError();
  if (*Bytes % CodeAlign)
    return notMultiple(I, *Bytes, CodeAlign);
  uint64_t Delta = *Bytes / CodeAlign;
  if (Delta < 0x40) {
    OS << char(DW_CFA_advance_loc | Delta);
  } else if (isUInt<8>(Delta)) {
    OS << char(DW_CFA_advance_loc1) << char(Delta);
  } else if (isUInt<16>(Delta)) {
    OS << char(DW_CFA_advance_loc2);
    support::endian::write<uint16_t>(OS, Delta, Endian);
  } else if (isUInt<32>(Delta)) {
    OS << char(DW_CFA_advance_loc4);
    support::endian::write<uint32_t>(OS, Delta, Endian);
  } else {
    return createStringError(errc::invalid_argument,
                             "advance of %" PRIu64 " is out of range", Delta);
  }
  return Error::success();
}

Error CFIEncoder::encodeOffset(const CFIProgram::Instruction &I) {
  bool Val = I.Opcode == DW_CFA_val_offset || I.Opcode == DW_CFA_val_offset_sf;
  uint64_t Reg = I.Ops[0];
  Expected<int64_t> Off = refactor(I, 1, DataAlign);
  if (!Off)
    return Off.takeError();
  if (*Off >= 0 && !Val && Reg < 0x40) {
    OS << char(DW_CFA_offset | Reg);
    encodeULEB128(*Off, OS);
  } else if (*Off >= 0) {
    OS << char(Val ? DW_CFA_val_offset : DW_CFA_offset_extended);
    encodeULEB128(Reg, OS);
    encodeULEB128(*Off, OS);
  } else {
    OS << char(Val ? DW_CFA_val_offset_sf : DW_CFA_offset_extended_sf);
    encodeULEB128(Reg, OS);
    encodeSLEB128(*Off, OS);
  }
  return Error::success();
}

Error CFIEncoder::encodeCFARule(const CFIProgram::Instruction &I) {
  uint8_t UOp, SOp;
  switch (I.Opcode) {
  case DW_CFA_def_cfa:
  case DW_CFA_def_cfa_sf:
    UOp = DW_CFA_def_cfa;
    SOp = DW_CFA_def_cfa_sf;
    break;
  case DW_CFA_def_cfa_offset:
  case DW_CFA_def_cfa_offset_sf:
    UOp = DW_CFA_def_cfa_offset;
    SOp = DW_CFA_def_cfa_offset_sf;
    break;
  default:
    UOp = DW_CFA_LLVM_def_aspace_cfa;
    SOp = DW_CFA_LLVM_def_aspace_cfa_sf;
    break;
  }
  bool HasReg = UOp != DW_CFA_def_cfa_offset;
  Expected<int64_t> Off = I.getOperandAsSigned(P, HasReg ? 1 : 0);
  if (!Off)
    return Off.takeError();
  // The unsigned forms take the offset in bytes, the signed ones factored by
  // the data alignment factor.
  unsigned USize = *Off >= 0 ? getULEB128Size(*Off) : UINT_MAX;
  unsigned SSize =
      *Off % DataAlign == 0 ? getSLEB128Size(*Off / DataAlign) : UINT_MAX;
  if (USize == UINT_MAX && SSize == UINT_MAX)
    return notMultiple(I, *Off, DataAlign);
  bool Signed = SSize < USize;
  OS << char(Signed ? SOp : UOp);
  if (HasReg)
    encodeULEB128(I.Ops[0], OS);
  if (Signed)
    encodeSLEB128(*Off / DataAlign, OS);
  else
    encodeULEB128(*Off, OS);
  if (UOp == DW_CFA_LLVM_def_aspace_cfa)
    encodeULEB128(I.Ops[2], OS);
  return Error::success();
}

Error CFIEncoder::encodeOperands(const CFIProgram::Instruction &I) {
  auto Types = CFIProgram::getOperandTypes()[I.Opcode];
  if (Types[0] == CFIProgram::OT_Unset)
    return createStringError(errc::invalid_argument,
                             "cannot encode CFI opcode 0x%" PRIx8, I.Opcode);
  OS << char(I.Opcode);
  for (unsigned Idx = 0; Idx < CFIProgram::MaxOperands; ++Idx) {
    switch (Types[Idx]) {
    case CFIProgram::OT_Unset:
    case CFIProgram::OT_None:
      return Error::success();
    case CFIProgram::OT_Register:
    case CFIProgram::OT_Offset:
    case CFIProgram::OT_AddressSpace:
    case CFIProgram::OT_RAState:
      encodeULEB128(I.Ops[Idx], OS);
      break;
    case CFIProgram::OT_Address:
      if (AddressSize == 4)
        support::endian::write<uint32_t>(OS, I.Ops[Idx], Endian);
      else if (AddressSize == 8)
        support::endian::write<uint64_t>(OS, I.Ops[Idx], Endian);
      else
        return createStringError(errc::invalid_argument,
                                 "unsupported address size %" PRIu8,
                                 AddressSize);
      break;
    case CFIProgram::OT_Expression: {
      StringRef Bytes = I.Expression ? I.Expression->getData() : StringRef();
      encodeULEB128(Bytes.size(), OS);
      OS << Bytes;
      break;
    }
    case CFIProgram::OT_FactoredCodeOffset:
      // Only advances have these, and they are encoded above.
      return createStringError(errc::invalid_argument,
                               "cannot encode CFI opcode 0x%" PRIx8, I.Opcode);
    case CFIProgram::OT_SignedFactCodeOffset:
    case CFIProgram::OT_UnsignedFactDataOffset:
    case CFIProgram::OT_SignedFactDataOffset: {
      bool Code = Types[Idx] == CFIProgram::OT_SignedFactCodeOffset;
      Expected<int64_t> V =
          refactor(I, Idx, Code ? int64_t(CodeAlign) : DataAlign);
      if (!V)
        return V.takeError();
      if (Types[Idx] == CFIProgram::OT_SignedFactCodeOffset ||
          Types[Idx] == CFIProgram::OT_SignedFactDataOffset)
        encodeSLEB128(*V, OS);
      else if (*V >= 0)
        encodeULEB128(*V, OS);
      else
        return createStringError(errc::invalid_argument,
                                 "%s: negative unsigned operand",
                                 P.callFrameString(I.Opcode).str().c_str());
      break;
    }
    }
  }
  return Error::success();
}

Error CFIProgram::encode(raw_ostream &OS, uint64_t NewCodeAlign,
                         int64_t NewDataAlign, bool IsLittleEndian,
                         uint8_t AddressSize) const {
  if (NewCodeAlign == 0 || NewDataAlign == 0)
    return createStringError(errc::invalid_argument,
                             "alignment factors must not be zero");
  CFIEncoder Encoder(*this, OS, NewCodeAlign, NewDataAlign,
                     IsLittleEndian ? endianness::little : endianness::big,
                     AddressSize);
  for (const Instruction &I : Instructions)
    if (Error E = Encoder.encode(I))
      return E;
  return Error::success();
}
