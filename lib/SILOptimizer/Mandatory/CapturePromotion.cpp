//===--- CapturePromotion.cpp - Promotes closure captures -----------------===//
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
///
/// \file
///
/// Promotes captures from 'inout' (i.e. by-reference) to by-value
/// ==============================================================
///
/// Swift's closure model is that all local variables are capture by reference.
/// This produces a very simple programming model which is great to use, but
/// relies on the optimizer to promote by-ref captures to by-value (i.e.
/// by-copy) captures for decent performance. Consider this simple example:
///
///   func foo(a : () -> ()) {} // assume this has an unknown body
///
///   func bar() {
///     var x = 42
///
///     foo({ print(x) })
///   }
///
/// Since x is captured by-ref by the closure, x must live on the heap. By
/// looking at bar without any knowledge of foo, we can know that it is safe to
/// promote this to a by-value capture, allowing x to live on the stack under
/// the following conditions:
///
/// 1. If x is not modified in the closure body and is only loaded.
/// 2. If we can prove that all mutations to x occur before the closure is
///    formed.
///
/// Under these conditions if x is loadable then we can even load the given
/// value and pass it as a scalar instead of an address.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-capture-promotion"
#include "swift/AST/GenericEnvironment.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/TypeSubstCloner.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/SpecializationMangler.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include <tuple>

using namespace swift;

STATISTIC(NumCapturesPromoted, "Number of captures promoted");

namespace {
using IndicesSet = llvm::SmallSet<unsigned, 4>;
using PartialApplyIndicesMap = llvm::DenseMap<PartialApplyInst *, IndicesSet>;
} // anonymous namespace

//===----------------------------------------------------------------------===//
//                           Reachability Utilities
//===----------------------------------------------------------------------===//

namespace {

/// Transient reference to a block set within ReachabilityInfo.
///
/// This is a bitset that conveniently flattens into a matrix allowing bit-wise
/// operations without masking.
///
/// TODO: If this sticks around, maybe we'll make a BitMatrix ADT.
class ReachingBlockSet {
public:
  enum { BITWORD_SIZE = (unsigned)sizeof(uint64_t) * CHAR_BIT };

  constexpr static size_t numBitWordsForNumBlocks(unsigned NumBlocks) {
    return (NumBlocks + BITWORD_SIZE - 1) / BITWORD_SIZE;
  }

  /// Transient reference to a reaching block matrix.
  struct ReachingBlockMatrix {
    uint64_t *bits;
    unsigned numBitWords; // Words per row.

    ReachingBlockMatrix() : bits(nullptr), numBitWords(0) {}

    bool empty() const { return !bits; }
  };

  static ReachingBlockMatrix allocateMatrix(unsigned numBlocks) {
    ReachingBlockMatrix m;
    m.numBitWords = numBitWordsForNumBlocks(numBlocks);
    m.bits = new uint64_t[numBlocks * m.numBitWords];
    memset(m.bits, 0, numBlocks * m.numBitWords * sizeof(uint64_t));
    return m;
  }
  static void deallocateMatrix(ReachingBlockMatrix &m) {
    delete[] m.bits;
    m.bits = nullptr;
    m.numBitWords = 0;
  }
  static ReachingBlockSet allocateSet(unsigned numBlocks) {
    ReachingBlockSet s;
    s.numBitWords = numBitWordsForNumBlocks(numBlocks);
    s.bits = new uint64_t[s.numBitWords];
    return s;
  }
  static void deallocateSet(ReachingBlockSet &s) {
    delete[] s.bits;
    s.bits = nullptr;
    s.numBitWords = 0;
  }

private:
  uint64_t *bits;
  unsigned numBitWords;

public:
  ReachingBlockSet() : bits(nullptr), numBitWords(0) {}

  ReachingBlockSet(unsigned blockID, ReachingBlockMatrix &m)
      : bits(&m.bits[blockID * m.numBitWords]), numBitWords(m.numBitWords) {}

  bool test(unsigned id) const {
    assert(id / BITWORD_SIZE < numBitWords && "block ID out-of-bounds");
    unsigned int modulus = id % BITWORD_SIZE;
    long shifted = 1L << modulus;
    return bits[id / BITWORD_SIZE] & shifted;
  }

  void set(unsigned id) {
    unsigned int modulus = id % BITWORD_SIZE;
    long shifted = 1L << modulus;
    assert(id / BITWORD_SIZE < numBitWords && "block ID out-of-bounds");
    bits[id / BITWORD_SIZE] |= shifted;
  }

  ReachingBlockSet &operator|=(const ReachingBlockSet &rhs) {
    for (unsigned i : range(numBitWords))
      bits[i] |= rhs.bits[i];
    return *this;
  }

  void clear() { memset(bits, 0, numBitWords * sizeof(uint64_t)); }

  bool operator==(const ReachingBlockSet &rhs) const {
    assert(numBitWords == rhs.numBitWords && "mismatched sets");
    for (unsigned i : range(numBitWords))
      if (bits[i] != rhs.bits[i])
        return false;
    return true;
  }

  bool operator!=(const ReachingBlockSet &rhs) const { return !(*this == rhs); }

  ReachingBlockSet(const ReachingBlockSet &rhs)
      : bits(rhs.bits), numBitWords(rhs.numBitWords) {}
  const ReachingBlockSet &operator=(const ReachingBlockSet &RHS) {
    assert(numBitWords == RHS.numBitWords && "mismatched sets");
    for (unsigned i : range(numBitWords))
      bits[i] = RHS.bits[i];
    return *this;
  }
};

/// Store the reachability matrix: ToBlock -> FromBlocks.
class ReachabilityInfo {
  SILFunction *f;
  llvm::DenseMap<SILBasicBlock *, unsigned> blockMap;
  ReachingBlockSet::ReachingBlockMatrix matrix;

public:
  ReachabilityInfo(SILFunction *f) : f(f) {}
  ~ReachabilityInfo() { ReachingBlockSet::deallocateMatrix(matrix); }

  bool isComputed() const { return !matrix.empty(); }

  bool isReachable(SILBasicBlock *From, SILBasicBlock *To);

private:
  void compute();
};

} // end anonymous namespace

/// Compute ReachabilityInfo so that it can answer queries about
/// whether a given basic block in a function is reachable from another basic
/// block in the function.
///
/// FIXME: Computing global reachability requires initializing an N^2
/// bitset. This could be avoided by computing reachability on-the-fly
/// for each alloc_box by walking backward from mutations.
void ReachabilityInfo::compute() {
  assert(!isComputed() && "already computed");

  unsigned n = 0;
  for (auto &block : *f)
    blockMap.insert({&block, n++});
  matrix = ReachingBlockSet::allocateMatrix(n);
  ReachingBlockSet newSet = ReachingBlockSet::allocateSet(n);

  LLVM_DEBUG(llvm::dbgs() << "Computing Reachability for " << f->getName()
                          << " with " << n << " blocks.\n");

  // Iterate to a fix point, two times for a topological DAG.
  bool madeChange;
  do {
    madeChange = false;

    // Visit all blocks in a predictable order, hopefully close to topological.
    for (auto &block : *f) {
      ReachingBlockSet curSet(blockMap[&block], matrix);
      if (!madeChange) {
        // If we have not detected a change yet, then calculate new
        // reachabilities into a new bit vector so we can determine if any
        // change has occurred.
        newSet = curSet;
        for (auto pi = block.pred_begin(), pe = block.pred_end(); pi != pe;
             ++pi) {
          unsigned predID = blockMap[*pi];
          ReachingBlockSet predSet(predID, matrix);
          newSet |= predSet;
          newSet.set(predID);
        }
        if (newSet != curSet) {
          curSet = newSet;
          madeChange = true;
        }
      } else {
        // Otherwise, just update the existing reachabilities in-place.
        for (auto *predBlock : block.getPredecessorBlocks()) {
          unsigned predID = blockMap[predBlock];
          ReachingBlockSet predSet(predID, matrix);
          curSet |= predSet;
          curSet.set(predID);
        }
      }
      LLVM_DEBUG(llvm::dbgs()
                     << "  Block " << blockMap[&block] << " reached by ";
                 for (unsigned i
                      : range(n)) {
                   if (curSet.test(i))
                     llvm::dbgs() << i << " ";
                 } llvm::dbgs()
                 << "\n");
    }
  } while (madeChange);

  ReachingBlockSet::deallocateSet(newSet);
}

/// Return true if the To basic block is reachable from the From basic
/// block. A block is considered reachable from itself only if its entry can be
/// recursively reached from its own exit.
bool ReachabilityInfo::isReachable(SILBasicBlock *fromBlock,
                                   SILBasicBlock *toBlock) {
  if (!isComputed())
    compute();

  auto fi = blockMap.find(fromBlock), ti = blockMap.find(toBlock);
  assert(fi != blockMap.end() && ti != blockMap.end());
  ReachingBlockSet fromSet(ti->second, matrix);
  return fromSet.test(fi->second);
}

//===----------------------------------------------------------------------===//
//                               ClosureCloner
//===----------------------------------------------------------------------===//

namespace {

/// A SILCloner subclass which clones a closure function while converting
/// one or more captures from 'inout' (by-reference) to by-value.
class ClosureCloner : public SILClonerWithScopes<ClosureCloner> {
public:
  friend class SILInstructionVisitor<ClosureCloner>;
  friend class SILCloner<ClosureCloner>;

  ClosureCloner(SILOptFunctionBuilder &funcBuilder, SILFunction *orig,
                IsSerialized_t serialized, StringRef clonedName,
                IndicesSet &promotableIndices, ResilienceExpansion expansion);

  void populateCloned();

  SILFunction *getCloned() { return &getBuilder().getFunction(); }

  static SILFunction *
  constructClonedFunction(SILOptFunctionBuilder &funcBuilder,
                          PartialApplyInst *pai, FunctionRefInst *fri,
                          IndicesSet &promotableIndices,
                          ResilienceExpansion resilienceExpansion);

private:
  static SILFunction *initCloned(SILOptFunctionBuilder &funcBuilder,
                                 SILFunction *orig, IsSerialized_t serialized,
                                 StringRef clonedName,
                                 IndicesSet &promotableIndices,
                                 ResilienceExpansion expansion);

  SILValue getProjectBoxMappedVal(SILValue operandValue);

  void visitDebugValueAddrInst(DebugValueAddrInst *inst);
  void visitStrongReleaseInst(StrongReleaseInst *inst);
  void visitDestroyValueInst(DestroyValueInst *inst);
  void visitStructElementAddrInst(StructElementAddrInst *inst);
  void visitLoadInst(LoadInst *inst);
  void visitLoadBorrowInst(LoadBorrowInst *inst);
  void visitProjectBoxInst(ProjectBoxInst *inst);
  void visitBeginAccessInst(BeginAccessInst *inst);
  void visitEndAccessInst(EndAccessInst *inst);

  ResilienceExpansion resilienceExpansion;
  SILFunction *origF;
  IndicesSet &promotableIndices;
  llvm::DenseMap<SILArgument *, SILValue> boxArgumentMap;
  llvm::DenseMap<ProjectBoxInst *, SILValue> projectBoxArgumentMap;
};

} // end anonymous namespace

ClosureCloner::ClosureCloner(SILOptFunctionBuilder &funcBuilder,
                             SILFunction *orig, IsSerialized_t serialized,
                             StringRef clonedName,
                             IndicesSet &promotableIndices,
                             ResilienceExpansion resilienceExpansion)
    : SILClonerWithScopes<ClosureCloner>(
          *initCloned(funcBuilder, orig, serialized, clonedName,
                      promotableIndices, resilienceExpansion)),
      origF(orig), promotableIndices(promotableIndices) {
  assert(orig->getDebugScope()->Parent != getCloned()->getDebugScope()->Parent);
}

/// Compute the SILParameterInfo list for the new cloned closure.
///
/// Our goal as a result of this transformation is to:
///
/// 1. Let through all arguments not related to a promotable box.
/// 2. Replace container box value arguments for the cloned closure with the
///    transformed address or value argument.
static void
computeNewArgInterfaceTypes(SILFunction *f, IndicesSet &promotableIndices,
                            SmallVectorImpl<SILParameterInfo> &outTys,
                            ResilienceExpansion expansion) {
  auto fnConv = f->getConventions();
  auto parameters = fnConv.funcTy->getParameters();

  LLVM_DEBUG(llvm::dbgs() << "Preparing New Args!\n");

  auto &types = f->getModule().Types;

  // For each parameter in the old function...
  for (unsigned index : indices(parameters)) {
    auto &param = parameters[index];

    // The PromotableIndices index is expressed as the argument index (num
    // indirect result + param index). Add back the num indirect results to get
    // the arg index when working with PromotableIndices.
    unsigned argIndex = index + fnConv.getSILArgIndexOfFirstParam();

    LLVM_DEBUG(llvm::dbgs()
                   << "Index: " << index << "; PromotableIndices: "
                   << (promotableIndices.count(argIndex) ? "yes" : "no")
                   << " Param: ";
               param.print(llvm::dbgs()));

    if (!promotableIndices.count(argIndex)) {
      outTys.push_back(param);
      continue;
    }

    // Perform the proper conversions and then add it to the new parameter list
    // for the type.
    assert(!param.isFormalIndirect());
    auto paramTy =
        param.getSILStorageType(fnConv.silConv.getModule(), fnConv.funcTy,
                                TypeExpansionContext::minimal());
    auto paramBoxTy = paramTy.castTo<SILBoxType>();
    assert(paramBoxTy->getLayout()->getFields().size() == 1 &&
           "promoting compound box not implemented yet");
    auto paramBoxedTy =
        getSILBoxFieldType(TypeExpansionContext(*f), paramBoxTy, types, 0);
    assert(expansion == f->getResilienceExpansion());
    auto &paramTL = types.getTypeLowering(paramBoxedTy, *f);
    ParameterConvention convention;
    if (paramTL.isAddressOnly()) {
      convention = ParameterConvention::Indirect_In;
    } else if (paramTL.isTrivial()) {
      convention = ParameterConvention::Direct_Unowned;
    } else {
      convention = param.isGuaranteed() ? ParameterConvention::Direct_Guaranteed
                                        : ParameterConvention::Direct_Owned;
    }
    outTys.push_back(SILParameterInfo(paramBoxedTy.getASTType(), convention));
  }
}

static std::string getSpecializedName(SILFunction *f, IsSerialized_t serialized,
                                      IndicesSet &promotableIndices) {
  auto p = Demangle::SpecializationPass::CapturePromotion;
  Mangle::FunctionSignatureSpecializationMangler mangler(p, serialized, f);
  auto fnConv = f->getConventions();

  for (unsigned argIdx = 0, endIdx = fnConv.getNumSILArguments();
       argIdx < endIdx; ++argIdx) {
    if (!promotableIndices.count(argIdx))
      continue;
    mangler.setArgumentBoxToValue(argIdx);
  }
  return mangler.mangle();
}

/// Create the function corresponding to the clone of the original
/// closure with the signature modified to reflect promotable captures (which
/// are given by PromotableIndices, such that each entry in the set is the
/// index of the box containing the variable in the closure's argument list, and
/// the address of the box's contents is the argument immediately following each
/// box argument); does not actually clone the body of the function
///
/// *NOTE* PromotableIndices only contains the container value of the box, not
/// the address value.
SILFunction *
ClosureCloner::initCloned(SILOptFunctionBuilder &functionBuilder,
                          SILFunction *orig, IsSerialized_t serialized,
                          StringRef clonedName, IndicesSet &promotableIndices,
                          ResilienceExpansion resilienceExpansion) {
  SILModule &mod = orig->getModule();

  // Compute the arguments for our new function.
  SmallVector<SILParameterInfo, 4> clonedInterfaceArgTys;
  computeNewArgInterfaceTypes(orig, promotableIndices, clonedInterfaceArgTys,
                              resilienceExpansion);

  SILFunctionType *origFTI = orig->getLoweredFunctionType();

  // Create the thin function type for the cloned closure.
  auto clonedTy = SILFunctionType::get(
      origFTI->getInvocationGenericSignature(), origFTI->getExtInfo(),
      origFTI->getCoroutineKind(), origFTI->getCalleeConvention(),
      clonedInterfaceArgTys, origFTI->getYields(), origFTI->getResults(),
      origFTI->getOptionalErrorResult(), SubstitutionMap(), SubstitutionMap(),
      mod.getASTContext(), origFTI->getWitnessMethodConformanceOrInvalid());

  assert((orig->isTransparent() || orig->isBare() || orig->getLocation()) &&
         "SILFunction missing location");
  assert((orig->isTransparent() || orig->isBare() || orig->getDebugScope()) &&
         "SILFunction missing DebugScope");
  assert(!orig->isGlobalInit() && "Global initializer cannot be cloned");

  auto *fn = functionBuilder.createFunction(
      orig->getLinkage(), clonedName, clonedTy, orig->getGenericEnvironment(),
      orig->getLocation(), orig->isBare(), IsNotTransparent, serialized,
      IsNotDynamic, orig->getEntryCount(), orig->isThunk(),
      orig->getClassSubclassScope(), orig->getInlineStrategy(),
      orig->getEffectsKind(), orig, orig->getDebugScope());
  for (auto &attr : orig->getSemanticsAttrs())
    fn->addSemanticsAttr(attr);
  if (!orig->hasOwnership()) {
    fn->setOwnershipEliminated();
  }
  return fn;
}

/// Populate the body of the cloned closure, modifying instructions as
/// necessary to take into consideration the promoted capture(s)
void ClosureCloner::populateCloned() {
  SILFunction *cloned = getCloned();

  // Create arguments for the entry block
  SILBasicBlock *origEntryBB = &*origF->begin();
  SILBasicBlock *clonedEntryBB = cloned->createBasicBlock();
  getBuilder().setInsertionPoint(clonedEntryBB);

  SmallVector<SILValue, 4> entryArgs;
  entryArgs.reserve(origEntryBB->getArguments().size());

  unsigned argNo = 0;
  auto ai = origEntryBB->args_begin(), ae = origEntryBB->args_end();
  for (; ai != ae; ++argNo, ++ai) {
    if (!promotableIndices.count(argNo)) {
      // Simply create a new argument which copies the original argument
      SILValue mappedValue = clonedEntryBB->createFunctionArgument(
          (*ai)->getType(), (*ai)->getDecl());
      entryArgs.push_back(mappedValue);
      continue;
    }

    // Handle the case of a promoted capture argument.
    auto boxTy = (*ai)->getType().castTo<SILBoxType>();
    assert(boxTy->getLayout()->getFields().size() == 1 &&
           "promoting compound box not implemented");
    auto boxedTy = getSILBoxFieldType(TypeExpansionContext(*cloned), boxTy,
                                      cloned->getModule().Types, 0)
                       .getObjectType();
    SILValue mappedValue =
        clonedEntryBB->createFunctionArgument(boxedTy, (*ai)->getDecl());

    // If SIL ownership is enabled, we need to perform a borrow here if we have
    // a non-trivial value. We know that our value is not written to and it does
    // not escape. The use of a borrow enforces this.
    if (cloned->hasOwnership() &&
        mappedValue.getOwnershipKind() != OwnershipKind::None) {
      SILLocation loc(const_cast<ValueDecl *>((*ai)->getDecl()));
      mappedValue = getBuilder().emitBeginBorrowOperation(loc, mappedValue);
    }
    entryArgs.push_back(mappedValue);

    boxArgumentMap.insert(std::make_pair(*ai, mappedValue));

    // Track the projections of the box.
    for (auto *use : (*ai)->getUses()) {
      if (auto *pbi = dyn_cast<ProjectBoxInst>(use->getUser())) {
        projectBoxArgumentMap.insert(std::make_pair(pbi, mappedValue));
      }
    }
  }

  // Visit original BBs in depth-first preorder, starting with the
  // entry block, cloning all instructions and terminators.
  cloneFunctionBody(origF, clonedEntryBB, entryArgs);
}

SILFunction *ClosureCloner::constructClonedFunction(
    SILOptFunctionBuilder &funcBuilder, PartialApplyInst *pai,
    FunctionRefInst *fri, IndicesSet &promotableIndices,
    ResilienceExpansion resilienceExpansion) {
  SILFunction *f = pai->getFunction();

  // Create the Cloned Name for the function.
  SILFunction *origF = fri->getReferencedFunctionOrNull();

  IsSerialized_t isSerialized = IsNotSerialized;
  if (f->isSerialized() && origF->isSerialized())
    isSerialized = IsSerialized_t::IsSerializable;

  auto clonedName = getSpecializedName(origF, isSerialized, promotableIndices);

  // If we already have such a cloned function in the module then just use it.
  if (auto *prevF = f->getModule().lookUpFunction(clonedName)) {
    assert(prevF->isSerialized() == isSerialized);
    return prevF;
  }

  // Otherwise, create a new clone.
  ClosureCloner cloner(funcBuilder, origF, isSerialized, clonedName,
                       promotableIndices, resilienceExpansion);
  cloner.populateCloned();
  return cloner.getCloned();
}

/// If this operand originates from a mapped ProjectBox, return the mapped
/// value. Otherwise return an invalid value.
SILValue ClosureCloner::getProjectBoxMappedVal(SILValue operandValue) {
  if (auto *bai = dyn_cast<BeginAccessInst>(operandValue))
    operandValue = bai->getSource();

  if (auto *pbi = dyn_cast<ProjectBoxInst>(operandValue)) {
    auto iter = projectBoxArgumentMap.find(pbi);
    if (iter != projectBoxArgumentMap.end())
      return iter->second;
  }
  return SILValue();
}

/// Handle a debug_value_addr instruction during cloning of a closure;
/// if its operand is the promoted address argument then lower it to a
/// debug_value, otherwise it is handled normally.
void ClosureCloner::visitDebugValueAddrInst(DebugValueAddrInst *inst) {
  if (SILValue value = getProjectBoxMappedVal(inst->getOperand())) {
    getBuilder().setCurrentDebugScope(getOpScope(inst->getDebugScope()));
    getBuilder().createDebugValue(inst->getLoc(), value, *inst->getVarInfo());
    return;
  }
  SILCloner<ClosureCloner>::visitDebugValueAddrInst(inst);
}

/// Handle a strong_release instruction during cloning of a closure; if
/// it is a strong release of a promoted box argument, then it is replaced with
/// a ReleaseValue of the new object type argument, otherwise it is handled
/// normally.
void ClosureCloner::visitStrongReleaseInst(StrongReleaseInst *inst) {
  assert(
      !inst->getFunction()->hasOwnership() &&
      "Should not see strong release in a function with qualified ownership");
  SILValue operandValue = inst->getOperand();
  if (auto *arg = dyn_cast<SILArgument>(operandValue)) {
    auto iter = boxArgumentMap.find(arg);
    if (iter != boxArgumentMap.end()) {
      // Releases of the box arguments get replaced with ReleaseValue of the new
      // object type argument.
      auto &typeLowering =
          getBuilder().getTypeLowering(iter->second->getType());
      SILBuilderWithPostProcess<ClosureCloner, 1> b(this, inst);
      typeLowering.emitDestroyValue(b, inst->getLoc(), iter->second);
      return;
    }
  }

  SILCloner<ClosureCloner>::visitStrongReleaseInst(inst);
}

/// Handle a destroy_value instruction during cloning of a closure; if
/// it is a strong release of a promoted box argument, then it is replaced with
/// a destroy_value of the new object type argument, otherwise it is handled
/// normally.
void ClosureCloner::visitDestroyValueInst(DestroyValueInst *inst) {
  SILValue operand = inst->getOperand();
  if (auto *arg = dyn_cast<SILArgument>(operand)) {
    auto iter = boxArgumentMap.find(arg);
    if (iter != boxArgumentMap.end()) {
      // Releases of the box arguments get replaced with an end_borrow,
      // destroy_value of the new object type argument.
      SILFunction &f = getBuilder().getFunction();
      auto &typeLowering = f.getTypeLowering(iter->second->getType());
      SILBuilderWithPostProcess<ClosureCloner, 1> b(this, inst);
      SILValue value = iter->second;

      // If ownership is enabled, then we must emit a begin_borrow for any
      // non-trivial value.
      if (f.hasOwnership() && value.getOwnershipKind() != OwnershipKind::None) {
        auto *bbi = cast<BeginBorrowInst>(value);
        value = bbi->getOperand();
        b.emitEndBorrowOperation(inst->getLoc(), bbi);
      }

      typeLowering.emitDestroyValue(b, inst->getLoc(), value);
      return;
    }
  }

  SILCloner<ClosureCloner>::visitDestroyValueInst(inst);
}

/// Handle a struct_element_addr instruction during cloning of a closure.
///
/// If its operand is the promoted address argument then ignore it, otherwise it
/// is handled normally.
void ClosureCloner::visitStructElementAddrInst(StructElementAddrInst *seai) {
  if (getProjectBoxMappedVal(seai->getOperand()))
    return;

  SILCloner<ClosureCloner>::visitStructElementAddrInst(seai);
}

/// project_box of captured boxes can be eliminated.
void ClosureCloner::visitProjectBoxInst(ProjectBoxInst *pbi) {
  if (auto *arg = dyn_cast<SILArgument>(pbi->getOperand()))
    if (boxArgumentMap.count(arg))
      return;

  SILCloner<ClosureCloner>::visitProjectBoxInst(pbi);
}

/// If its operand is the promoted address argument then ignore it, otherwise it
/// is handled normally.
void ClosureCloner::visitBeginAccessInst(BeginAccessInst *bai) {
  if (getProjectBoxMappedVal(bai->getSource()))
    return;

  SILCloner<ClosureCloner>::visitBeginAccessInst(bai);
}

/// If its operand is the promoted address argument then ignore it, otherwise it
/// is handled normally.
void ClosureCloner::visitEndAccessInst(EndAccessInst *eai) {
  if (getProjectBoxMappedVal(eai->getBeginAccess()))
    return;

  SILCloner<ClosureCloner>::visitEndAccessInst(eai);
}

/// Handle a load_borrow instruction during cloning of a closure.
///
/// The two relevant cases are a direct load from a promoted address argument or
/// a load of a struct_element_addr of a promoted address argument.
void ClosureCloner::visitLoadBorrowInst(LoadBorrowInst *lbi) {
  assert(lbi->getFunction()->hasOwnership() &&
         "We should only see a load borrow in ownership qualified SIL");
  if (SILValue value = getProjectBoxMappedVal(lbi->getOperand())) {
    // Loads of the address argument get eliminated completely; the uses of
    // the loads get mapped to uses of the new object type argument.
    //
    // We assume that the value is already guaranteed.
    assert(
        value.getOwnershipKind().isCompatibleWith(OwnershipKind::Guaranteed) &&
        "Expected argument value to be guaranteed");
    recordFoldedValue(lbi, value);
    return;
  }

  SILCloner<ClosureCloner>::visitLoadBorrowInst(lbi);
  return;
}

/// Handle a load instruction during cloning of a closure.
///
/// The two relevant cases are a direct load from a promoted address argument or
/// a load of a struct_element_addr of a promoted address argument.
void ClosureCloner::visitLoadInst(LoadInst *li) {
  if (SILValue value = getProjectBoxMappedVal(li->getOperand())) {
    // Loads of the address argument get eliminated completely; the uses of
    // the loads get mapped to uses of the new object type argument.
    //
    // If we are compiling with SIL ownership, we need to take different
    // behaviors depending on the type of load. Specifically, if we have a
    // load [copy], then we need to add a copy_value here. If we have a take
    // or trivial, we just propagate the value through.
    if (li->getFunction()->hasOwnership() &&
        li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
      value = getBuilder().createCopyValue(li->getLoc(), value);
    }
    recordFoldedValue(li, value);
    return;
  }

  auto *seai = dyn_cast<StructElementAddrInst>(li->getOperand());
  if (!seai) {
    SILCloner<ClosureCloner>::visitLoadInst(li);
    return;
  }

  if (SILValue value = getProjectBoxMappedVal(seai->getOperand())) {
    // Loads of a struct_element_addr of an argument get replaced with a
    // struct_extract of the new passed in value. The value should be borrowed
    // already, so we can just extract the value.
    assert(
        !getBuilder().getFunction().hasOwnership() ||
        value.getOwnershipKind().isCompatibleWith(OwnershipKind::Guaranteed));
    value = getBuilder().emitStructExtract(li->getLoc(), value,
                                           seai->getField(), li->getType());

    // If we were performing a load [copy], then we need to a perform a copy
    // here since when cloning, we do not eliminate the destroy on the copied
    // value.
    if (li->getFunction()->hasOwnership() &&
        li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
      value = getBuilder().createCopyValue(li->getLoc(), value);
    }
    recordFoldedValue(li, value);
    return;
  }
  SILCloner<ClosureCloner>::visitLoadInst(li);
}

//===----------------------------------------------------------------------===//
//                        EscapeMutationScanningState
//===----------------------------------------------------------------------===//

namespace {

struct EscapeMutationScanningState {
  /// The list of mutations that we found while checking for escapes.
  llvm::SmallVector<SILInstruction *, 8> foundMutations;

  /// A flag that we use to ensure that we only ever see 1 project_box on an
  /// alloc_box.
  bool sawProjectBoxInst;

  /// The global partial_apply -> index map.
  llvm::DenseMap<PartialApplyInst *, unsigned> &globalIndexMap;
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//         Partial Apply BoxArg Mutation/Escape/Capture Use Analysis
//===----------------------------------------------------------------------===//

static SILArgument *getBoxFromIndex(SILFunction *f, unsigned index) {
  assert(f->isDefinition() && "Expected definition not external declaration!");
  auto &entry = f->front();
  return entry.getArgument(index);
}

static bool isNonMutatingLoad(SILInstruction *inst) {
  auto *li = dyn_cast<LoadInst>(inst);
  if (!li)
    return false;
  return li->getOwnershipQualifier() != LoadOwnershipQualifier::Take;
}

/// Given a partial_apply instruction and the argument index into its
/// callee's argument list of a box argument (which is followed by an argument
/// for the address of the box's contents), return true if the closure is known
/// not to mutate the captured variable.
static bool isNonMutatingCapture(SILArgument *boxArg) {
  SmallVector<ProjectBoxInst *, 2> projectBoxInsts;

  // Conservatively do not allow any use of the box argument other than a
  // strong_release or projection, since this is the pattern expected from
  // SILGen.
  for (auto *use : boxArg->getUses()) {
    if (isa<StrongReleaseInst>(use->getUser()) ||
        isa<DestroyValueInst>(use->getUser()))
      continue;

    if (auto *pbi = dyn_cast<ProjectBoxInst>(use->getUser())) {
      projectBoxInsts.push_back(pbi);
      continue;
    }

    return false;
  }

  // Only allow loads of projections, either directly or via
  // struct_element_addr instructions.
  //
  // TODO: This seems overly limited.  Why not projections of tuples and other
  // stuff?  Also, why not recursive struct elements?  This should be a helper
  // function that mirrors isNonEscapingUse.
  auto isAddrUseMutating = [](SILInstruction *addrInst) {
    if (auto *seai = dyn_cast<StructElementAddrInst>(addrInst)) {
      return all_of(seai->getUses(), [](Operand *op) -> bool {
        return isNonMutatingLoad(op->getUser());
      });
    }

    return isNonMutatingLoad(addrInst) || isa<DebugValueAddrInst>(addrInst) ||
           isa<MarkFunctionEscapeInst>(addrInst) ||
           isa<EndAccessInst>(addrInst);
  };

  for (auto *pbi : projectBoxInsts) {
    for (auto *use : pbi->getUses()) {
      if (auto *bai = dyn_cast<BeginAccessInst>(use->getUser())) {
        for (auto *accessUseOper : bai->getUses()) {
          if (!isAddrUseMutating(accessUseOper->getUser()))
            return false;
        }
        continue;
      }

      if (!isAddrUseMutating(use->getUser()))
        return false;
    }
  }

  return true;
}

bool isPartialApplyNonEscapingUser(Operand *currentOp, PartialApplyInst *pai,
                                   EscapeMutationScanningState &state) {
  LLVM_DEBUG(llvm::dbgs() << "    Found partial: " << *pai);

  unsigned opNo = currentOp->getOperandNumber();
  assert(opNo != 0 && "Alloc box used as callee of partial apply?");

  // If we've already seen this partial apply, then it means the same alloc
  // box is being captured twice by the same closure, which is odd and
  // unexpected: bail instead of trying to handle this case.
  if (state.globalIndexMap.count(pai)) {
    LLVM_DEBUG(llvm::dbgs() << "        FAIL! Already seen.\n");
    return false;
  }

  SILModule &mod = pai->getModule();
  SILFunction *f = pai->getFunction();
  auto closureType = pai->getType().castTo<SILFunctionType>();
  SILFunctionConventions closureConv(closureType, mod);

  // Calculate the index into the closure's argument list of the captured
  // box pointer (the captured address is always the immediately following
  // index so is not stored separately);
  unsigned index = opNo - 1 + closureConv.getNumSILArguments();

  auto *fn = pai->getReferencedFunctionOrNull();

  // It is not safe to look at the content of dynamically replaceable functions
  // since this pass looks at the content of Fn.
  if (!fn || !fn->isDefinition() || fn->isDynamicallyReplaceable()) {
    LLVM_DEBUG(llvm::dbgs() << "        FAIL! Not a direct function definition "
                               "reference.\n");
    return false;
  }

  SILArgument *boxArg = getBoxFromIndex(fn, index);

  // For now, return false is the address argument is an address-only type,
  // since we currently handle loadable types only.
  // TODO: handle address-only types
  // FIXME: Expansion
  auto boxTy = boxArg->getType().castTo<SILBoxType>();
  assert(boxTy->getLayout()->getFields().size() == 1 &&
         "promoting compound box not implemented yet");
  if (getSILBoxFieldType(TypeExpansionContext(*fn), boxTy, mod.Types, 0)
          .isAddressOnly(*f)) {
    LLVM_DEBUG(llvm::dbgs() << "        FAIL! Box is an address only "
                               "argument!\n");
    return false;
  }

  // Verify that this closure is known not to mutate the captured value; if
  // it does, then conservatively refuse to promote any captures of this
  // value.
  if (!isNonMutatingCapture(boxArg)) {
    LLVM_DEBUG(llvm::dbgs() << "        FAIL: Have a mutating capture!\n");
    return false;
  }

  // Record the index and continue.
  LLVM_DEBUG(llvm::dbgs()
             << "        Partial apply does not escape, may be optimizable!\n");
  LLVM_DEBUG(llvm::dbgs() << "        Index: " << index << "\n");
  state.globalIndexMap.insert(std::make_pair(pai, index));
  return true;
}

//===----------------------------------------------------------------------===//
//                     Project Box Escaping Use Analysis
//===----------------------------------------------------------------------===//

namespace {

class NonEscapingUserVisitor
    : public SILInstructionVisitor<NonEscapingUserVisitor, bool> {
  llvm::SmallVector<Operand *, 32> worklist;
  llvm::SmallVectorImpl<SILInstruction *> &foundMutations;
  NullablePtr<Operand> currentOp;

public:
  NonEscapingUserVisitor(
      Operand *initialOperand,
      llvm::SmallVectorImpl<SILInstruction *> &foundMutations)
      : worklist(), foundMutations(foundMutations), currentOp() {
    worklist.push_back(initialOperand);
  }

  NonEscapingUserVisitor(const NonEscapingUserVisitor &) = delete;
  NonEscapingUserVisitor &operator=(const NonEscapingUserVisitor &) = delete;
  NonEscapingUserVisitor(NonEscapingUserVisitor &&) = delete;
  NonEscapingUserVisitor &operator=(NonEscapingUserVisitor &&) = delete;

  bool compute() {
    while (!worklist.empty()) {
      currentOp = worklist.pop_back_val();
      SILInstruction *user = currentOp.get()->getUser();

      // Ignore type dependent operands.
      if (user->isTypeDependentOperand(*(currentOp.get())))
        continue;

      // Then visit the specific user. This routine returns true if the value
      // does not escape. In such a case, continue.
      if (visit(user)) {
        continue;
      }

      return false;
    }

    return true;
  }

  /// Visit a random value base.
  ///
  /// These are considered to be escapes.
  bool visitSILInstruction(SILInstruction *inst) {
    LLVM_DEBUG(llvm::dbgs()
               << "    FAIL! Have unknown escaping user: " << *inst);
    return false;
  }

#define ALWAYS_NON_ESCAPING_INST(INST)                                         \
  bool visit##INST##Inst(INST##Inst *) { return true; }
  // Marking the boxed value as escaping is OK. It's just a DI annotation.
  ALWAYS_NON_ESCAPING_INST(MarkFunctionEscape)
  // These remaining instructions are ok and don't count as mutations.
  ALWAYS_NON_ESCAPING_INST(StrongRetain)
  ALWAYS_NON_ESCAPING_INST(Load)
  ALWAYS_NON_ESCAPING_INST(StrongRelease)
  ALWAYS_NON_ESCAPING_INST(DestroyValue)
#undef ALWAYS_NON_ESCAPING_INST

  bool visitDeallocBoxInst(DeallocBoxInst *dbi) {
    foundMutations.push_back(dbi);
    return true;
  }

  bool visitEndAccessInst(EndAccessInst *) { return true; }

  bool visitApplyInst(ApplyInst *ai) {
    auto argIndex = currentOp.get()->getOperandNumber() - 1;
    SILFunctionConventions substConv(ai->getSubstCalleeType(), ai->getModule());
    auto convention = substConv.getSILArgumentConvention(argIndex);
    if (!convention.isIndirectConvention()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "    FAIL! Found non indirect apply user: " << *ai);
      return false;
    }
    foundMutations.push_back(ai);
    return true;
  }

  /// Add the Operands of a transitive use instruction to the worklist.
  void addUsesToWorklist(SingleValueInstruction *svi) {
    for (auto *use : svi->getUses()) {
      worklist.push_back(use);
    }
  }

  /// This is separate from the normal copy value handling since we are matching
  /// the old behavior of non-top-level uses not being able to have partial
  /// apply and project box uses.
  struct detail {
    enum IsMutating_t {
      IsNotMutating = 0,
      IsMutating = 1,
    };
  };
#define RECURSIVE_INST_VISITOR(MUTATING, INST)                                 \
  bool visit##INST##Inst(INST##Inst *i) {                                      \
    if (bool(detail::MUTATING)) {                                              \
      foundMutations.push_back(i);                                             \
    }                                                                          \
    addUsesToWorklist(i);                                                      \
    return true;                                                               \
  }
  // *NOTE* It is important that we do not have copy_value here. The reason why
  // is that we only want to handle copy_value directly of the alloc_box without
  // going through any other instructions. This protects our optimization later
  // on.
  //
  // Additionally, copy_value is not a valid use of any of the instructions that
  // we allow through.
  //
  // TODO: Can we ever hit copy_values here? If we do, we may be missing
  // opportunities.
  RECURSIVE_INST_VISITOR(IsNotMutating, StructElementAddr)
  RECURSIVE_INST_VISITOR(IsNotMutating, TupleElementAddr)
  RECURSIVE_INST_VISITOR(IsNotMutating, InitEnumDataAddr)
  RECURSIVE_INST_VISITOR(IsNotMutating, OpenExistentialAddr)
  // begin_access may signify a modification, but is considered nonmutating
  // because we will peek though it's uses to find the actual mutation.
  RECURSIVE_INST_VISITOR(IsNotMutating, BeginAccess)
  RECURSIVE_INST_VISITOR(IsMutating, UncheckedTakeEnumDataAddr)
#undef RECURSIVE_INST_VISITOR

  bool visitCopyAddrInst(CopyAddrInst *cai) {
    if (currentOp.get()->getOperandNumber() == 1 || cai->isTakeOfSrc())
      foundMutations.push_back(cai);
    return true;
  }

  bool visitStoreInst(StoreInst *si) {
    if (currentOp.get()->getOperandNumber() != 1) {
      LLVM_DEBUG(llvm::dbgs() << "    FAIL! Found store of pointer: " << *si);
      return false;
    }
    foundMutations.push_back(si);
    return true;
  }

  bool visitAssignInst(AssignInst *ai) {
    if (currentOp.get()->getOperandNumber() != 1) {
      LLVM_DEBUG(llvm::dbgs() << "    FAIL! Found store of pointer: " << *ai);
      return false;
    }
    foundMutations.push_back(ai);
    return true;
  }
};

} // end anonymous namespace

/// Given a use of an alloc_box instruction, return true if the use
/// definitely does not allow the box to escape; also, if the use is an
/// instruction which possibly mutates the contents of the box, then add it to
/// the Mutations vector.
static bool isNonEscapingUse(Operand *initialOp,
                             EscapeMutationScanningState &state) {
  return NonEscapingUserVisitor(initialOp, state.foundMutations).compute();
}

static bool isProjectBoxNonEscapingUse(ProjectBoxInst *pbi,
                                       EscapeMutationScanningState &state) {
  LLVM_DEBUG(llvm::dbgs() << "    Found project box: " << *pbi);

  for (Operand *addrOp : pbi->getUses()) {
    if (!isNonEscapingUse(addrOp, state)) {
      LLVM_DEBUG(llvm::dbgs() << "    FAIL! Has escaping user of addr:"
                              << *addrOp->getUser());
      return false;
    }
  }

  return true;
}

//===----------------------------------------------------------------------===//
//                Top Level AllocBox Escape/Mutation Analysis
//===----------------------------------------------------------------------===//

static bool scanUsesForEscapesAndMutations(Operand *op,
                                           EscapeMutationScanningState &state) {
  SILInstruction *user = op->getUser();

  if (auto *pai = dyn_cast<PartialApplyInst>(user)) {
    return isPartialApplyNonEscapingUser(op, pai, state);
  }

  // A mark_dependence user on a partial_apply is safe.
  if (auto *mdi = dyn_cast<MarkDependenceInst>(user)) {
    if (mdi->getBase() == op->get()) {
      auto parent = mdi->getValue();
      while ((mdi = dyn_cast<MarkDependenceInst>(parent))) {
        parent = mdi->getValue();
      }
      return isa<PartialApplyInst>(parent);
    }
  }

  if (auto *pbi = dyn_cast<ProjectBoxInst>(user)) {
    // It is assumed in later code that we will only have 1 project_box. This
    // can be seen since there is no code for reasoning about multiple
    // boxes. Just put in the restriction so we are consistent.
    if (state.sawProjectBoxInst)
      return false;
    state.sawProjectBoxInst = true;
    return isProjectBoxNonEscapingUse(pbi, state);
  }

  // Given a top level copy value use or mark_uninitialized, check all of its
  // user operands as if they were apart of the use list of the base operand.
  //
  // This is a separate code path from the non escaping user visitor check since
  // we want to be more conservative around non-top level copies (i.e. a copy
  // derived from a projection like instruction). In fact such a thing may not
  // even make any sense!
  if (isa<CopyValueInst>(user) || isa<MarkUninitializedInst>(user)) {
    return all_of(cast<SingleValueInstruction>(user)->getUses(),
                  [&state](Operand *userOp) -> bool {
                    return scanUsesForEscapesAndMutations(userOp, state);
                  });
  }

  // Verify that this use does not otherwise allow the alloc_box to
  // escape.
  return isNonEscapingUse(op, state);
}

/// Examine an alloc_box instruction, returning true if at least one
/// capture of the boxed variable is promotable.  If so, then the pair of the
/// partial_apply instruction and the index of the box argument in the closure's
/// argument list is added to IM.
static bool
examineAllocBoxInst(AllocBoxInst *abi, ReachabilityInfo &ri,
                    llvm::DenseMap<PartialApplyInst *, unsigned> &im) {
  LLVM_DEBUG(llvm::dbgs() << "Visiting alloc box: " << *abi);
  EscapeMutationScanningState state{{}, false, im};

  // Scan the box for interesting uses.
  if (any_of(abi->getUses(), [&state](Operand *op) {
        return !scanUsesForEscapesAndMutations(op, state);
      })) {
    LLVM_DEBUG(llvm::dbgs()
               << "Found an escaping use! Can not optimize this alloc box?!\n");
    return false;
  }

  LLVM_DEBUG(llvm::dbgs() << "We can optimize this alloc box!\n");

  // Helper lambda function to determine if instruction b is strictly after
  // instruction a, assuming both are in the same basic block.
  auto isAfter = [](SILInstruction *a, SILInstruction *b) {
    auto fIter = b->getParent()->begin();
    auto bIter = b->getIterator();
    auto aIter = a->getIterator();
    while (bIter != fIter) {
      --bIter;
      if (aIter == bIter)
        return true;
    }
    return false;
  };

  LLVM_DEBUG(llvm::dbgs()
             << "Checking for any mutations that invalidate captures...\n");
  // Loop over all mutations to possibly invalidate captures.
  for (auto *inst : state.foundMutations) {
    auto iter = im.begin();
    while (iter != im.end()) {
      auto *pai = iter->first;
      // The mutation invalidates a capture if it occurs in a block reachable
      // from the block the partial_apply is in, or if it is in the same
      // block is after the partial_apply.
      if (ri.isReachable(pai->getParent(), inst->getParent()) ||
          (pai->getParent() == inst->getParent() && isAfter(pai, inst))) {
        LLVM_DEBUG(llvm::dbgs() << "    Invalidating: " << *pai);
        LLVM_DEBUG(llvm::dbgs() << "    Because of user: " << *inst);
        auto prev = iter++;
        im.erase(prev);
        continue;
      }
      ++iter;
    }
    // If there are no valid captures left, then stop.
    if (im.empty()) {
      LLVM_DEBUG(llvm::dbgs() << "    Ran out of valid captures... bailing!\n");
      return false;
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "    We can optimize this box!\n");
  return true;
}

/// For an alloc_box or iterated copy_value alloc_box, get or create the
/// project_box for the copy or original alloc_box.
///
/// There are two possible case here:
///
/// 1. It could be an alloc box.
/// 2. It could be an iterated copy_value from an alloc_box.
///
/// Some important constraints from our initial safety condition checks:
///
/// 1. We only see a project_box paired with an alloc_box. e.x.:
///
///       (project_box (alloc_box)).
///
/// 2. We only see a mark_uninitialized when paired with an (alloc_box,
///    project_box). e.x.:
///
///       (project_box (mark_uninitialized (alloc_box)))
///
/// The asserts are to make sure that if the initial safety condition check
/// is changed, this code is changed as well.
static SILValue getOrCreateProjectBoxHelper(SILValue partialOperand) {
  // If we have a copy_value, just create a project_box on the copy and return.
  if (auto *cvi = dyn_cast<CopyValueInst>(partialOperand)) {
    SILBuilderWithScope b(std::next(cvi->getIterator()));
    return b.createProjectBox(cvi->getLoc(), cvi, 0);
  }

  // Otherwise, handle the alloc_box case. If we have a mark_uninitialized on
  // the box, we know that we will have a project_box of that value due to SIL
  // verifier invariants.
  SingleValueInstruction *box = cast<AllocBoxInst>(partialOperand);
  if (auto *mui = box->getSingleUserOfType<MarkUninitializedInst>()) {
    if (auto *pbi = mui->getSingleUserOfType<ProjectBoxInst>()) {
      return pbi;
    }
  }

  // Otherwise, create a new project_box.
  SILBuilderWithScope b(std::next(box->getIterator()));
  return b.createProjectBox(box->getLoc(), box, 0);
}

//===----------------------------------------------------------------------===//
//         Top Level Processing of Partial Applies with AllocBox Args
//===----------------------------------------------------------------------===//

/// Change the base in mark_dependence.
static void
mapMarkDependenceArguments(SingleValueInstruction *root,
                           llvm::DenseMap<SILValue, SILValue> &map,
                           SmallVectorImpl<SILInstruction *> &toDelete) {
  SmallVector<Operand *, 16> useWorklist(root->getUses());
  for (auto *use : useWorklist) {
    if (auto *mdi = dyn_cast<MarkDependenceInst>(use->getUser())) {
      mapMarkDependenceArguments(mdi, map, toDelete);
      auto iter = map.find(mdi->getBase());
      if (iter != map.end()) {
        mdi->setBase(iter->second);
      }
      // Remove mark_dependence on trivial values.
      if (mdi->getBase()->getType().isTrivial(*mdi->getFunction())) {
        mdi->replaceAllUsesWith(mdi->getValue());
        toDelete.push_back(mdi);
      }
    }
  }
}

/// Given a partial_apply instruction and a set of promotable indices,
/// clone the closure with the promoted captures and replace the partial_apply
/// with a partial_apply of the new closure, fixing up reference counting as
/// necessary. Also, if the closure is cloned, the cloned function is added to
/// the worklist.
static SILFunction *
processPartialApplyInst(SILOptFunctionBuilder &funcBuilder,
                        PartialApplyInst *pai, IndicesSet &promotableIndices,
                        SmallVectorImpl<SILFunction *> &worklist) {
  SILFunction *f = pai->getFunction();
  SILModule &mod = pai->getModule();
  auto *fri = dyn_cast<FunctionRefInst>(pai->getCallee());

  // Clone the closure with the given promoted captures.
  SILFunction *clonedFn = ClosureCloner::constructClonedFunction(
      funcBuilder, pai, fri, promotableIndices, f->getResilienceExpansion());
  worklist.push_back(clonedFn);

  // Initialize a SILBuilder and create a function_ref referencing the cloned
  // closure.
  SILBuilderWithScope builder(pai);
  builder.addOpenedArchetypeOperands(pai);
  SILValue fnVal = builder.createFunctionRef(pai->getLoc(), clonedFn);

  // Populate the argument list for a new partial_apply instruction, taking into
  // consideration any captures.
  auto calleeFunctionTy = pai->getCallee()->getType().castTo<SILFunctionType>();
  auto substCalleeFunctionTy = calleeFunctionTy;
  if (pai->hasSubstitutions())
    substCalleeFunctionTy = calleeFunctionTy->substGenericArgs(
        mod, pai->getSubstitutionMap(), TypeExpansionContext(*f));
  SILFunctionConventions calleeConv(substCalleeFunctionTy, mod);
  auto calleePInfo = substCalleeFunctionTy->getParameters();
  SILFunctionConventions paConv(pai->getType().castTo<SILFunctionType>(), mod);
  unsigned firstIndex = paConv.getNumSILArguments();
  unsigned opNo = 1;
  unsigned opCount = pai->getNumOperands() - pai->getNumTypeDependentOperands();
  SmallVector<SILValue, 16> args;
  auto numIndirectResults = calleeConv.getNumIndirectSILResults();
  llvm::DenseMap<SILValue, SILValue> capturedMap;
  llvm::SmallSet<SILValue, 16> newCaptures;
  for (; opNo != opCount; ++opNo) {
    unsigned index = opNo - 1 + firstIndex;
    if (!promotableIndices.count(index)) {
      args.push_back(pai->getOperand(opNo));
      continue;
    }

    // First the grab the box and projected_box for the box value.
    //
    // *NOTE* Box may be a copy_value.
    SILValue box = pai->getOperand(opNo);
    SILValue addr = getOrCreateProjectBoxHelper(box);

    auto &typeLowering = f->getTypeLowering(addr->getType());
    auto newCaptured =
        typeLowering.emitLoadOfCopy(builder, pai->getLoc(), addr, IsNotTake);
    args.push_back(newCaptured);

    capturedMap[box] = newCaptured;
    newCaptures.insert(newCaptured);

    // A partial_apply [stack] does not own the captured argument but we must
    // destroy the projected object. We will do so after having created the new
    // partial_apply below.
    if (pai->isOnStack())
      continue;

    // Cleanup the captured argument.
    //
    // *NOTE* If we initially had a box, then this is on the actual
    // alloc_box. Otherwise, it is on the specific iterated copy_value that we
    // started with.
    SILParameterInfo cpInfo = calleePInfo[index - numIndirectResults];
    assert(calleeConv.getSILType(cpInfo, builder.getTypeExpansionContext()) ==
               box->getType() &&
           "SILType of parameter info does not match type of parameter");
    releasePartialApplyCapturedArg(builder, pai->getLoc(), box, cpInfo);
    ++NumCapturesPromoted;
  }

  // Create a new partial apply with the new arguments.
  auto *newPAI = builder.createPartialApply(
      pai->getLoc(), fnVal, pai->getSubstitutionMap(), args,
      pai->getType().getAs<SILFunctionType>()->getCalleeConvention(),
      pai->isOnStack());
  pai->replaceAllUsesWith(newPAI);
  pai->eraseFromParent();
  if (fri->use_empty()) {
    fri->eraseFromParent();
    // TODO: If this is the last use of the closure, and if it has internal
    // linkage, we should remove it from the SILModule now.
  }

  if (newPAI->isOnStack()) {
    // Insert destroy's of new captured arguments.
    for (auto *use : newPAI->getUses()) {
      if (auto *dsi = dyn_cast<DeallocStackInst>(use->getUser())) {
        builder.setInsertionPoint(std::next(SILBasicBlock::iterator(dsi)));
        insertDestroyOfCapturedArguments(
            newPAI, builder,
            [&](SILValue arg) -> bool { return newCaptures.count(arg); });
      }
    }
    // Map the mark dependence arguments.
    SmallVector<SILInstruction *, 16> toDelete;
    mapMarkDependenceArguments(newPAI, capturedMap, toDelete);
    for (auto *inst : toDelete)
      inst->eraseFromParent();
  }

  return clonedFn;
}

static void constructMapFromPartialApplyToPromotableIndices(
    SILFunction *f, PartialApplyIndicesMap &partialApplyIndicesAccumulator) {
  ReachabilityInfo reachabilityInfo(f);

  // This is a map from each partial apply to a single index which is a
  // promotable box variable for the alloc_box currently being considered.
  llvm::DenseMap<PartialApplyInst *, unsigned> incrementalIndexMap;

  // Consider all alloc_box instructions in the function.
  for (auto &block : *f) {
    for (auto &inst : block) {
      if (auto *abi = dyn_cast<AllocBoxInst>(&inst)) {
        incrementalIndexMap.clear();
        if (examineAllocBoxInst(abi, reachabilityInfo, incrementalIndexMap)) {
          // If we are able to promote at least one capture of the alloc_box,
          // then add the promotable index to the main map.
          for (auto &indexPair : incrementalIndexMap)
            partialApplyIndicesAccumulator[indexPair.first].insert(
                indexPair.second);
        }
        LLVM_DEBUG(llvm::dbgs() << "\n");
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class CapturePromotionPass : public SILModuleTransform {
  /// The entry point to the transformation.
  void run() override {
    SmallVector<SILFunction *, 128> worklist;
    for (auto &f : *getModule()) {
      if (f.wasDeserializedCanonical())
        continue;

      processFunction(&f, worklist);
    }

    while (!worklist.empty()) {
      processFunction(worklist.pop_back_val(), worklist);
    }
  }

  void processFunction(SILFunction *f,
                       SmallVectorImpl<SILFunction *> &worklist);
};

} // end anonymous namespace

void CapturePromotionPass::processFunction(
    SILFunction *func, SmallVectorImpl<SILFunction *> &worklist) {
  LLVM_DEBUG(llvm::dbgs() << "******** Performing Capture Promotion on: "
                          << func->getName() << "********\n");
  // This is a map from each partial apply to a set of indices of promotable
  // box variables.
  PartialApplyIndicesMap indicesMap;
  constructMapFromPartialApplyToPromotableIndices(func, indicesMap);

  // Do the actual promotions; all promotions on a single partial_apply are
  // handled together.
  SILOptFunctionBuilder funcBuilder(*this);
  for (auto &indicesPair : indicesMap) {
    PartialApplyInst *pai = indicesPair.first;
    SILFunction *clonedFn =
        processPartialApplyInst(funcBuilder, pai, indicesPair.second, worklist);
    (void)clonedFn;
  }
  invalidateAnalysis(func, SILAnalysis::InvalidationKind::Everything);
}

SILTransform *swift::createCapturePromotion() {
  return new CapturePromotionPass();
}
