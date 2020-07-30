//===--- SpecializationMangler.cpp - mangling of specializations ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/SpecializationMangler.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/Basic/Demangler.h"
#include "swift/Basic/ManglingMacros.h"

using namespace swift;
using namespace NewMangling;

void SpecializationMangler::beginMangling() {
  ASTMangler::beginMangling();
  if (Fragile)
    ArgOpBuffer << 'q';
  ArgOpBuffer << char(uint8_t(Pass) + '0');
}

std::string SpecializationMangler::finalize() {
  std::string MangledSpecialization = ASTMangler::finalize();
  Demangler D(MangledSpecialization);
  NodePointer TopLevel = D.demangleTopLevel();

  StringRef FuncName = Function->getName();
  NodePointer FuncTopLevel;
  if (FuncName.startswith(MANGLING_PREFIX_STR)) {
    FuncTopLevel = Demangler(FuncName).demangleTopLevel();
    assert(FuncTopLevel);
  } else if (FuncName.startswith("_T")) {
    FuncTopLevel = demangleSymbolAsNode(FuncName.data(), FuncName.size());
  }
  if (!FuncTopLevel) {
    FuncTopLevel = NodeFactory::create(Node::Kind::Global);
    FuncTopLevel->addChild(NodeFactory::create(Node::Kind::Identifier, FuncName));
  }
  for (NodePointer FuncChild : *FuncTopLevel) {
    assert(FuncChild->getKind() != Node::Kind::Suffix ||
           FuncChild->getText() == "merged");
    TopLevel->addChild(FuncChild);
  }
  return Demangle::mangleNodeNew(TopLevel);
}

//===----------------------------------------------------------------------===//
//                           Generic Specialization
//===----------------------------------------------------------------------===//

std::string GenericSpecializationMangler::mangle() {
  beginMangling();
  
  SILFunctionType *FTy = Function->getLoweredFunctionType();
  CanGenericSignature Sig = FTy->getGenericSignature();

  unsigned idx = 0;
  bool First = true;
  for (Type DepType : Sig->getAllDependentTypes()) {
    // It is sufficient to only mangle the substitutions of the "primary"
    // dependent types. As all other dependent types are just derived from the
    // primary types, this will give us unique symbol names.
    if (DepType->is<GenericTypeParamType>()) {
      appendType(Subs[idx].getReplacement()->getCanonicalType());
      appendListSeparator(First);
    }
    ++idx;
  }
  assert(idx == Subs.size() && "subs not parallel to dependent types");
  assert(!First && "no generic substitutions");
  
  appendSpecializationOperator(isReAbstracted ? "Tg" : "TG");
  return finalize();
}

//===----------------------------------------------------------------------===//
//                         Partial Generic Specialization
//===----------------------------------------------------------------------===//

std::string PartialSpecializationMangler::mangle() {
  beginMangling();
  appendType(SpecializedFnTy);
  appendSpecializationOperator(isReAbstracted ? "Tp" : "TP");
  return finalize();
}

//===----------------------------------------------------------------------===//
//                      Function Signature Optimizations
//===----------------------------------------------------------------------===//

FunctionSignatureSpecializationMangler::
FunctionSignatureSpecializationMangler(Demangle::SpecializationPass P,
                                       IsFragile_t Fragile, SILFunction *F)
  : SpecializationMangler(P, Fragile, F) {
  for (unsigned i = 0, e = F->getLoweredFunctionType()->getNumSILArguments();
       i != e; ++i) {
    (void)i;
    Args.push_back({ArgumentModifierIntBase(ArgumentModifier::Unmodified),
                    nullptr});
  }
  ReturnValue = ReturnValueModifierIntBase(ReturnValueModifier::Unmodified);
}

void
FunctionSignatureSpecializationMangler::
setArgumentDead(unsigned ArgNo) {
  Args[ArgNo].first |= ArgumentModifierIntBase(ArgumentModifier::Dead);
}

void
FunctionSignatureSpecializationMangler::
setArgumentClosureProp(unsigned ArgNo, PartialApplyInst *PAI) {
  auto &Info = Args[ArgNo];
  Info.first = ArgumentModifierIntBase(ArgumentModifier::ClosureProp);
  Info.second = PAI;
}

void
FunctionSignatureSpecializationMangler::
setArgumentClosureProp(unsigned ArgNo, ThinToThickFunctionInst *TTTFI) {
  auto &Info = Args[ArgNo];
  Info.first = ArgumentModifierIntBase(ArgumentModifier::ClosureProp);
  Info.second = TTTFI;
}

void
FunctionSignatureSpecializationMangler::
setArgumentConstantProp(unsigned ArgNo, LiteralInst *LI) {
  auto &Info = Args[ArgNo];
  Info.first = ArgumentModifierIntBase(ArgumentModifier::ConstantProp);
  Info.second = LI;
}

void
FunctionSignatureSpecializationMangler::
setArgumentOwnedToGuaranteed(unsigned ArgNo) {
  Args[ArgNo].first |= ArgumentModifierIntBase(ArgumentModifier::OwnedToGuaranteed);
}

void
FunctionSignatureSpecializationMangler::
setArgumentSROA(unsigned ArgNo) {
  Args[ArgNo].first |= ArgumentModifierIntBase(ArgumentModifier::SROA);
}

void
FunctionSignatureSpecializationMangler::
setArgumentBoxToValue(unsigned ArgNo) {
  Args[ArgNo].first = ArgumentModifierIntBase(ArgumentModifier::BoxToValue);
}

void
FunctionSignatureSpecializationMangler::
setArgumentBoxToStack(unsigned ArgNo) {
  Args[ArgNo].first = ArgumentModifierIntBase(ArgumentModifier::BoxToStack);
}

void
FunctionSignatureSpecializationMangler::
setReturnValueOwnedToUnowned() {
  ReturnValue |= ReturnValueModifierIntBase(ReturnValueModifier::OwnedToUnowned);
}

void
FunctionSignatureSpecializationMangler::mangleConstantProp(LiteralInst *LI) {
  // Append the prefix for constant propagation 'p'.
  ArgOpBuffer << 'p';

  // Then append the unique identifier of our literal.
  switch (LI->getKind()) {
  default:
    llvm_unreachable("unknown literal");
  case ValueKind::FunctionRefInst: {
    SILFunction *F = cast<FunctionRefInst>(LI)->getReferencedFunction();
    ArgOpBuffer << 'f';
    appendIdentifier(F->getName());
    break;
  }
  case ValueKind::GlobalAddrInst: {
    SILGlobalVariable *G = cast<GlobalAddrInst>(LI)->getReferencedGlobal();
    ArgOpBuffer << 'g';
    appendIdentifier(G->getName());
    break;
  }
  case ValueKind::IntegerLiteralInst: {
    APInt apint = cast<IntegerLiteralInst>(LI)->getValue();
    ArgOpBuffer << 'i' << apint;
    break;
  }
  case ValueKind::FloatLiteralInst: {
    APInt apint = cast<FloatLiteralInst>(LI)->getBits();
    ArgOpBuffer << 'd' << apint;
    break;
  }
  case ValueKind::StringLiteralInst: {
    StringLiteralInst *SLI = cast<StringLiteralInst>(LI);
    StringRef V = SLI->getValue();
    assert(V.size() <= 32 && "Cannot encode string of length > 32");
    std::string VBuffer;
    if (V.size() > 0 && (isDigit(V[0]) || V[0] == '_')) {
      VBuffer = "_";
      VBuffer.append(V.data(), V.size());
      V = VBuffer;
    }
    appendIdentifier(V);

    ArgOpBuffer << 's';
    switch (SLI->getEncoding()) {
      case StringLiteralInst::Encoding::UTF8: ArgOpBuffer << 'b'; break;
      case StringLiteralInst::Encoding::UTF16: ArgOpBuffer << 'w'; break;
      case StringLiteralInst::Encoding::ObjCSelector: ArgOpBuffer << 'c'; break;
    }
    break;
  }
  }
}

void
FunctionSignatureSpecializationMangler::mangleClosureProp(SILInstruction *Inst) {
  ArgOpBuffer << 'c';

  // Add in the partial applies function name if we can find one. Assert
  // otherwise. The reason why this is ok to do is currently we only perform
  // closure specialization if we know the function_ref in question. When this
  // restriction is removed, the assert here will fire.
  if (auto *TTTFI = dyn_cast<ThinToThickFunctionInst>(Inst)) {
    auto *FRI = cast<FunctionRefInst>(TTTFI->getCallee());
    appendIdentifier(FRI->getReferencedFunction()->getName());
    return;
  }
  auto *PAI = cast<PartialApplyInst>(Inst);
  auto *FRI = cast<FunctionRefInst>(PAI->getCallee());
  appendIdentifier(FRI->getReferencedFunction()->getName());

  // Then we mangle the types of the arguments that the partial apply is
  // specializing.
  for (auto &Op : PAI->getArgumentOperands()) {
    SILType Ty = Op.get()->getType();
    appendType(Ty.getSwiftRValueType());
  }
}

void FunctionSignatureSpecializationMangler::mangleArgument(
    ArgumentModifierIntBase ArgMod, NullablePtr<SILInstruction> Inst) {
  if (ArgMod == ArgumentModifierIntBase(ArgumentModifier::ConstantProp)) {
    mangleConstantProp(cast<LiteralInst>(Inst.get()));
    return;
  }

  if (ArgMod == ArgumentModifierIntBase(ArgumentModifier::ClosureProp)) {
    mangleClosureProp(Inst.get());
    return;
  }

  if (ArgMod == ArgumentModifierIntBase(ArgumentModifier::Unmodified)) {
    ArgOpBuffer << 'n';
    return;
  }

  if (ArgMod == ArgumentModifierIntBase(ArgumentModifier::BoxToValue)) {
    ArgOpBuffer << 'i';
    return;
  }

  if (ArgMod == ArgumentModifierIntBase(ArgumentModifier::BoxToStack)) {
    ArgOpBuffer << 's';
    return;
  }

  bool hasSomeMod = false;
  if (ArgMod & ArgumentModifierIntBase(ArgumentModifier::Dead)) {
    ArgOpBuffer << 'd';
    hasSomeMod = true;
  }

  if (ArgMod & ArgumentModifierIntBase(ArgumentModifier::OwnedToGuaranteed)) {
    ArgOpBuffer << (hasSomeMod ? 'G' : 'g');
    hasSomeMod = true;
  }
  if (ArgMod & ArgumentModifierIntBase(ArgumentModifier::SROA)) {
    ArgOpBuffer << (hasSomeMod ? 'X' : 'x');
    hasSomeMod = true;
  }

  assert(hasSomeMod && "Unknown modifier");
}

void FunctionSignatureSpecializationMangler::
mangleReturnValue(ReturnValueModifierIntBase RetMod) {
  if (RetMod == ReturnValueModifierIntBase(ReturnValueModifier::Unmodified)) {
    ArgOpBuffer << 'n';
    return;
  }

  bool hasSomeMode = false;
  if (RetMod & ReturnValueModifierIntBase(ReturnValueModifier::Dead)) {
    ArgOpBuffer << 'd';
    hasSomeMode = true;
  }

  if (RetMod & ReturnValueModifierIntBase(ReturnValueModifier::OwnedToUnowned)) {
    ArgOpBuffer << (hasSomeMode ? 'G' : 'g');
  }
}

std::string FunctionSignatureSpecializationMangler::mangle(int UniqueID) {
  ArgOpStorage.clear();
  beginMangling();

  if (UniqueID)
    ArgOpBuffer << UniqueID;

  for (unsigned i : indices(Args)) {
    ArgumentModifierIntBase ArgMod;
    NullablePtr<SILInstruction> Inst;
    std::tie(ArgMod, Inst) = Args[i];
    mangleArgument(ArgMod, Inst);
  }
  ArgOpBuffer << '_';
  mangleReturnValue(ReturnValue);

  appendSpecializationOperator("Tf");
  return finalize();
}
