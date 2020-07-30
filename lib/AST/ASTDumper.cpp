//===--- ASTDumper.cpp - Swift Language AST Dumper ------------------------===//
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
//
//  This file implements dumping for the Swift ASTs.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/QuotedString.h"
#include "swift/AST/AST.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

struct TerminalColor {
  llvm::raw_ostream::Colors Color;
  bool Bold;
};

#define DEF_COLOR(NAME, COLOR, BOLD) \
static const TerminalColor NAME##Color = { llvm::raw_ostream::COLOR, BOLD };

DEF_COLOR(Func, YELLOW, false)
DEF_COLOR(Range, YELLOW, false)
DEF_COLOR(Accessibility, YELLOW, false)
DEF_COLOR(ASTNode, YELLOW, true)
DEF_COLOR(Parameter, YELLOW, false)
DEF_COLOR(Extension, MAGENTA, false)
DEF_COLOR(Pattern, RED, true)
DEF_COLOR(Override, RED, false)
DEF_COLOR(Stmt, RED, true)
DEF_COLOR(Captures, RED, false)
DEF_COLOR(Arguments, RED, false)
DEF_COLOR(TypeRepr, GREEN, false)
DEF_COLOR(LiteralValue, GREEN, false)
DEF_COLOR(Decl, GREEN, true)
DEF_COLOR(Parenthesis, BLUE, false)
DEF_COLOR(Type, BLUE, false)
DEF_COLOR(Discriminator, BLUE, false)
DEF_COLOR(InterfaceType, GREEN, false)
DEF_COLOR(Identifier, GREEN, false)
DEF_COLOR(Expr, MAGENTA, true)
DEF_COLOR(ExprModifier, CYAN, false)
DEF_COLOR(DeclModifier, CYAN, false)
DEF_COLOR(ClosureModifier, CYAN, false)
DEF_COLOR(TypeField, CYAN, false)
DEF_COLOR(Location, CYAN, false)

#undef DEF_COLOR

namespace {
  /// RAII object that prints with the given color, if color is supported on the
  /// given stream.
  class PrintWithColorRAII {
    raw_ostream &OS;
    bool ShowColors;

  public:
    PrintWithColorRAII(raw_ostream &os, TerminalColor color)
    : OS(os), ShowColors(false)
    {
      if (&os == &llvm::errs() || &os == &llvm::outs())
        ShowColors = llvm::errs().has_colors() && llvm::outs().has_colors();

      if (ShowColors)
        OS.changeColor(color.Color, color.Bold);
    }

    ~PrintWithColorRAII() {
      if (ShowColors) {
        OS.resetColor();
      }
    }

    raw_ostream &getOS() const { return OS; }

    template<typename T>
    friend raw_ostream &operator<<(PrintWithColorRAII &&printer,
                                   const T &value){
      printer.OS << value;
      return printer.OS;
    }

    friend raw_ostream &operator<<(PrintWithColorRAII &&printer,
                                   AccessSemantics accessKind){
      switch (accessKind) {
      case AccessSemantics::Ordinary:
        return printer.OS;
      case AccessSemantics::DirectToStorage:
        return printer.OS << " direct_to_storage";
      case AccessSemantics::DirectToAccessor:
        return printer.OS << " direct_to_accessor";
      case AccessSemantics::BehaviorInitialization:
        return printer.OS << " behavior_init";
      }
      llvm_unreachable("bad access kind");
    }
  };
} // end anonymous namespace

//===----------------------------------------------------------------------===//
//  Generic param list printing.
//===----------------------------------------------------------------------===//

void RequirementRepr::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void RequirementRepr::printImpl(raw_ostream &out, bool AsWritten) const {
  auto printTy = [&](const TypeLoc &TyLoc) {
    if (AsWritten && TyLoc.getTypeRepr()) {
      TyLoc.getTypeRepr()->print(out);
    } else {
      TyLoc.getType().print(out);
    }
  };

  auto printLayoutConstraint =
      [&](const LayoutConstraintLoc &LayoutConstraintLoc) {
        LayoutConstraintLoc.getLayoutConstraint()->print(out);
      };

  switch (getKind()) {
  case RequirementReprKind::LayoutConstraint:
    printTy(getSubjectLoc());
    out << " : ";
    printLayoutConstraint(getLayoutConstraintLoc());
    break;

  case RequirementReprKind::TypeConstraint:
    printTy(getSubjectLoc());
    out << " : ";
    printTy(getConstraintLoc());
    break;

  case RequirementReprKind::SameType:
    printTy(getFirstTypeLoc());
    out << " == ";
    printTy(getSecondTypeLoc());
    break;
  }
}

void RequirementRepr::print(raw_ostream &out) const {
  printImpl(out, /*AsWritten=*/false);
}

void GenericParamList::print(llvm::raw_ostream &OS) {
  OS << '<';
  bool First = true;
  for (auto P : *this) {
    if (First) {
      First = false;
    } else {
      OS << ", ";
    }
    OS << P->getName();
    if (!P->getInherited().empty()) {
      OS << " : ";
      P->getInherited()[0].getType().print(OS);
    }
  }

  if (!getRequirements().empty()) {
    OS << " where ";
    interleave(getRequirements(),
               [&](const RequirementRepr &req) {
                 req.print(OS);
               },
               [&] { OS << ", "; });
  }
  OS << '>';
}

void GenericParamList::dump() {
  print(llvm::errs());
  llvm::errs() << '\n';
}

static void printGenericParameters(raw_ostream &OS, GenericParamList *Params) {
  if (!Params)
    return;
  OS << ' ';
  Params->print(OS);
}

//===----------------------------------------------------------------------===//
//  Decl printing.
//===----------------------------------------------------------------------===//

static StringRef
getStorageKindName(AbstractStorageDecl::StorageKindTy storageKind) {
  switch (storageKind) {
  case AbstractStorageDecl::Stored:
    return "stored";
  case AbstractStorageDecl::StoredWithTrivialAccessors:
    return "stored_with_trivial_accessors";
  case AbstractStorageDecl::StoredWithObservers:
    return "stored_with_observers";
  case AbstractStorageDecl::InheritedWithObservers:
    return "inherited_with_observers";
  case AbstractStorageDecl::Addressed:
    return "addressed";
  case AbstractStorageDecl::AddressedWithTrivialAccessors:
    return "addressed_with_trivial_accessors";
  case AbstractStorageDecl::AddressedWithObservers:
    return "addressed_with_observers";
  case AbstractStorageDecl::ComputedWithMutableAddress:
    return "computed_with_mutable_address";
  case AbstractStorageDecl::Computed:
    return "computed";
  }
  llvm_unreachable("bad storage kind");
}

// Print a name.
static void printName(raw_ostream &os, Identifier name) {
  if (name.empty())
    os << "<anonymous>";
  else
    os << name.str();
}

namespace {
  class PrintPattern : public PatternVisitor<PrintPattern> {
  public:
    raw_ostream &OS;
    unsigned Indent;
    bool ShowColors;

    explicit PrintPattern(raw_ostream &os, unsigned indent = 0)
      : OS(os), Indent(indent), ShowColors(false) {
      if (&os == &llvm::errs() || &os == &llvm::outs())
        ShowColors = llvm::errs().has_colors() && llvm::outs().has_colors();
    }

    void printRec(Decl *D) { D->dump(OS, Indent + 2); }
    void printRec(Expr *E) { E->print(OS, Indent + 2); }
    void printRec(Stmt *S) { S->print(OS, Indent + 2); }
    void printRec(TypeRepr *T);
    void printRec(const Pattern *P) {
      PrintPattern(OS, Indent+2).visit(const_cast<Pattern *>(P));
    }

    raw_ostream &printCommon(Pattern *P, const char *Name) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, PatternColor) << Name;

      if (P->isImplicit())
        PrintWithColorRAII(OS, ExprModifierColor) << " implicit";

      if (P->hasType()) {
        PrintWithColorRAII(OS, TypeColor) << " type='";
        P->getType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        PrintWithColorRAII(OS, TypeColor) << "'";
      }
      return OS;
    }

    void visitParenPattern(ParenPattern *P) {
      printCommon(P, "pattern_paren") << '\n';
      printRec(P->getSubPattern());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitTuplePattern(TuplePattern *P) {
      printCommon(P, "pattern_tuple");

      OS << " names=";
      interleave(P->getElements(),
                 [&](const TuplePatternElt &elt) {
                   auto name = elt.getLabel();
                   OS << (name.empty() ? "''" : name.str());
                 },
                 [&] { OS << ","; });

      for (auto &elt : P->getElements()) {
        OS << '\n';
        printRec(elt.getPattern());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitNamedPattern(NamedPattern *P) {
      printCommon(P, "pattern_named");
      PrintWithColorRAII(OS, IdentifierColor) << " '" << P->getNameStr() << "'";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitAnyPattern(AnyPattern *P) {
      printCommon(P, "pattern_any");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitTypedPattern(TypedPattern *P) {
      printCommon(P, "pattern_typed") << '\n';
      printRec(P->getSubPattern());
      if (P->getTypeLoc().getTypeRepr()) {
        OS << '\n';
        printRec(P->getTypeLoc().getTypeRepr());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitIsPattern(IsPattern *P) {
      printCommon(P, "pattern_is")
        << ' ' << getCheckedCastKindName(P->getCastKind()) << ' ';
      P->getCastTypeLoc().getType().print(OS);
      if (auto sub = P->getSubPattern()) {
        OS << '\n';
        printRec(sub);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitExprPattern(ExprPattern *P) {
      printCommon(P, "pattern_expr");
      OS << '\n';
      if (auto m = P->getMatchExpr())
        printRec(m);
      else
        printRec(P->getSubExpr());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitVarPattern(VarPattern *P) {
      printCommon(P, P->isLet() ? "pattern_let" : "pattern_var");
      OS << '\n';
      printRec(P->getSubPattern());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitEnumElementPattern(EnumElementPattern *P) {
      printCommon(P, "pattern_enum_element");
      OS << ' ';
      P->getParentType().getType().print(
        PrintWithColorRAII(OS, TypeColor).getOS());
      PrintWithColorRAII(OS, IdentifierColor) << '.' << P->getName();
      if (P->hasSubPattern()) {
        OS << '\n';
        printRec(P->getSubPattern());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitOptionalSomePattern(OptionalSomePattern *P) {
      printCommon(P, "optional_some_element");
      OS << '\n';
      printRec(P->getSubPattern());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    void visitBoolPattern(BoolPattern *P) {
      printCommon(P, "pattern_bool");
      OS << (P->getValue() ? " true)" : " false)");
    }

  };

  /// PrintDecl - Visitor implementation of Decl::print.
  class PrintDecl : public DeclVisitor<PrintDecl> {
  public:
    raw_ostream &OS;
    unsigned Indent;
    bool ShowColors;

    explicit PrintDecl(raw_ostream &os, unsigned indent = 0)
      : OS(os), Indent(indent), ShowColors(false) {
      if (&os == &llvm::errs() || &os == &llvm::outs())
        ShowColors = llvm::errs().has_colors() && llvm::outs().has_colors();
    }
    
    void printRec(Decl *D) { PrintDecl(OS, Indent + 2).visit(D); }
    void printRec(Expr *E) { E->print(OS, Indent+2); }
    void printRec(Stmt *S) { S->print(OS, Indent+2); }
    void printRec(Pattern *P) { PrintPattern(OS, Indent+2).visit(P); }
    void printRec(TypeRepr *T);

    // Print a field with a value.
    template<typename T>
    raw_ostream &printField(StringRef name, const T &value) {
      OS << " ";
      PrintWithColorRAII(OS, TypeFieldColor) << name;
      OS << "=" << value;
      return OS;
    }

    void printCommon(Decl *D, const char *Name,
                     TerminalColor Color = DeclColor) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, Color) << Name;

      if (D->isImplicit())
        PrintWithColorRAII(OS, DeclModifierColor) << " implicit";
    }

    void printInherited(ArrayRef<TypeLoc> Inherited) {
      if (Inherited.empty())
        return;
      OS << " inherits: ";
      bool First = true;
      for (auto Super : Inherited) {
        if (First)
          First = false;
        else
          OS << ", ";

        Super.getType().print(OS);
      }
    }

    void visitImportDecl(ImportDecl *ID) {
      printCommon(ID, "import_decl");

      if (ID->isExported())
        OS << " exported";

      const char *KindString;
      switch (ID->getImportKind()) {
      case ImportKind::Module:
        KindString = nullptr;
        break;
      case ImportKind::Type:
        KindString = "type";
        break;
      case ImportKind::Struct:
        KindString = "struct";
        break;
      case ImportKind::Class:
        KindString = "class";
        break;
      case ImportKind::Enum:
        KindString = "enum";
        break;
      case ImportKind::Protocol:
        KindString = "protocol";
        break;
      case ImportKind::Var:
        KindString = "var";
        break;
      case ImportKind::Func:
        KindString = "func";
        break;
      }
      if (KindString)
        OS << " kind=" << KindString;

      OS << " '";
      interleave(ID->getFullAccessPath(),
                 [&](const ImportDecl::AccessPathElement &Elem) {
                   OS << Elem.first;
                 },
                 [&] { OS << '.'; });
      OS << "')";
    }

    void visitExtensionDecl(ExtensionDecl *ED) {
      printCommon(ED, "extension_decl", ExtensionColor);
      OS << ' ';
      ED->getExtendedType().print(OS);
      printInherited(ED->getInherited());
      for (Decl *Member : ED->getMembers()) {
        OS << '\n';
        printRec(Member);
      }
      OS << ")";
    }

    void printDeclName(const ValueDecl *D) {
      if (D->getFullName()) {
        PrintWithColorRAII(OS, IdentifierColor)
          << '\"' << D->getFullName() << '\"';
      } else {
        PrintWithColorRAII(OS, IdentifierColor)
          << "'anonname=" << (const void*)D << '\'';
      }
    }

    void visitTypeAliasDecl(TypeAliasDecl *TAD) {
      printCommon(TAD, "typealias");
      PrintWithColorRAII(OS, TypeColor) << " type='";
      if (TAD->getUnderlyingTypeLoc().getType()) {
        PrintWithColorRAII(OS, TypeColor)
          << TAD->getUnderlyingTypeLoc().getType().getString();
      } else {
        PrintWithColorRAII(OS, TypeColor) << "<<<unresolved>>>";
      }
      printInherited(TAD->getInherited());
      OS << "')";
    }

    void printAbstractTypeParamCommon(AbstractTypeParamDecl *decl,
                                      const char *name) {
      printCommon(decl, name);
      if (auto superclassTy = decl->getSuperclass()) {
        OS << " superclass='" << superclassTy->getString() << "'";
      }
    }

    void visitGenericTypeParamDecl(GenericTypeParamDecl *decl) {
      printAbstractTypeParamCommon(decl, "generic_type_param");
      OS << " depth=" << decl->getDepth() << " index=" << decl->getIndex();
      OS << ")";
    }

    void visitAssociatedTypeDecl(AssociatedTypeDecl *decl) {
      printAbstractTypeParamCommon(decl, "associated_type_decl");
      if (auto defaultDef = decl->getDefaultDefinitionType()) {
        OS << " default=";
        defaultDef.print(OS);
      }
      
      OS << ")";
    }

    void visitProtocolDecl(ProtocolDecl *PD) {
      printCommon(PD, "protocol");
      printInherited(PD->getInherited());
      for (auto VD : PD->getMembers()) {
        OS << '\n';
        printRec(VD);
      }
      OS << ")";
    }

    void printCommon(ValueDecl *VD, const char *Name,
                     TerminalColor Color = DeclColor) {
      printCommon((Decl*)VD, Name, Color);

      OS << ' ';
      printDeclName(VD);
      if (AbstractFunctionDecl *AFD = dyn_cast<AbstractFunctionDecl>(VD))
        printGenericParameters(OS, AFD->getGenericParams());
      if (GenericTypeDecl *GTD = dyn_cast<GenericTypeDecl>(VD))
        printGenericParameters(OS, GTD->getGenericParams());

      if (auto *var = dyn_cast<VarDecl>(VD)) {
        PrintWithColorRAII(OS, TypeColor) << " type='";
        if (var->hasType())
          var->getType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        else
          PrintWithColorRAII(OS, TypeColor) << "<null type>";
        PrintWithColorRAII(OS, TypeColor) << "'";
      }

      if (VD->hasInterfaceType()) {
        PrintWithColorRAII(OS, InterfaceTypeColor) << " interface type='";
        VD->getInterfaceType()->print(
            PrintWithColorRAII(OS, InterfaceTypeColor).getOS());
        PrintWithColorRAII(OS, InterfaceTypeColor) << "'";
      }

      if (VD->hasAccessibility()) {
        PrintWithColorRAII(OS, AccessibilityColor) << " access=";
        switch (VD->getFormalAccess()) {
        case Accessibility::Private:
          PrintWithColorRAII(OS, AccessibilityColor) << "private";
          break;
        case Accessibility::FilePrivate:
          PrintWithColorRAII(OS, AccessibilityColor) << "fileprivate";
          break;
        case Accessibility::Internal:
          PrintWithColorRAII(OS, AccessibilityColor) << "internal";
          break;
        case Accessibility::Public:
          PrintWithColorRAII(OS, AccessibilityColor) << "public";
          break;
        case Accessibility::Open:
          PrintWithColorRAII(OS, AccessibilityColor) << "open";
          break;
        }
      }

      if (auto Overridden = VD->getOverriddenDecl()) {
        PrintWithColorRAII(OS, OverrideColor) << " override=";
        Overridden->dumpRef(PrintWithColorRAII(OS, OverrideColor).getOS());
      }

      if (VD->isFinal())
        OS << " final";
      if (VD->isObjC())
        OS << " @objc";
    }

    void printCommon(NominalTypeDecl *NTD, const char *Name,
                     TerminalColor Color = DeclColor) {
      printCommon((ValueDecl *)NTD, Name, Color);

      if (NTD->hasInterfaceType()) {
        if (NTD->hasFixedLayout())
          OS << " @_fixed_layout";
        else
          OS << " @_resilient_layout";
      }
    }

    void visitSourceFile(const SourceFile &SF) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, ASTNodeColor) << "source_file";
      for (Decl *D : SF.Decls) {
        if (D->isImplicit())
          continue;

        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitVarDecl(VarDecl *VD) {
      printCommon(VD, "var_decl");
      if (VD->isStatic())
        PrintWithColorRAII(OS, DeclModifierColor) << " type";
      if (VD->isLet())
        PrintWithColorRAII(OS, DeclModifierColor) << " let";
      if (VD->hasNonPatternBindingInit())
        PrintWithColorRAII(OS, DeclModifierColor) << " non_pattern_init";
      PrintWithColorRAII(OS, DeclModifierColor)
        << " storage_kind=" << getStorageKindName(VD->getStorageKind());
      if (VD->getAttrs().hasAttribute<LazyAttr>())
        PrintWithColorRAII(OS, DeclModifierColor) << " lazy";

      printAccessors(VD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printAccessors(AbstractStorageDecl *D) {
      if (FuncDecl *Get = D->getGetter()) {
        OS << "\n";
        printRec(Get);
      }
      if (FuncDecl *Set = D->getSetter()) {
        OS << "\n";
        printRec(Set);
      }
      if (FuncDecl *MaterializeForSet = D->getMaterializeForSetFunc()) {
        OS << "\n";
        printRec(MaterializeForSet);
      }
      if (D->hasObservers()) {
        if (FuncDecl *WillSet = D->getWillSetFunc()) {
          OS << "\n";
          printRec(WillSet);
        }
        if (FuncDecl *DidSet = D->getDidSetFunc()) {
          OS << "\n";
          printRec(DidSet);
        }
      }
      if (D->hasAddressors()) {
        if (FuncDecl *addressor = D->getAddressor()) {
          OS << "\n";
          printRec(addressor);
        }
        if (FuncDecl *mutableAddressor = D->getMutableAddressor()) {
          OS << "\n";
          printRec(mutableAddressor);
        }
      }
    }

    void visitParamDecl(ParamDecl *PD) {
      printParameter(PD);
    }

    void visitEnumCaseDecl(EnumCaseDecl *ECD) {
      printCommon(ECD, "enum_case_decl");
      for (EnumElementDecl *D : ECD->getElements()) {
        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitEnumDecl(EnumDecl *ED) {
      printCommon(ED, "enum_decl");
      printInherited(ED->getInherited());
      for (Decl *D : ED->getMembers()) {
        OS << '\n';
        printRec(D);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitEnumElementDecl(EnumElementDecl *EED) {
      printCommon(EED, "enum_element_decl");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitStructDecl(StructDecl *SD) {
      printCommon(SD, "struct_decl");
      printInherited(SD->getInherited());
      for (Decl *D : SD->getMembers()) {
        OS << '\n';
        printRec(D);
      }
      OS << ")";
    }

    void visitClassDecl(ClassDecl *CD) {
      printCommon(CD, "class_decl");
      printInherited(CD->getInherited());
      for (Decl *D : CD->getMembers()) {
        OS << '\n';
        printRec(D);
      }
      OS << ")";
    }

    void visitPatternBindingDecl(PatternBindingDecl *PBD) {
      printCommon(PBD, "pattern_binding_decl");

      for (auto entry : PBD->getPatternList()) {
        OS << '\n';
        printRec(entry.getPattern());
        if (entry.getInit()) {
          OS << '\n';
          printRec(entry.getInit());
        }
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitSubscriptDecl(SubscriptDecl *SD) {
      printCommon(SD, "subscript_decl");
      OS << " storage_kind=" << getStorageKindName(SD->getStorageKind());
      printAccessors(SD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printCommonAFD(AbstractFunctionDecl *D, const char *Type) {
      printCommon(D, Type, FuncColor);
      if (!D->getCaptureInfo().isTrivial()) {
        OS << " ";
        D->getCaptureInfo().print(OS);
      }

      if (auto fec = D->getForeignErrorConvention()) {
        OS << " foreign_error=";
        bool wantResultType = false;
        switch (fec->getKind()) {
        case ForeignErrorConvention::ZeroResult:
          OS << "ZeroResult";
          wantResultType = true;
          break;

        case ForeignErrorConvention::NonZeroResult:
          OS << "NonZeroResult";
          wantResultType = true;
          break;

        case ForeignErrorConvention::ZeroPreservedResult:
          OS << "ZeroPreservedResult";
          break;

        case ForeignErrorConvention::NilResult:
          OS << "NilResult";
          break;

        case ForeignErrorConvention::NonNilError:
          OS << "NonNilError";
          break;
        }

        OS << ((fec->isErrorOwned() == ForeignErrorConvention::IsOwned)
                ? ",owned"
                : ",unowned");
        OS << ",param=" << llvm::utostr(fec->getErrorParameterIndex());
        OS << ",paramtype=" << fec->getErrorParameterType().getString();
        if (wantResultType)
          OS << ",resulttype=" << fec->getResultType().getString();
      }
    }

    void printParameter(const ParamDecl *P) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, ParameterColor) << "parameter ";
      printDeclName(P);
      if (!P->getArgumentName().empty())
        PrintWithColorRAII(OS, IdentifierColor)
          << " apiName=" << P->getArgumentName();

      if (P->hasType()) {
        PrintWithColorRAII(OS, TypeColor) << " type='";
        P->getType().print(PrintWithColorRAII(OS, TypeColor).getOS());
        PrintWithColorRAII(OS, TypeColor) << "'";
      }

      if (P->hasInterfaceType()) {
        PrintWithColorRAII(OS, InterfaceTypeColor) << " interface type='";
        P->getInterfaceType().print(
            PrintWithColorRAII(OS, InterfaceTypeColor).getOS());
        PrintWithColorRAII(OS, InterfaceTypeColor) << "'";
      }

      if (!P->isLet())
        OS << " mutable";

      if (P->isVariadic())
        OS << " variadic";

      switch (P->getDefaultArgumentKind()) {
      case DefaultArgumentKind::None: break;
      case DefaultArgumentKind::Column:
        printField("default_arg", "#column");
        break;
      case DefaultArgumentKind::DSOHandle:
        printField("default_arg", "#dsohandle");
        break;
      case DefaultArgumentKind::File:
        printField("default_arg", "#file");
        break;
      case DefaultArgumentKind::Function:
        printField("default_arg", "#function");
        break;
      case DefaultArgumentKind::Inherited:
        printField("default_arg", "inherited");
        break;
      case DefaultArgumentKind::Line:
        printField("default_arg", "#line");
        break;
      case DefaultArgumentKind::Nil:
        printField("default_arg", "nil");
        break;
      case DefaultArgumentKind::EmptyArray:
        printField("default_arg", "[]");
        break;
      case DefaultArgumentKind::EmptyDictionary:
        printField("default_arg", "[:]");
        break;
      case DefaultArgumentKind::Normal:
        printField("default_arg", "normal");
        break;
      }

      if (auto init = P->getDefaultValue()) {
        OS << " expression=\n";
        printRec(init);
      }

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void printParameterList(const ParameterList *params) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, ParameterColor) << "parameter_list";
      Indent += 2;
      for (auto P : *params) {
        OS << '\n';
        printParameter(P);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
      Indent -= 2;
    }

    void printAbstractFunctionDecl(AbstractFunctionDecl *D) {
      for (auto pl : D->getParameterLists()) {
        OS << '\n';
        Indent += 2;
        printParameterList(pl);
        Indent -= 2;
     }
      if (auto FD = dyn_cast<FuncDecl>(D)) {
        if (FD->getBodyResultTypeLoc().getTypeRepr()) {
          OS << '\n';
          Indent += 2;
          OS.indent(Indent);
          PrintWithColorRAII(OS, ParenthesisColor) << '(';
          OS << "result\n";
          printRec(FD->getBodyResultTypeLoc().getTypeRepr());
          PrintWithColorRAII(OS, ParenthesisColor) << ')';
          Indent -= 2;
        }
      }
      if (auto Body = D->getBody(/*canSynthesize=*/false)) {
        OS << '\n';
        printRec(Body);
      }
     }

    void visitFuncDecl(FuncDecl *FD) {
      printCommonAFD(FD, "func_decl");
      if (FD->isStatic())
        OS << " type";
      if (auto *ASD = FD->getAccessorStorageDecl()) {
        switch (FD->getAccessorKind()) {
        case AccessorKind::NotAccessor: llvm_unreachable("Isn't an accessor?");
        case AccessorKind::IsGetter: OS << " getter"; break;
        case AccessorKind::IsSetter: OS << " setter"; break;
        case AccessorKind::IsWillSet: OS << " willset"; break;
        case AccessorKind::IsDidSet: OS << " didset"; break;
        case AccessorKind::IsMaterializeForSet: OS << " materializeForSet"; break;
        case AccessorKind::IsAddressor: OS << " addressor"; break;
        case AccessorKind::IsMutableAddressor: OS << " mutableAddressor"; break;
        }

        OS << "_for=" << ASD->getFullName();
      }

      for (auto VD: FD->getSatisfiedProtocolRequirements()) {
        OS << '\n';
        OS.indent(Indent+2);
        PrintWithColorRAII(OS, ParenthesisColor) << '(';
        OS << "conformance ";
        VD->dumpRef(OS);
        PrintWithColorRAII(OS, ParenthesisColor) << ')';
      }

      printAbstractFunctionDecl(FD);

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
     }

    void visitConstructorDecl(ConstructorDecl *CD) {
      printCommonAFD(CD, "constructor_decl");
      if (CD->isRequired())
        PrintWithColorRAII(OS, DeclModifierColor) << " required";
      switch (CD->getInitKind()) {
      case CtorInitializerKind::Designated:
        PrintWithColorRAII(OS, DeclModifierColor) << " designated";
        break;

      case CtorInitializerKind::Convenience:
        PrintWithColorRAII(OS, DeclModifierColor) << " convenience";
        break;

      case CtorInitializerKind::ConvenienceFactory:
        PrintWithColorRAII(OS, DeclModifierColor) << " convenience_factory";
        break;

      case CtorInitializerKind::Factory:
        PrintWithColorRAII(OS, DeclModifierColor) << " factory";
        break;
      }

      switch (CD->getFailability()) {
      case OTK_None:
        break;

      case OTK_Optional:
        PrintWithColorRAII(OS, DeclModifierColor) << " failable=Optional";
        break;

      case OTK_ImplicitlyUnwrappedOptional:
        PrintWithColorRAII(OS, DeclModifierColor)
          << " failable=ImplicitlyUnwrappedOptional";
        break;
      }

      printAbstractFunctionDecl(CD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitDestructorDecl(DestructorDecl *DD) {
      printCommonAFD(DD, "destructor_decl");
      printAbstractFunctionDecl(DD);
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
      printCommon(TLCD, "top_level_code_decl");
      if (TLCD->getBody()) {
        OS << "\n";
        printRec(TLCD->getBody());
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitIfConfigDecl(IfConfigDecl *ICD) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      OS << "#if_decl\n";
      Indent += 2;
      for (auto &Clause : ICD->getClauses()) {
        if (Clause.Cond) {
          PrintWithColorRAII(OS, ParenthesisColor) << '(';
          OS << "#if:\n";
          printRec(Clause.Cond);
        } else {
          OS << '\n';
          PrintWithColorRAII(OS, ParenthesisColor) << '(';
          OS << "#else:\n";
        }

        for (auto D : Clause.Members) {
          OS << '\n';
          printRec(D);
        }

        PrintWithColorRAII(OS, ParenthesisColor) << ')';
      }

      Indent -= 2;
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPrecedenceGroupDecl(PrecedenceGroupDecl *PGD) {
      printCommon(PGD, "precedence_group_decl ");
      OS << PGD->getName() << "\n";

      OS.indent(Indent+2);
      OS << "associativity ";
      switch (PGD->getAssociativity()) {
      case Associativity::None: OS << "none\n"; break;
      case Associativity::Left: OS << "left\n"; break;
      case Associativity::Right: OS << "right\n"; break;
      }

      OS.indent(Indent+2);
      OS << "assignment " << (PGD->isAssignment() ? "true" : "false");

      auto printRelations =
          [&](StringRef label, ArrayRef<PrecedenceGroupDecl::Relation> rels) {
        if (rels.empty()) return;
        OS << '\n';
        OS.indent(Indent+2);
        OS << label << ' ' << rels[0].Name;
        for (auto &rel : rels.slice(1))
          OS << ", " << rel.Name;
      };
      printRelations("higherThan", PGD->getHigherThan());
      printRelations("lowerThan", PGD->getLowerThan());

      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitInfixOperatorDecl(InfixOperatorDecl *IOD) {
      printCommon(IOD, "infix_operator_decl ");
      OS << IOD->getName() << "\n";
      OS.indent(Indent+2);
      OS << "precedence " << IOD->getPrecedenceGroupName();
      if (!IOD->getPrecedenceGroup()) OS << " <null>";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPrefixOperatorDecl(PrefixOperatorDecl *POD) {
      printCommon(POD, "prefix_operator_decl ");
      OS << POD->getName();
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitPostfixOperatorDecl(PostfixOperatorDecl *POD) {
      printCommon(POD, "postfix_operator_decl ");
      OS << POD->getName();
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }

    void visitModuleDecl(ModuleDecl *MD) {
      printCommon(MD, "module");
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
  };
} // end anonymous namespace

void ParameterList::dump() const {
  dump(llvm::errs(), 0);
}

void ParameterList::dump(raw_ostream &OS, unsigned Indent) const {
  llvm::Optional<llvm::SaveAndRestore<bool>> X;
  
  // Make sure to print type variables if we can get to ASTContext.
  if (size() != 0 && get(0)) {
    auto &ctx = get(0)->getASTContext();
    X.emplace(llvm::SaveAndRestore<bool>(ctx.LangOpts.DebugConstraintSolver,
                                         true));
  }
  
  PrintDecl(OS, Indent).printParameterList(this);
  llvm::errs() << '\n';
}



void Decl::dump() const {
  dump(llvm::errs(), 0);
}

void Decl::dump(raw_ostream &OS, unsigned Indent) const {
  // Make sure to print type variables.
  llvm::SaveAndRestore<bool> X(getASTContext().LangOpts.DebugConstraintSolver,
                               true);
  PrintDecl(OS, Indent).visit(const_cast<Decl *>(this));
  OS << '\n';
}

/// Print the given declaration context (with its parents).
void swift::printContext(raw_ostream &os, DeclContext *dc) {
  if (auto parent = dc->getParent()) {
    printContext(os, parent);
    os << '.';
  }

  switch (dc->getContextKind()) {
  case DeclContextKind::Module:
    printName(os, cast<ModuleDecl>(dc)->getName());
    break;

  case DeclContextKind::FileUnit:
    // FIXME: print the file's basename?
    os << "(file)";
    break;

  case DeclContextKind::SerializedLocal:
    os << "local context";
    break;

  case DeclContextKind::AbstractClosureExpr: {
    auto *ACE = cast<AbstractClosureExpr>(dc);
    if (isa<ClosureExpr>(ACE)) {
      PrintWithColorRAII(os, DiscriminatorColor)
        << "explicit closure discriminator=";
    }
    if (isa<AutoClosureExpr>(ACE)) {
      PrintWithColorRAII(os, DiscriminatorColor)
        << "autoclosure discriminator=";
    }
    PrintWithColorRAII(os, DiscriminatorColor) << ACE->getDiscriminator();
    break;
  }

  case DeclContextKind::GenericTypeDecl:
    printName(os, cast<GenericTypeDecl>(dc)->getName());
    break;

  case DeclContextKind::ExtensionDecl:
    if (auto extendedTy = cast<ExtensionDecl>(dc)->getExtendedType()) {
      if (auto nominal = extendedTy->getAnyNominal()) {
        printName(os, nominal->getName());
        break;
      }
    }
    os << "extension";
    break;

  case DeclContextKind::Initializer:
    switch (cast<Initializer>(dc)->getInitializerKind()) {
    case InitializerKind::PatternBinding:
      os << "pattern binding initializer";
      break;
    case InitializerKind::DefaultArgument:
      os << "default argument initializer";
      break;
    }
    break;

  case DeclContextKind::TopLevelCodeDecl:
    os << "top-level code";
    break;

  case DeclContextKind::AbstractFunctionDecl: {
    auto *AFD = cast<AbstractFunctionDecl>(dc);
    if (isa<FuncDecl>(AFD))
      os << "func decl";
    if (isa<ConstructorDecl>(AFD))
      os << "init";
    if (isa<DestructorDecl>(AFD))
      os << "deinit";
    break;
  }
  case DeclContextKind::SubscriptDecl:
    os << "subscript decl";
    break;
  }
}

std::string ValueDecl::printRef() const {
  std::string result;
  llvm::raw_string_ostream os(result);
  dumpRef(os);
  return os.str();
}

void ValueDecl::dumpRef(raw_ostream &os) const {
  // Print the context.
  printContext(os, getDeclContext());
  os << ".";

  // Print name.
  getFullName().printPretty(os);

  // Print location.
  auto &srcMgr = getASTContext().SourceMgr;
  if (getLoc().isValid()) {
    os << '@';
    getLoc().print(os, srcMgr);
  }
}

void LLVM_ATTRIBUTE_USED ValueDecl::dumpRef() const {
  dumpRef(llvm::errs());
}

void SourceFile::dump() const {
  dump(llvm::errs());
}

void SourceFile::dump(llvm::raw_ostream &OS) const {
  llvm::SaveAndRestore<bool> X(getASTContext().LangOpts.DebugConstraintSolver,
                               true);
  PrintDecl(OS).visitSourceFile(*this);
  llvm::errs() << '\n';
}

void Pattern::dump() const {
  PrintPattern(llvm::errs()).visit(const_cast<Pattern*>(this));
  llvm::errs() << '\n';
}

//===----------------------------------------------------------------------===//
// Printing for Stmt and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintStmt - Visitor implementation of Expr::print.
class PrintStmt : public StmtVisitor<PrintStmt> {
public:
  raw_ostream &OS;
  unsigned Indent;

  PrintStmt(raw_ostream &os, unsigned indent) : OS(os), Indent(indent) {
  }

  void printRec(Stmt *S) {
    Indent += 2;
    if (S)
      visit(S);
    else
      OS.indent(Indent) << "(**NULL STATEMENT**)";
    Indent -= 2;
  }

  void printRec(Decl *D) { D->dump(OS, Indent + 2); }
  void printRec(Expr *E) { E->print(OS, Indent + 2); }
  void printRec(const Pattern *P) {
    PrintPattern(OS, Indent+2).visit(const_cast<Pattern *>(P));
  }

  void printRec(StmtConditionElement C) {
    switch (C.getKind()) {
    case StmtConditionElement::CK_Boolean:
      return printRec(C.getBoolean());
    case StmtConditionElement::CK_PatternBinding:
      Indent += 2;
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, PatternColor) << "pattern\n";

      printRec(C.getPattern());
      OS << "\n";
      printRec(C.getInitializer());
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
      Indent -= 2;
      break;
    case StmtConditionElement::CK_Availability:
      Indent += 2;
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      OS << "#available\n";
      for (auto *Query : C.getAvailability()->getQueries()) {
        OS << '\n';
        switch (Query->getKind()) {
        case AvailabilitySpecKind::PlatformVersionConstraint:
          cast<PlatformVersionConstraintAvailabilitySpec>(Query)->print(OS, Indent + 2);
          break;
        case AvailabilitySpecKind::LanguageVersionConstraint:
          cast<LanguageVersionConstraintAvailabilitySpec>(Query)->print(OS, Indent + 2);
          break;
        case AvailabilitySpecKind::OtherPlatform:
          cast<OtherPlatformAvailabilitySpec>(Query)->print(OS, Indent + 2);
          break;
        }
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ")";
      Indent -= 2;
      break;
    }
  }

  void visitBraceStmt(BraceStmt *S) {
    printASTNodes(S->getElements(), "brace_stmt");
  }

  void printASTNodes(const ArrayRef<ASTNode> &Elements, StringRef Name) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << "(";
    PrintWithColorRAII(OS, ASTNodeColor) << Name;
    for (auto Elt : Elements) {
      OS << '\n';
      if (Expr *SubExpr = Elt.dyn_cast<Expr*>())
        printRec(SubExpr);
      else if (Stmt *SubStmt = Elt.dyn_cast<Stmt*>())
        printRec(SubStmt);
      else
        printRec(Elt.get<Decl*>());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitReturnStmt(ReturnStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "return_stmt";
    if (S->hasResult()) {
      OS << '\n';
      printRec(S->getResult());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDeferStmt(DeferStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "defer_stmt\n";
    printRec(S->getTempDecl());
    OS << '\n';
    printRec(S->getCallExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitIfStmt(IfStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "if_stmt\n";
    for (auto elt : S->getCond())
      printRec(elt);
    OS << '\n';
    printRec(S->getThenStmt());
    if (S->getElseStmt()) {
      OS << '\n';
      printRec(S->getElseStmt());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitGuardStmt(GuardStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "guard_stmt\n";
    for (auto elt : S->getCond())
      printRec(elt);
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitIfConfigStmt(IfConfigStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "#if_stmt\n";
    Indent += 2;
    for (auto &Clause : S->getClauses()) {
      OS.indent(Indent);
      if (Clause.Cond) {
        PrintWithColorRAII(OS, ParenthesisColor) << '(';
        PrintWithColorRAII(OS, StmtColor) << "#if:\n";
        printRec(Clause.Cond);
      } else {
        PrintWithColorRAII(OS, StmtColor) << "#else";
      }

      OS << '\n';
      Indent += 2;
      printASTNodes(Clause.Elements, "elements");
      Indent -= 2;
    }

    Indent -= 2;
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDoStmt(DoStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "do_stmt\n";
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitWhileStmt(WhileStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "while_stmt\n";
    for (auto elt : S->getCond())
      printRec(elt);
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitRepeatWhileStmt(RepeatWhileStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "do_while_stmt\n";
    printRec(S->getBody());
    OS << '\n';
    printRec(S->getCond());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForStmt(ForStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "for_stmt\n";
    if (!S->getInitializerVarDecls().empty()) {
      for (auto D : S->getInitializerVarDecls()) {
        printRec(D);
        OS << '\n';
      }
    } else if (auto *Initializer = S->getInitializer().getPtrOrNull()) {
      printRec(Initializer);
      OS << '\n';
    } else {
      OS.indent(Indent+2) << "<null initializer>\n";
    }

    if (auto *Cond = S->getCond().getPtrOrNull())
      printRec(Cond);
    else
      OS.indent(Indent+2) << "<null condition>";
    OS << '\n';

    if (auto *Increment = S->getIncrement().getPtrOrNull()) {
      printRec(Increment);
    } else {
      OS.indent(Indent+2) << "<null increment>";
    }
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForEachStmt(ForEachStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    OS << "for_each_stmt\n";
    printRec(S->getPattern());
    OS << '\n';
    if (S->getWhere()) {
      Indent += 2;
      OS.indent(Indent) << "(where\n";
      printRec(S->getWhere());
      OS << ")\n";
      Indent -= 2;
    }
    printRec(S->getPattern());
    OS << '\n';
    printRec(S->getSequence());
    OS << '\n';
    if (S->getIterator()) {
      printRec(S->getIterator());
      OS << '\n';
    }
    if (S->getIteratorNext()) {
      printRec(S->getIteratorNext());
      OS << '\n';
    }
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBreakStmt(BreakStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "break_stmt";
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitContinueStmt(ContinueStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "continue_stmt";
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFallthroughStmt(FallthroughStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "fallthrough_stmt";
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitSwitchStmt(SwitchStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "switch_stmt\n";
    printRec(S->getSubjectExpr());
    for (CaseStmt *C : S->getCases()) {
      OS << '\n';
      printRec(C);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCaseStmt(CaseStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "case_stmt";
    for (const auto &LabelItem : S->getCaseLabelItems()) {
      OS << '\n';
      OS.indent(Indent + 2);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      PrintWithColorRAII(OS, StmtColor) << "case_label_item";
      if (auto *CasePattern = LabelItem.getPattern()) {
        OS << '\n';
        printRec(CasePattern);
      }
      if (auto *Guard = LabelItem.getGuardExpr()) {
        OS << '\n';
        Guard->print(OS, Indent+4);
      }
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
    }
    OS << '\n';
    printRec(S->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFailStmt(FailStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "fail_stmt";
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitThrowStmt(ThrowStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "throw_stmt\n";
    printRec(S->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDoCatchStmt(DoCatchStmt *S) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "do_catch_stmt\n";
    printRec(S->getBody());
    OS << '\n';
    Indent += 2;
    visitCatches(S->getCatches());
    Indent -= 2;
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCatches(ArrayRef<CatchStmt*> clauses) {
    for (auto clause : clauses) {
      visitCatchStmt(clause);
    }
  }
  void visitCatchStmt(CatchStmt *clause) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, StmtColor) << "catch\n";
    printRec(clause->getErrorPattern());
    if (auto guard = clause->getGuardExpr()) {
      OS << '\n';
      printRec(guard);
    }
    OS << '\n';
    printRec(clause->getBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
};

} // end anonymous namespace

void Stmt::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void Stmt::print(raw_ostream &OS, unsigned Indent) const {
  PrintStmt(OS, Indent).visit(const_cast<Stmt*>(this));
}

//===----------------------------------------------------------------------===//
// Printing for Expr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintExpr - Visitor implementation of Expr::print.
class PrintExpr : public ExprVisitor<PrintExpr> {
public:
  raw_ostream &OS;
  unsigned Indent;

  PrintExpr(raw_ostream &os, unsigned indent) : OS(os), Indent(indent) {
  }

  void printRec(Expr *E) {
    Indent += 2;
    if (E)
      visit(E);
    else
      OS.indent(Indent) << "(**NULL EXPRESSION**)";
    Indent -= 2;
  }

  void printRecLabeled(Expr *E, StringRef label) {
    Indent += 2;
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    OS << '\n';
    printRec(E);
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
    Indent -= 2;
  }

  /// FIXME: This should use ExprWalker to print children.

  void printRec(Decl *D) { D->dump(OS, Indent + 2); }
  void printRec(Stmt *S) { S->print(OS, Indent + 2); }
  void printRec(const Pattern *P) {
    PrintPattern(OS, Indent+2).visit(const_cast<Pattern *>(P));
  }
  void printRec(TypeRepr *T);
  void printRec(ProtocolConformanceRef conf) {
    conf.dump(OS, Indent + 2);
  }

  static const char *getAccessKindString(AccessKind kind) {
    switch (kind) {
    case AccessKind::Read: return "read";
    case AccessKind::Write: return "write";
    case AccessKind::ReadWrite: return "readwrite";
    }
    llvm_unreachable("bad access kind");
  }

  raw_ostream &printCommon(Expr *E, const char *C) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, ExprColor) << C;

    if (E->isImplicit())
      PrintWithColorRAII(OS, ExprModifierColor) << " implicit";
    PrintWithColorRAII(OS, TypeColor) << " type='" << E->getType() << '\'';

    if (E->hasLValueAccessKind()) {
      PrintWithColorRAII(OS, ExprModifierColor)
        << " accessKind=" << getAccessKindString(E->getLValueAccessKind());
    }

    // If we have a source range and an ASTContext, print the source range.
    if (auto Ty = E->getType()) {
      auto &Ctx = Ty->getASTContext();
      auto L = E->getLoc();
      if (L.isValid()) {
        PrintWithColorRAII(OS, LocationColor) << " location=";
        L.print(PrintWithColorRAII(OS, LocationColor).getOS(), Ctx.SourceMgr);
      }

      auto R = E->getSourceRange();
      if (R.isValid()) {
        PrintWithColorRAII(OS, RangeColor) << " range=";
        R.print(PrintWithColorRAII(OS, RangeColor).getOS(),
                Ctx.SourceMgr, /*PrintText=*/false);
      }
    }

    return OS;
  }

  void visitErrorExpr(ErrorExpr *E) {
    printCommon(E, "error_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCodeCompletionExpr(CodeCompletionExpr *E) {
    printCommon(E, "code_completion_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitNilLiteralExpr(NilLiteralExpr *E) {
    printCommon(E, "nil_literal_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    printCommon(E, "integer_literal_expr");
    if (E->isNegative())
      PrintWithColorRAII(OS, LiteralValueColor) << " negative";
    PrintWithColorRAII(OS, LiteralValueColor) << " value=";
    Type T = E->getType();
    if (T.isNull() || !T->is<BuiltinIntegerType>())
      PrintWithColorRAII(OS, LiteralValueColor) << E->getDigitsText();
    else
      PrintWithColorRAII(OS, LiteralValueColor) << E->getValue();
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFloatLiteralExpr(FloatLiteralExpr *E) {
    printCommon(E, "float_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor)
      << " value=" << E->getDigitsText();
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitBooleanLiteralExpr(BooleanLiteralExpr *E) {
    printCommon(E, "boolean_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor)
      << " value=" << (E->getValue() ? "true" : "false");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void printStringEncoding(StringLiteralExpr::Encoding encoding) {
    switch (encoding) {
    case StringLiteralExpr::UTF8:
      PrintWithColorRAII(OS, LiteralValueColor) << "utf8";
      break;
    case StringLiteralExpr::UTF16:
      PrintWithColorRAII(OS, LiteralValueColor) << "utf16";
      break;
    case StringLiteralExpr::OneUnicodeScalar:
      PrintWithColorRAII(OS, LiteralValueColor) << "unicodeScalar";
      break;
    }
  }

  void visitStringLiteralExpr(StringLiteralExpr *E) {
    printCommon(E, "string_literal_expr");
    PrintWithColorRAII(OS, LiteralValueColor) << " encoding=";
    printStringEncoding(E->getEncoding());
    PrintWithColorRAII(OS, LiteralValueColor)
      << " value=" << QuotedString(E->getValue())
      << " builtin_initializer=";
    E->getBuiltinInitializer().dump(
      PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, LiteralValueColor) << " initializer=";
    E->getInitializer().dump(PrintWithColorRAII(OS, LiteralValueColor).getOS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *E) {
    printCommon(E, "interpolated_string_literal_expr");
    for (auto Segment : E->getSegments()) {
      OS << '\n';
      printRec(Segment);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *E) {
    printCommon(E, "magic_identifier_literal_expr") << " kind=";
    switch (E->getKind()) {
    case MagicIdentifierLiteralExpr::File:
      OS << "#file encoding=";
      printStringEncoding(E->getStringEncoding());
      break;

    case MagicIdentifierLiteralExpr::Function:
      OS << "#function encoding=";
      printStringEncoding(E->getStringEncoding());
      break;
        
    case MagicIdentifierLiteralExpr::Line:  OS << "#line"; break;
    case MagicIdentifierLiteralExpr::Column:  OS << "#column"; break;
    case MagicIdentifierLiteralExpr::DSOHandle:  OS << "#dsohandle"; break;
    }

    if (E->isString()) {
      OS << " builtin_initializer=";
      E->getBuiltinInitializer().dump(OS);
      OS << " initializer=";
      E->getInitializer().dump(OS);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitObjectLiteralExpr(ObjectLiteralExpr *E) {
    printCommon(E, "object_literal") 
      << " kind='" << E->getLiteralKindPlainName() << "'";
    printArgumentLabels(E->getArgumentLabels());
    OS << "\n";
    printRec(E->getArg());
  }

  void visitDiscardAssignmentExpr(DiscardAssignmentExpr *E) {
    printCommon(E, "discard_assignment_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  
  void visitDeclRefExpr(DeclRefExpr *E) {
    printCommon(E, "declref_expr");
    PrintWithColorRAII(OS, DeclColor) << " decl=";
    E->getDeclRef().dump(PrintWithColorRAII(OS, DeclColor).getOS());
    PrintWithColorRAII(OS, AccessibilityColor) << E->getAccessSemantics();
    PrintWithColorRAII(OS, ExprModifierColor)
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind())
      << " specialized=" << (E->isSpecialized()? "yes" : "no");

    for (auto TR : E->getGenericArgs()) {
      OS << '\n';
      printRec(TR);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitSuperRefExpr(SuperRefExpr *E) {
    printCommon(E, "super_ref_expr");
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTypeExpr(TypeExpr *E) {
    printCommon(E, "type_expr");
    PrintWithColorRAII(OS, TypeReprColor) << " typerepr='";
    if (E->getTypeRepr())
      E->getTypeRepr()->print(PrintWithColorRAII(OS, TypeReprColor).getOS());
    else
      PrintWithColorRAII(OS, TypeReprColor) << "<<NULL>>";
    PrintWithColorRAII(OS, TypeReprColor) << "'";
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *E) {
    printCommon(E, "other_constructor_ref_expr")
      << " decl=";
    E->getDeclRef().dump(OS);
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *E) {
    printCommon(E, "overloaded_decl_ref_expr")
      << " name=" << E->getDecls()[0]->getName()
      << " #decls=" << E->getDecls().size()
      << " specialized=" << (E->isSpecialized()? "yes" : "no")
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());

    for (ValueDecl *D : E->getDecls()) {
      OS << '\n';
      OS.indent(Indent);
      D->dumpRef(OS);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *E) {
    printCommon(E, "unresolved_decl_ref_expr");
    PrintWithColorRAII(OS, IdentifierColor) << " name=" << E->getName();
    PrintWithColorRAII(OS, ExprModifierColor)
      << " specialized=" << (E->isSpecialized()? "yes" : "no")
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *E) {
    printCommon(E, "unresolved_specialize_expr") << '\n';
    printRec(E->getSubExpr());
    for (TypeLoc T : E->getUnresolvedParams()) {
      OS << '\n';
      printRec(T.getTypeRepr());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitMemberRefExpr(MemberRefExpr *E) {
    printCommon(E, "member_ref_expr")
      << " decl=";
    E->getMember().dump(OS);

    PrintWithColorRAII(OS, AccessibilityColor) << E->getAccessSemantics();
    if (E->isSuper())
      OS << " super";

    OS << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDynamicMemberRefExpr(DynamicMemberRefExpr *E) {
    printCommon(E, "dynamic_member_ref_expr")
      << " decl=";
    E->getMember().dump(OS);
    OS << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedMemberExpr(UnresolvedMemberExpr *E) {
    printCommon(E, "unresolved_member_expr")
      << " name='" << E->getName() << "'";
    printArgumentLabels(E->getArgumentLabels());
    if (E->getArgument()) {
      OS << '\n';
      printRec(E->getArgument());
    }
    OS << "')";
  }
  void visitDotSelfExpr(DotSelfExpr *E) {
    printCommon(E, "dot_self_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitParenExpr(ParenExpr *E) {
    printCommon(E, "paren_expr");
    if (E->hasTrailingClosure())
      OS << " trailing-closure";
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitTupleExpr(TupleExpr *E) {
    printCommon(E, "tuple_expr");
    if (E->hasTrailingClosure())
      OS << " trailing-closure";

    if (E->hasElementNames()) {
      PrintWithColorRAII(OS, IdentifierColor) << " names=";

      interleave(E->getElementNames(),
                 [&](Identifier name) {
                   PrintWithColorRAII(OS, IdentifierColor)
                     << (name.empty()?"''":name.str());
                 },
                 [&] { PrintWithColorRAII(OS, IdentifierColor) << ","; });
    }

    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      if (E->getElement(i))
        printRec(E->getElement(i));
      else
        OS.indent(Indent+2) << "<<tuple element default value>>";
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitArrayExpr(ArrayExpr *E) {
    printCommon(E, "array_expr");
    for (auto elt : E->getElements()) {
      OS << '\n';
      printRec(elt);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDictionaryExpr(DictionaryExpr *E) {
    printCommon(E, "dictionary_expr");
    for (auto elt : E->getElements()) {
      OS << '\n';
      printRec(elt);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitSubscriptExpr(SubscriptExpr *E) {
    printCommon(E, "subscript_expr");
    PrintWithColorRAII(OS, AccessibilityColor) << E->getAccessSemantics();
    if (E->isSuper())
      OS << " super";
    if (E->hasDecl()) {
      OS << "  decl=";
      E->getDecl().dump(OS);
    }
    printArgumentLabels(E->getArgumentLabels());
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDynamicSubscriptExpr(DynamicSubscriptExpr *E) {
    printCommon(E, "dynamic_subscript_expr")
      << " decl=";
    E->getMember().dump(OS);
    printArgumentLabels(E->getArgumentLabels());
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedDotExpr(UnresolvedDotExpr *E) {
    printCommon(E, "unresolved_dot_expr")
      << " field '" << E->getName() << "'"
      << " function_ref=" << getFunctionRefKindStr(E->getFunctionRefKind());
    if (E->getBase()) {
      OS << '\n';
      printRec(E->getBase());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitTupleElementExpr(TupleElementExpr *E) {
    printCommon(E, "tuple_element_expr")
      << " field #" << E->getFieldNumber() << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitTupleShuffleExpr(TupleShuffleExpr *E) {
    printCommon(E, "tuple_shuffle_expr");
    if (E->isSourceScalar()) OS << " source_is_scalar";
    OS << " elements=[";
    for (unsigned i = 0, e = E->getElementMapping().size(); i != e; ++i) {
      if (i) OS << ", ";
      OS << E->getElementMapping()[i];
    }
    OS << "]";
    OS << " variadic_sources=[";
    interleave(E->getVariadicArgs(),
               [&](unsigned source) {
                 OS << source;
               },
               [&] { OS << ", "; });
    OS << "]";

    if (auto defaultArgsOwner = E->getDefaultArgsOwner()) {
      OS << " default_args_owner=";
      defaultArgsOwner.dump(OS);
      dump(defaultArgsOwner.getSubstitutions());
    }

    OS << "\n";
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnresolvedTypeConversionExpr(UnresolvedTypeConversionExpr *E) {
    printCommon(E, "unresolvedtype_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitFunctionConversionExpr(FunctionConversionExpr *E) {
    printCommon(E, "function_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCovariantFunctionConversionExpr(CovariantFunctionConversionExpr *E){
    printCommon(E, "covariant_function_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCovariantReturnConversionExpr(CovariantReturnConversionExpr *E){
    printCommon(E, "covariant_return_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitErasureExpr(ErasureExpr *E) {
    printCommon(E, "erasure_expr") << '\n';
    for (auto conf : E->getConformances()) {
      printRec(conf);
      OS << '\n';
    }
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAnyHashableErasureExpr(AnyHashableErasureExpr *E) {
    printCommon(E, "any_hashable_erasure_expr") << '\n';
    printRec(E->getConformance());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitLoadExpr(LoadExpr *E) {
    printCommon(E, "load_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitMetatypeConversionExpr(MetatypeConversionExpr *E) {
    printCommon(E, "metatype_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitCollectionUpcastConversionExpr(CollectionUpcastConversionExpr *E) {
    printCommon(E, "collection_upcast_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    if (auto keyConversion = E->getKeyConversion()) {
      OS << '\n';
      printRecLabeled(keyConversion.Conversion, "key_conversion");
    }
    if (auto valueConversion = E->getValueConversion()) {
      OS << '\n';
      printRecLabeled(valueConversion.Conversion, "value_conversion");
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitDerivedToBaseExpr(DerivedToBaseExpr *E) {
    printCommon(E, "derived_to_base_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitArchetypeToSuperExpr(ArchetypeToSuperExpr *E) {
    printCommon(E, "archetype_to_super_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitLValueToPointerExpr(LValueToPointerExpr *E) {
    printCommon(E, "lvalue_to_pointer") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitInjectIntoOptionalExpr(InjectIntoOptionalExpr *E) {
    printCommon(E, "inject_into_optional") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitClassMetatypeToObjectExpr(ClassMetatypeToObjectExpr *E) {
    printCommon(E, "class_metatype_to_object") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitExistentialMetatypeToObjectExpr(ExistentialMetatypeToObjectExpr *E) {
    printCommon(E, "existential_metatype_to_object") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitProtocolMetatypeToObjectExpr(ProtocolMetatypeToObjectExpr *E) {
    printCommon(E, "protocol_metatype_to_object") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitInOutToPointerExpr(InOutToPointerExpr *E) {
    printCommon(E, "inout_to_pointer") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitArrayToPointerExpr(ArrayToPointerExpr *E) {
    printCommon(E, "array_to_pointer") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitStringToPointerExpr(StringToPointerExpr *E) {
    printCommon(E, "string_to_pointer") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitPointerToPointerExpr(PointerToPointerExpr *E) {
    printCommon(E, "pointer_to_pointer") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForeignObjectConversionExpr(ForeignObjectConversionExpr *E) {
    printCommon(E, "foreign_object_conversion") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitUnevaluatedInstanceExpr(UnevaluatedInstanceExpr *E) {
    printCommon(E, "unevaluated_instance") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitInOutExpr(InOutExpr *E) {
    printCommon(E, "inout_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitForceTryExpr(ForceTryExpr *E) {
    printCommon(E, "force_try_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOptionalTryExpr(OptionalTryExpr *E) {
    printCommon(E, "optional_try_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTryExpr(TryExpr *E) {
    printCommon(E, "try_expr");
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitSequenceExpr(SequenceExpr *E) {
    printCommon(E, "sequence_expr");
    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      printRec(E->getElement(i));
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCaptureListExpr(CaptureListExpr *E) {
    printCommon(E, "capture_list");
    for (auto capture : E->getCaptureList()) {
      OS << '\n';
      Indent += 2;
      printRec(capture.Var);
      printRec(capture.Init);
      Indent -= 2;
    }
    printRec(E->getClosureBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  llvm::raw_ostream &printClosure(AbstractClosureExpr *E, char const *name) {
    printCommon(E, name);
    PrintWithColorRAII(OS, DiscriminatorColor)
      << " discriminator=" << E->getDiscriminator();
    if (!E->getCaptureInfo().isTrivial()) {
      OS << " ";
      E->getCaptureInfo().print(PrintWithColorRAII(OS, CapturesColor).getOS());
    }

    return OS;
  }

  void visitClosureExpr(ClosureExpr *E) {
    printClosure(E, "closure_expr");
    if (E->hasSingleExpressionBody())
      PrintWithColorRAII(OS, ClosureModifierColor) << " single-expression";

    if (E->getParameters()) {
      OS << '\n';
      PrintDecl(OS, Indent+2).printParameterList(E->getParameters());
    }

    OS << '\n';
    if (E->hasSingleExpressionBody()) {
      printRec(E->getSingleExpressionBody());
    } else {
      printRec(E->getBody());
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAutoClosureExpr(AutoClosureExpr *E) {
    printClosure(E, "autoclosure_expr") << '\n';

    if (E->getParameters()) {
      OS << '\n';
      PrintDecl(OS, Indent+2).printParameterList(E->getParameters());
    }

    OS << '\n';
    printRec(E->getSingleExpressionBody());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDynamicTypeExpr(DynamicTypeExpr *E) {
    printCommon(E, "metatype_expr");
    OS << '\n';
    printRec(E->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitOpaqueValueExpr(OpaqueValueExpr *E) {
    printCommon(E, "opaque_value_expr") << " @ " << (void*)E;
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void printArgumentLabels(ArrayRef<Identifier> argLabels) {
    PrintWithColorRAII(OS, ArgumentsColor) << " arg_labels=";
    for (auto label : argLabels) {
      PrintWithColorRAII(OS, ArgumentsColor)
        << (label.empty() ? "_" : label.str()) << ":";
    }
  }

  void printApplyExpr(ApplyExpr *E, const char *NodeName) {
    printCommon(E, NodeName);
    if (E->isSuper())
      PrintWithColorRAII(OS, ExprModifierColor) << " super";
    if (E->isThrowsSet()) {
      PrintWithColorRAII(OS, ExprModifierColor)
        << (E->throws() ? " throws" : " nothrow");
    }
    if (auto call = dyn_cast<CallExpr>(E))
      printArgumentLabels(call->getArgumentLabels());

    OS << '\n';
    printRec(E->getFn());
    OS << '\n';
    printRec(E->getArg());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCallExpr(CallExpr *E) {
    printApplyExpr(E, "call_expr");
  }
  void visitPrefixUnaryExpr(PrefixUnaryExpr *E) {
    printApplyExpr(E, "prefix_unary_expr");
  }
  void visitPostfixUnaryExpr(PostfixUnaryExpr *E) {
    printApplyExpr(E, "postfix_unary_expr");
  }
  void visitBinaryExpr(BinaryExpr *E) {
    printApplyExpr(E, "binary_expr");
  }
  void visitDotSyntaxCallExpr(DotSyntaxCallExpr *E) {
    printApplyExpr(E, "dot_syntax_call_expr");
  }
  void visitConstructorRefCallExpr(ConstructorRefCallExpr *E) {
    printApplyExpr(E, "constructor_ref_call_expr");
  }
  void visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *E) {
    printCommon(E, "dot_syntax_base_ignored") << '\n';
    printRec(E->getLHS());
    OS << '\n';
    printRec(E->getRHS());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void printExplicitCastExpr(ExplicitCastExpr *E, const char *name) {
    printCommon(E, name) << ' ';
    if (auto checkedCast = dyn_cast<CheckedCastExpr>(E))
      OS << getCheckedCastKindName(checkedCast->getCastKind()) << ' ';
    OS << "writtenType='";
    E->getCastTypeLoc().getType().print(OS);
    OS << "'\n";
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForcedCheckedCastExpr(ForcedCheckedCastExpr *E) {
    printExplicitCastExpr(E, "forced_checked_cast_expr");
  }
  void visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *E) {
    printExplicitCastExpr(E, "conditional_checked_cast_expr");
  }
  void visitIsExpr(IsExpr *E) {
    printExplicitCastExpr(E, "is_subtype_expr");
  }
  void visitCoerceExpr(CoerceExpr *E) {
    printExplicitCastExpr(E, "coerce_expr");
  }
  void visitArrowExpr(ArrowExpr *E) {
    printCommon(E, "arrow") << '\n';
    printRec(E->getArgsExpr());
    OS << '\n';
    printRec(E->getResultExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *E) {
    printCommon(E, "rebind_self_in_constructor_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitIfExpr(IfExpr *E) {
    printCommon(E, "if_expr") << '\n';
    printRec(E->getCondExpr());
    OS << '\n';
    printRec(E->getThenExpr());
    OS << '\n';
    printRec(E->getElseExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitAssignExpr(AssignExpr *E) {
    OS.indent(Indent) << "(assign_expr\n";
    printRec(E->getDest());
    OS << '\n';
    printRec(E->getSrc());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitEnumIsCaseExpr(EnumIsCaseExpr *E) {
    printCommon(E, "enum_is_case_expr") << ' ' <<
      E->getEnumElement()->getName() << "\n";
    printRec(E->getSubExpr());
    
  }
  void visitUnresolvedPatternExpr(UnresolvedPatternExpr *E) {
    printCommon(E, "unresolved_pattern_expr") << '\n';
    printRec(E->getSubPattern());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitBindOptionalExpr(BindOptionalExpr *E) {
    printCommon(E, "bind_optional_expr")
      << " depth=" << E->getDepth() << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitOptionalEvaluationExpr(OptionalEvaluationExpr *E) {
    printCommon(E, "optional_evaluation_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitForceValueExpr(ForceValueExpr *E) {
    printCommon(E, "force_value_expr") << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitOpenExistentialExpr(OpenExistentialExpr *E) {
    printCommon(E, "open_existential_expr") << '\n';
    printRec(E->getOpaqueValue());
    OS << '\n';
    printRec(E->getExistentialValue());
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitMakeTemporarilyEscapableExpr(MakeTemporarilyEscapableExpr *E) {
    printCommon(E, "make_temporarily_escapable_expr") << '\n';
    printRec(E->getOpaqueValue());
    OS << '\n';
    printRec(E->getNonescapingClosureValue());
    OS << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitEditorPlaceholderExpr(EditorPlaceholderExpr *E) {
    printCommon(E, "editor_placeholder_expr") << '\n';
    auto *TyR = E->getTypeLoc().getTypeRepr();
    auto *ExpTyR = E->getTypeForExpansion();
    if (TyR)
      printRec(TyR);
    if (ExpTyR && ExpTyR != TyR) {
      OS << '\n';
      printRec(ExpTyR);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
  void visitObjCSelectorExpr(ObjCSelectorExpr *E) {
    printCommon(E, "objc_selector_expr");
    OS << " kind=";
    switch (E->getSelectorKind()) {
      case ObjCSelectorExpr::Method:
        OS << "method";
        break;
      case ObjCSelectorExpr::Getter:
        OS << "getter";
        break;
      case ObjCSelectorExpr::Setter:
        OS << "setter";
        break;
    }
    OS << " decl=";
    if (auto method = E->getMethod()) {
      method->dumpRef(OS);
    } else {
      OS << "<unresolved>";
    }
    OS << '\n';
    printRec(E->getSubExpr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitObjCKeyPathExpr(ObjCKeyPathExpr *E) {
    printCommon(E, "keypath_expr");
    for (unsigned i = 0, n = E->getNumComponents(); i != n; ++i) {
      OS << "\n";
      OS.indent(Indent + 2);
      OS << "component=";
      if (auto decl = E->getComponentDecl(i))
        decl->dumpRef(OS);
      else
        OS << E->getComponentName(i);
    }
    if (auto semanticE = E->getSemanticExpr()) {
      OS << '\n';
      printRec(semanticE);
    }
    OS << ")";
  }
};

} // end anonymous namespace


void Expr::dump(raw_ostream &OS) const {
  if (auto ty = getType()) {
    llvm::SaveAndRestore<bool> X(ty->getASTContext().LangOpts.
                                 DebugConstraintSolver, true);
  
    print(OS);
  } else {
    print(OS);
  }
  OS << '\n';
}

void Expr::dump() const {
  dump(llvm::errs());
}

void Expr::print(raw_ostream &OS, unsigned Indent) const {
  PrintExpr(OS, Indent).visit(const_cast<Expr*>(this));
}

void Expr::print(ASTPrinter &Printer, const PrintOptions &Opts) const {
  // FIXME: Fully use the ASTPrinter.
  llvm::SmallString<128> Str;
  llvm::raw_svector_ostream OS(Str);
  print(OS);
  Printer << OS.str();
}

//===----------------------------------------------------------------------===//
// Printing for TypeRepr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
class PrintTypeRepr : public TypeReprVisitor<PrintTypeRepr> {
public:
  raw_ostream &OS;
  unsigned Indent;
  bool ShowColors;

  PrintTypeRepr(raw_ostream &os, unsigned indent)
    : OS(os), Indent(indent), ShowColors(false) {
    if (&os == &llvm::errs() || &os == &llvm::outs())
      ShowColors = llvm::errs().has_colors() && llvm::outs().has_colors();
  }

  void printRec(Decl *D) { D->dump(OS, Indent + 2); }
  void printRec(Expr *E) { E->print(OS, Indent + 2); }
  void printRec(TypeRepr *T) { PrintTypeRepr(OS, Indent + 2).visit(T); }

  raw_ostream &printCommon(TypeRepr *T, const char *Name) {
    OS.indent(Indent);
    PrintWithColorRAII(OS, ParenthesisColor) << '(';
    PrintWithColorRAII(OS, TypeReprColor) << Name;
    return OS;
  }

  void visitErrorTypeRepr(ErrorTypeRepr *T) {
    printCommon(T, "type_error");
  }

  void visitAttributedTypeRepr(AttributedTypeRepr *T) {
    printCommon(T, "type_attributed") << " attrs=";
    T->printAttrs(OS);
    OS << '\n';
    printRec(T->getTypeRepr());
  }

  void visitIdentTypeRepr(IdentTypeRepr *T) {
    printCommon(T, "type_ident");
    Indent += 2;
    for (auto comp : T->getComponentRange()) {
      OS << '\n';
      printCommon(nullptr, "component");
      PrintWithColorRAII(OS, IdentifierColor)
        << " id='" << comp->getIdentifier() << '\'';
      OS << " bind=";
      if (comp->isBound())
        comp->getBoundDecl()->dumpRef(OS);
      else OS << "none";
      PrintWithColorRAII(OS, ParenthesisColor) << ')';
      if (auto GenIdT = dyn_cast<GenericIdentTypeRepr>(comp)) {
        for (auto genArg : GenIdT->getGenericArgs()) {
          OS << '\n';
          printRec(genArg);
        }
      }
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
    Indent -= 2;
  }

  void visitFunctionTypeRepr(FunctionTypeRepr *T) {
    printCommon(T, "type_function");
    OS << '\n'; printRec(T->getArgsTypeRepr());
    if (T->throws())
      OS << " throws ";
    OS << '\n'; printRec(T->getResultTypeRepr());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitArrayTypeRepr(ArrayTypeRepr *T) {
    printCommon(T, "type_array") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitDictionaryTypeRepr(DictionaryTypeRepr *T) {
    printCommon(T, "type_dictionary") << '\n';
    printRec(T->getKey());
    OS << '\n';
    printRec(T->getValue());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitTupleTypeRepr(TupleTypeRepr *T) {
    printCommon(T, "type_tuple");

    if (T->hasElementNames()) {
      OS << " names=";
      for (unsigned i = 0, end = T->getNumElements(); i != end; ++i) {
        if (i) OS << ",";
        auto name = T->getElementName(i);
        if (T->isNamedParameter(i))
          OS << (name.empty() ? "_" : "_ " + name.str());
        else
          OS << (name.empty() ? "''" : name.str());
      }
    }

    for (auto elem : T->getElements()) {
      OS << '\n';
      printRec(elem);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitCompositionTypeRepr(CompositionTypeRepr *T) {
    printCommon(T, "type_composite");
    for (auto elem : T->getTypes()) {
      OS << '\n';
      printRec(elem);
    }
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitMetatypeTypeRepr(MetatypeTypeRepr *T) {
    printCommon(T, "type_metatype") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitProtocolTypeRepr(ProtocolTypeRepr *T) {
    printCommon(T, "type_protocol") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }

  void visitInOutTypeRepr(InOutTypeRepr *T) {
    printCommon(T, "type_inout") << '\n';
    printRec(T->getBase());
    PrintWithColorRAII(OS, ParenthesisColor) << ')';
  }
};

} // end anonymous namespace

void PrintDecl::printRec(TypeRepr *T) {
  PrintTypeRepr(OS, Indent+2).visit(T);
}

void PrintExpr::printRec(TypeRepr *T) {
  PrintTypeRepr(OS, Indent+2).visit(T);
}

void PrintPattern::printRec(TypeRepr *T) {
  PrintTypeRepr(OS, Indent+2).visit(T);
}

void TypeRepr::dump() const {
  PrintTypeRepr(llvm::errs(), 0).visit(const_cast<TypeRepr*>(this));
  llvm::errs() << '\n';
}

void Substitution::dump() const {
  dump(llvm::errs());
}

void Substitution::dump(llvm::raw_ostream &out, unsigned indent) const {
  out.indent(indent);
  print(out);
  out << '\n';

  for (auto &c : Conformance) {
    c.dump(out, indent + 2);
  }
}

void ProtocolConformanceRef::dump() const {
  dump(llvm::errs());
}

void ProtocolConformanceRef::dump(llvm::raw_ostream &out,
                                  unsigned indent) const {
  if (isConcrete()) {
    getConcrete()->dump(out, indent);
  } else {
    out.indent(indent) << "(abstract_conformance protocol="
                       << getAbstract()->getName();
    PrintWithColorRAII(out, ParenthesisColor) << ')';
  }
}

void swift::dump(const ArrayRef<Substitution> &subs) {
  unsigned i = 0;
  for (const auto &s : subs) {
    llvm::errs() << i++ << ": ";
    s.dump();
  }
}

void ProtocolConformance::dump() const {
  auto &out = llvm::errs();
  dump(out);
  out << '\n';
}

void ProtocolConformance::dump(llvm::raw_ostream &out, unsigned indent) const {
  auto printCommon = [&](StringRef kind) {
    out.indent(indent);
    PrintWithColorRAII(out, ParenthesisColor) << '(';
    out << kind << "_conformance type=" << getType()
        << " protocol=" << getProtocol()->getName();
  };

  switch (getKind()) {
  case ProtocolConformanceKind::Normal:
    printCommon("normal");
    // Maybe print information about the conforming context?
    break;

  case ProtocolConformanceKind::Inherited: {
    auto conf = cast<InheritedProtocolConformance>(this);
    printCommon("inherited");
    out << '\n';
    conf->getInheritedConformance()->dump(out, indent + 2);
    break;
  }

  case ProtocolConformanceKind::Specialized: {
    auto conf = cast<SpecializedProtocolConformance>(this);
    printCommon("specialized");
    out << '\n';
    for (auto sub : conf->getGenericSubstitutions()) {
      sub.dump(out, indent + 2);
      out << '\n';
    }
    conf->getGenericConformance()->dump(out, indent + 2);
    break;
  }
  }

  PrintWithColorRAII(out, ParenthesisColor) << ')';
}

//===----------------------------------------------------------------------===//
// Dumping for Types.
//===----------------------------------------------------------------------===//

namespace {
  class PrintType : public TypeVisitor<PrintType, void, StringRef> {
    raw_ostream &OS;
    unsigned Indent;

    raw_ostream &printCommon(const TypeBase *T, StringRef label,
                             StringRef name) {
      OS.indent(Indent);
      PrintWithColorRAII(OS, ParenthesisColor) << '(';
      if (!label.empty()) {
        PrintWithColorRAII(OS, TypeFieldColor) << label;
        OS << "=";
      }

      PrintWithColorRAII(OS, TypeColor) << name;
      return OS;
    }

    // Print a single flag.
    raw_ostream &printFlag(StringRef name) {
      PrintWithColorRAII(OS, TypeFieldColor) << " " << name;
      return OS;
    }

    // Print a single flag if it is set.
    raw_ostream &printFlag(bool isSet, StringRef name) {
      if (isSet)
        printFlag(name);

      return OS;
    }

    // Print a field with a value.
    template<typename T>
    raw_ostream &printField(StringRef name, const T &value) {
      OS << " ";
      PrintWithColorRAII(OS, TypeFieldColor) << name;
      OS << "=" << value;
      return OS;
    }

    void dumpParameterFlags(ParameterTypeFlags paramFlags) {
      if (paramFlags.isVariadic())
        printFlag("vararg");
      if (paramFlags.isAutoClosure())
        printFlag("autoclosure");
      if (paramFlags.isEscaping())
        printFlag("escaping");
    }

  public:
    PrintType(raw_ostream &os, unsigned indent) : OS(os), Indent(indent) { }

    void printRec(Type type) {
      printRec("", type);
    }

    void printRec(StringRef label, Type type) {
      OS << "\n";

      if (type.isNull())
        OS << "<<null>>";
      else {
        Indent += 2;
        visit(type, label);
        Indent -=2;
      }
    }

#define TRIVIAL_TYPE_PRINTER(Class,Name)                        \
    void visit##Class##Type(Class##Type *T, StringRef label) {  \
      printCommon(T, label, #Name "_type") << ")";              \
    }

    void visitErrorType(ErrorType *T, StringRef label) {
      printCommon(T, label, "error_type");
      if (auto originalType = T->getOriginalType())
        printRec("original_type", originalType);
      OS << ")";
    }

    TRIVIAL_TYPE_PRINTER(Unresolved, unresolved)

    void visitBuiltinIntegerType(BuiltinIntegerType *T, StringRef label) {
      printCommon(T, label, "builtin_integer_type");
      if (T->isFixedWidth())
        printField("bit_width", T->getFixedWidth());
      else
        printFlag("word_sized");
      OS << ")";
    }

    void visitBuiltinFloatType(BuiltinFloatType *T, StringRef label) {
      printCommon(T, label, "builtin_float_type");
      printField("bit_width", T->getBitWidth());
      OS << ")";
    }

    TRIVIAL_TYPE_PRINTER(BuiltinRawPointer, builtin_raw_pointer)
    TRIVIAL_TYPE_PRINTER(BuiltinNativeObject, builtin_native_object)
    TRIVIAL_TYPE_PRINTER(BuiltinBridgeObject, builtin_bridge_object)
    TRIVIAL_TYPE_PRINTER(BuiltinUnknownObject, builtin_unknown_object)
    TRIVIAL_TYPE_PRINTER(BuiltinUnsafeValueBuffer, builtin_unsafe_value_buffer)

    void visitBuiltinVectorType(BuiltinVectorType *T, StringRef label) {
      printCommon(T, label, "builtin_vector_type");
      printField("num_elements", T->getNumElements());
      printRec(T->getElementType());
      OS << ")";
    }

    void visitNameAliasType(NameAliasType *T, StringRef label) {
      printCommon(T, label, "name_alias_type");
      printField("decl", T->getDecl()->printRef());
      OS << ")";
    }

    void visitParenType(ParenType *T, StringRef label) {
      printCommon(T, label, "paren_type");
      dumpParameterFlags(T->getParameterFlags());
      printRec(T->getUnderlyingType());
      OS << ")";
    }

    void visitTupleType(TupleType *T, StringRef label) {
      printCommon(T, label, "tuple_type");
      printField("num_elements", T->getNumElements());
      Indent += 2;
      for (const auto &elt : T->getElements()) {
        OS << "\n";
        OS.indent(Indent) << "(";
        PrintWithColorRAII(OS, TypeFieldColor) << "tuple_type_elt";
        if (elt.hasName())
          printField("name", elt.getName().str());
        dumpParameterFlags(elt.getParameterFlags());
        printRec(elt.getType());
        OS << ")";
      }
      Indent -= 2;
      OS << ")";
    }

    void visitUnownedStorageType(UnownedStorageType *T, StringRef label) {
      printCommon(T, label, "unowned_storage_type");
      printRec(T->getReferentType());
      OS << ")";
    }

    void visitUnmanagedStorageType(UnmanagedStorageType *T, StringRef label) {
      printCommon(T, label, "unmanaged_storage_type");
      printRec(T->getReferentType());
      OS << ")";
    }

    void visitWeakStorageType(WeakStorageType *T, StringRef label) {
      printCommon(T, label, "weak_storage_type");
      printRec(T->getReferentType());
      OS << ")";
    }

    void visitEnumType(EnumType *T, StringRef label) {
      printCommon(T, label, "enum_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      OS << ")";
    }

    void visitStructType(StructType *T, StringRef label) {
      printCommon(T, label, "struct_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      OS << ")";
    }

    void visitClassType(ClassType *T, StringRef label) {
      printCommon(T, label, "class_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      OS << ")";
    }

    void visitProtocolType(ProtocolType *T, StringRef label) {
      printCommon(T, label, "protocol_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      OS << ")";
    }

    void printMetatypeRepresentation(MetatypeRepresentation representation) {
      OS << " ";
      switch (representation) {
      case MetatypeRepresentation::Thin:
        OS << "@thin";
        break;
      case MetatypeRepresentation::Thick:
        OS << "@thick";
        break;
      case MetatypeRepresentation::ObjC:
        OS << "@objc";
        break;
      }
    }

    void visitMetatypeType(MetatypeType *T, StringRef label) {
      printCommon(T, label, "metatype_type");
      if (T->hasRepresentation())
        printMetatypeRepresentation(T->getRepresentation());
      printRec(T->getInstanceType());
      OS << ")";
    }

    void visitExistentialMetatypeType(ExistentialMetatypeType *T,
                                      StringRef label) {
      printCommon(T, label, "existential_metatype_type");
      if (T->hasRepresentation())
        printMetatypeRepresentation(T->getRepresentation());
      printRec(T->getInstanceType());
      OS << ")";
    }

    void visitModuleType(ModuleType *T, StringRef label) {
      printCommon(T, label, "module_type");
      printField("module", T->getModule()->getName());
      OS << ")";
    }

    void visitDynamicSelfType(DynamicSelfType *T, StringRef label) {
      printCommon(T, label, "dynamic_self_type");
      printRec(T->getSelfType());
      OS << ")";
    }

    void visitArchetypeType(ArchetypeType *T, StringRef label) {
      printCommon(T, label, "archetype_type");
      if (T->getOpenedExistentialType())
        printField("opened_existential_id", T->getOpenedExistentialID());
      else
        printField("name", T->getFullName());
      printField("address", static_cast<void *>(T));
      printFlag(T->requiresClass(), "class");
      for (auto proto : T->getConformsTo())
        printField("conforms_to", proto->printRef());
      if (auto parent = T->getParent())
        printField("parent", static_cast<void *>(parent));
      if (auto assocType = T->getAssocType())
        printField("assoc_type", assocType->printRef());

      // FIXME: This is ugly.
      OS << "\n";
      if (auto genericEnv = T->getGenericEnvironment()) {
        if (auto owningDC = genericEnv->getOwningDeclContext()) {
          owningDC->printContext(OS, Indent + 2);
        }
      }

      if (auto superclass = T->getSuperclass())
        printRec("superclass", superclass);
      if (auto openedExistential = T->getOpenedExistentialType())
        printRec("opened_existential", openedExistential);

      Indent += 2;
      for (auto nestedType : T->getKnownNestedTypes()) {
        OS << "\n";
        OS.indent(Indent) << "(";
        PrintWithColorRAII(OS, TypeFieldColor) << "nested_type";
        OS << "=";
        OS << nestedType.first.str() << " ";
        if (!nestedType.second) {
          PrintWithColorRAII(OS, TypeColor) << "<<unresolved>>";
        } else {
          PrintWithColorRAII(OS, TypeColor);
          OS << "=" << nestedType.second.getString();
        }
        OS << ")";
      }
      Indent -= 2;

      OS << ")";
    }

    void visitGenericTypeParamType(GenericTypeParamType *T, StringRef label) {
      printCommon(T, label, "generic_type_param_type");
      printField("depth", T->getDepth());
      printField("index", T->getIndex());
      if (auto decl = T->getDecl())
        printField("decl", decl->printRef());
      OS << ")";
    }

    void visitDependentMemberType(DependentMemberType *T, StringRef label) {
      printCommon(T, label, "dependent_member_type");
      if (auto assocType = T->getAssocType()) {
        printField("assoc_type", assocType->printRef());
      } else {
        printField("name", T->getName().str());
      }
      printRec("base", T->getBase());
      OS << ")";
    }

    void printAnyFunctionTypeCommon(AnyFunctionType *T, StringRef label,
                                    StringRef name) {
      printCommon(T, label, name);

      switch (T->getExtInfo().getSILRepresentation()) {
      case SILFunctionType::Representation::Thick:
        break;

      case SILFunctionType::Representation::Block:
        printField("representation", "block");
        break;

      case SILFunctionType::Representation::CFunctionPointer:
        printField("representation", "c");
        break;

      case SILFunctionType::Representation::Thin:
        printField("representation", "thin");
        break;

      case SILFunctionType::Representation::Method:
        printField("representation", "method");
        break;
        
      case SILFunctionType::Representation::ObjCMethod:
        printField("representation", "objc_method");
        break;
        
      case SILFunctionType::Representation::WitnessMethod:
        printField("representation", "witness_method");
        break;

      case SILFunctionType::Representation::Closure:
        printField("representation", "closure");
        break;
      }

      printFlag(T->isAutoClosure(), "autoclosure");
      printFlag(!T->isNoEscape(), "escaping");
      printFlag(T->throws(), "throws");

      printRec("input", T->getInput());
      printRec("output", T->getResult());
    }

    void visitFunctionType(FunctionType *T, StringRef label) {
      printAnyFunctionTypeCommon(T, label, "function_type");
      OS << ")";
    }

    void visitGenericFunctionType(GenericFunctionType *T, StringRef label) {
      printAnyFunctionTypeCommon(T, label, "generic_function_type");
      // FIXME: generic signature dumping needs improvement
      OS << "\n";
      OS.indent(Indent + 2) << "(";
      printField("generic_sig", T->getGenericSignature()->getAsString());
      OS << ")";
      OS << ")";
    }

    void visitSILFunctionType(SILFunctionType *T, StringRef label) {
      printCommon(T, label, "sil_function_type");
      // FIXME: Print the structure of the type.
      printField("type", T->getString());
      OS << ")";
    }

    void visitSILBlockStorageType(SILBlockStorageType *T, StringRef label) {
      printCommon(T, label, "sil_block_storage_type");
      printRec(T->getCaptureType());
      OS << ")";
    }

    void visitSILBoxType(SILBoxType *T, StringRef label) {
      printCommon(T, label, "sil_box_type");
      // FIXME: Print the structure of the type.
      printField("type", T->getString());
      OS << ")";
    }

    void visitArraySliceType(ArraySliceType *T, StringRef label) {
      printCommon(T, label, "array_slice_type");
      printRec(T->getBaseType());
      OS << ")";
    }

    void visitOptionalType(OptionalType *T, StringRef label) {
      printCommon(T, label, "optional_type");
      printRec(T->getBaseType());
      OS << ")";
    }

    void visitImplicitlyUnwrappedOptionalType(
           ImplicitlyUnwrappedOptionalType *T, StringRef label) {
      printCommon(T, label, "implicitly_unwrapped_optional_type");
      printRec(T->getBaseType());
      OS << ")";
    }

    void visitDictionaryType(DictionaryType *T, StringRef label) {
      printCommon(T, label, "dictionary_type");
      printRec("key", T->getKeyType());
      printRec("value", T->getValueType());
      OS << ")";
    }

    void visitProtocolCompositionType(ProtocolCompositionType *T,
                                      StringRef label) {
      printCommon(T, label, "protocol_composition_type");
      for (auto proto : T->getProtocols()) {
        printRec(proto);
      }
      OS << ")";
    }

    void visitLValueType(LValueType *T, StringRef label) {
      printCommon(T, label, "lvalue_type");
      printRec(T->getObjectType());
      OS << ")";
    }

    void visitInOutType(InOutType *T, StringRef label) {
      printCommon(T, label, "inout_type");
      printRec(T->getObjectType());
      OS << ")";
    }

    void visitUnboundGenericType(UnboundGenericType *T, StringRef label) {
      printCommon(T, label, "unbound_generic_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      OS << ")";
    }

    void visitBoundGenericClassType(BoundGenericClassType *T, StringRef label) {
      printCommon(T, label, "bound_generic_class_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      OS << ")";
    }

    void visitBoundGenericStructType(BoundGenericStructType *T,
                                     StringRef label) {
      printCommon(T, label, "bound_generic_struct_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      OS << ")";
    }

    void visitBoundGenericEnumType(BoundGenericEnumType *T, StringRef label) {
      printCommon(T, label, "bound_generic_enum_type");
      printField("decl", T->getDecl()->printRef());
      if (T->getParent())
        printRec("parent", T->getParent());
      for (auto arg : T->getGenericArgs())
        printRec(arg);
      OS << ")";
    }

    void visitTypeVariableType(TypeVariableType *T, StringRef label) {
      printCommon(T, label, "type_variable_type");
      printField("id", T->getID());
      OS << ")";
    }

#undef TRIVIAL_TYPE_PRINTER
  };
} // end anonymous namespace

void Type::dump() const {
  // Make sure to print type variables.
  dump(llvm::errs());
}

void Type::dump(raw_ostream &os, unsigned indent) const {
  // Make sure to print type variables.
  llvm::SaveAndRestore<bool> X(getPointer()->getASTContext().LangOpts.
                               DebugConstraintSolver, true);
  PrintType(os, indent).visit(*this, "");
  os << "\n";
}

void TypeBase::dump() const {
  // Make sure to print type variables.
  Type(const_cast<TypeBase *>(this)).dump();
}

void TypeBase::dump(raw_ostream &os, unsigned indent) const {
  auto &ctx = const_cast<TypeBase*>(this)->getASTContext();
  
  // Make sure to print type variables.
  llvm::SaveAndRestore<bool> X(ctx.LangOpts.DebugConstraintSolver, true);
  Type(const_cast<TypeBase *>(this)).dump(os, indent);
}

void GenericEnvironment::dump() const {
  llvm::errs() << "Generic environment:\n";
  for (auto gp : getGenericParams()) {
    gp->dump();
    mapTypeIntoContext(gp)->dump();
  }
  llvm::errs() << "Generic parameters:\n";
  for (auto paramTy : getGenericParams())
    paramTy->dump();
}