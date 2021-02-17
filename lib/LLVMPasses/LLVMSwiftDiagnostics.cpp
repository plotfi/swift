//===--- LLVMMergeFunctions.cpp - Merge similar functions for swift -------===//
//
// This source file is part of the Swift.org open source project
// Licensed under Apache License v2.0 with Runtime Library Exception
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "swift/LLVMPasses/Passes.h"
#include "clang/AST/StableHash.h"
#include "clang/Basic/PointerAuthOptions.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/FunctionComparator.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/IR/GlobalPtrAuthInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

using namespace llvm;
using namespace swift;

#define DEBUG_TYPE "swift-diag"

static cl::opt<bool>
    DoSwiftDiagnosticsDump("dump-swift-diag",
                           cl::desc("Dumps various names and attributes of "
                                    "Functions and Values in a Swift Module."),
                           cl::init(false), cl::Hidden);

namespace {

struct SwiftDiagnostics : public ModulePass {
  static char ID;
  SwiftDiagnostics() : ModulePass(ID) {}
  bool runOnModule(Module &M) override;
};

} // end anonymous namespace

char SwiftDiagnostics::ID = 0;
INITIALIZE_PASS_BEGIN(SwiftDiagnostics,
                      "swift-diag", "Swift Diagnostics Dumper Pass",
                      false, false)
INITIALIZE_PASS_END(SwiftDiagnostics,
                    "swift-diag", "Swift Diagnostics Dumper Pass",
                    false, false)

llvm::ModulePass *swift::createSwiftDiagnosticsPass() {
  initializeSwiftDiagnosticsPass(*llvm::PassRegistry::getPassRegistry());
  return new SwiftDiagnostics();
}

bool SwiftDiagnostics::runOnModule(Module &M) {
  if (!DoSwiftDiagnosticsDump)
    return false;

  for (const auto &F : M) {
    llvm::errs() << "Swift Diag, Function: _" << F.getName() << "\n";
  }

  return false;
}