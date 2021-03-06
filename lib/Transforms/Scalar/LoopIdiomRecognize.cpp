//===-- LoopIdiomRecognize.cpp - Loop idiom recognition -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass implements an idiom recognizer that transforms simple loops into a
// non-loop form.  In cases that this kicks in, it can be a significant
// performance win.
//
//===----------------------------------------------------------------------===//
//
// TODO List:
//
// Future loop memory idioms to recognize:
//   memcmp, memmove, strlen, etc.
// Future floating point idioms to recognize in -ffast-math mode:
//   fpowi
// Future integer operation idioms to recognize:
//   ctpop, ctlz, cttz
//
// Beware that isel's default lowering for ctpop is highly inefficient for
// i64 and larger types when i64 is legal and the value has few bits set.  It
// would be good to enhance isel to emit a loop for ctpop in this case.
//
// This could recognize common matrix multiplies and dot product idioms and
// replace them with calls to BLAS (if linked in??).
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LoopIdiomRecognize.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/LoopPassManager.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BuildLibCalls.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
using namespace llvm;

#define DEBUG_TYPE "loop-idiom"

STATISTIC(NumMemSet, "Number of memset's formed from loop stores");
STATISTIC(NumMemCpy, "Number of memcpy's formed from loop load+stores");

namespace {

class LoopIdiomRecognize {
  Loop *CurLoop;
  AliasAnalysis *AA;
  DominatorTree *DT;
  LoopInfo *LI;
  ScalarEvolution *SE;
  TargetLibraryInfo *TLI;
  const TargetTransformInfo *TTI;
  const DataLayout *DL;

public:
  explicit LoopIdiomRecognize(AliasAnalysis *AA, DominatorTree *DT,
                              LoopInfo *LI, ScalarEvolution *SE,
                              TargetLibraryInfo *TLI,
                              const TargetTransformInfo *TTI,
                              const DataLayout *DL)
      : CurLoop(nullptr), AA(AA), DT(DT), LI(LI), SE(SE), TLI(TLI), TTI(TTI),
        DL(DL) {}

  bool runOnLoop(Loop *L);

private:
  typedef SmallVector<StoreInst *, 8> StoreList;
  typedef MapVector<Value *, StoreList> StoreListMap;
  StoreListMap StoreRefsForMemset;
  StoreListMap StoreRefsForMemsetPattern;
  StoreList StoreRefsForMemcpy;
  bool HasMemset;
  bool HasMemsetPattern;
  bool HasMemcpy;

  /// \name Countable Loop Idiom Handling
  /// @{

  bool runOnCountableLoop();
  bool runOnLoopBlock(BasicBlock *BB, const SCEV *BECount,
                      SmallVectorImpl<BasicBlock *> &ExitBlocks);

  void collectStores(BasicBlock *BB);
  bool isLegalStore(StoreInst *SI, bool &ForMemset, bool &ForMemsetPattern,
                    bool &ForMemcpy);
  bool processLoopStores(SmallVectorImpl<StoreInst *> &SL, const SCEV *BECount,
                         bool ForMemset);
  bool processLoopMemSet(MemSetInst *MSI, const SCEV *BECount);

  bool processLoopStridedStore(Value *DestPtr, unsigned StoreSize,
                               unsigned StoreAlignment, Value *StoredVal,
                               Instruction *TheStore,
                               SmallPtrSetImpl<Instruction *> &Stores,
                               const SCEVAddRecExpr *Ev, const SCEV *BECount,
                               bool NegStride);
  bool processLoopStoreOfLoopLoad(StoreInst *SI, const SCEV *BECount);

  /// @}
  /// \name Noncountable Loop Idiom Handling
  /// @{

  bool runOnNoncountableLoop();

  bool recognizePopcount();
  void transformLoopToPopcount(BasicBlock *PreCondBB, Instruction *CntInst,
                               PHINode *CntPhi, Value *Var);

  /// @}
};

class LoopIdiomRecognizeLegacyPass : public LoopPass {
public:
  static char ID;
  explicit LoopIdiomRecognizeLegacyPass() : LoopPass(ID) {
    initializeLoopIdiomRecognizeLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override {
    if (skipLoop(L))
      return false;

    AliasAnalysis *AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
    DominatorTree *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    LoopInfo *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    ScalarEvolution *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    TargetLibraryInfo *TLI =
        &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
    const TargetTransformInfo *TTI =
        &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(
            *L->getHeader()->getParent());
    const DataLayout *DL = &L->getHeader()->getModule()->getDataLayout();

    LoopIdiomRecognize LIR(AA, DT, LI, SE, TLI, TTI, DL);
    return LIR.runOnLoop(L);
  }

  /// This transformation requires natural loop information & requires that
  /// loop preheaders be inserted into the CFG.
  ///
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
    getLoopAnalysisUsage(AU);
  }
};
} // End anonymous namespace.

PreservedAnalyses LoopIdiomRecognizePass::run(Loop &L,
                                              AnalysisManager<Loop> &AM) {
  const auto &FAM =
      AM.getResult<FunctionAnalysisManagerLoopProxy>(L).getManager();
  Function *F = L.getHeader()->getParent();

  // Use getCachedResult because Loop pass cannot trigger a function analysis.
  auto *AA = FAM.getCachedResult<AAManager>(*F);
  auto *DT = FAM.getCachedResult<DominatorTreeAnalysis>(*F);
  auto *LI = FAM.getCachedResult<LoopAnalysis>(*F);
  auto *SE = FAM.getCachedResult<ScalarEvolutionAnalysis>(*F);
  auto *TLI = FAM.getCachedResult<TargetLibraryAnalysis>(*F);
  const auto *TTI = FAM.getCachedResult<TargetIRAnalysis>(*F);
  const auto *DL = &L.getHeader()->getModule()->getDataLayout();
  assert((AA && DT && LI && SE && TLI && TTI && DL) &&
         "Analyses for Loop Idiom Recognition not available");

  LoopIdiomRecognize LIR(AA, DT, LI, SE, TLI, TTI, DL);
  if (!LIR.runOnLoop(&L))
    return PreservedAnalyses::all();

  return getLoopPassPreservedAnalyses();
}

char LoopIdiomRecognizeLegacyPass::ID = 0;
INITIALIZE_PASS_BEGIN(LoopIdiomRecognizeLegacyPass, "loop-idiom",
                      "Recognize loop idioms", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_END(LoopIdiomRecognizeLegacyPass, "loop-idiom",
                    "Recognize loop idioms", false, false)

Pass *llvm::createLoopIdiomPass() { return new LoopIdiomRecognizeLegacyPass(); }

static void deleteDeadInstruction(Instruction *I) {
  I->replaceAllUsesWith(UndefValue::get(I->getType()));
  I->eraseFromParent();
}

//===----------------------------------------------------------------------===//
//
//          Implementation of LoopIdiomRecognize
//
//===----------------------------------------------------------------------===//

bool LoopIdiomRecognize::runOnLoop(Loop *L) {
  CurLoop = L;
  // If the loop could not be converted to canonical form, it must have an
  // indirectbr in it, just give up.
  if (!L->getLoopPreheader())
    return false;

  // Disable loop idiom recognition if the function's name is a common idiom.
  StringRef Name = L->getHeader()->getParent()->getName();
  if (Name == "memset" || Name == "memcpy")
    return false;

  HasMemset = TLI->has(LibFunc::memset);
  HasMemsetPattern = TLI->has(LibFunc::memset_pattern16);
  HasMemcpy = TLI->has(LibFunc::memcpy);

  if (HasMemset || HasMemsetPattern || HasMemcpy)
    if (SE->hasLoopInvariantBackedgeTakenCount(L))
      return runOnCountableLoop();

  return runOnNoncountableLoop();
}

bool LoopIdiomRecognize::runOnCountableLoop() {
  const SCEV *BECount = SE->getBackedgeTakenCount(CurLoop);
  assert(!isa<SCEVCouldNotCompute>(BECount) &&
         "runOnCountableLoop() called on a loop without a predictable"
         "backedge-taken count");

  // If this loop executes exactly one time, then it should be peeled, not
  // optimized by this pass.
  if (const SCEVConstant *BECst = dyn_cast<SCEVConstant>(BECount))
    if (BECst->getAPInt() == 0)
      return false;

  SmallVector<BasicBlock *, 8> ExitBlocks;
  CurLoop->getUniqueExitBlocks(ExitBlocks);

  DEBUG(dbgs() << "loop-idiom Scanning: F["
               << CurLoop->getHeader()->getParent()->getName() << "] Loop %"
               << CurLoop->getHeader()->getName() << "\n");

  bool MadeChange = false;

  // The following transforms hoist stores/memsets into the loop pre-header.
  // Give up if the loop has instructions may throw.
  LoopSafetyInfo SafetyInfo;
  computeLoopSafetyInfo(&SafetyInfo, CurLoop);
  if (SafetyInfo.MayThrow)
    return MadeChange;

  // Scan all the blocks in the loop that are not in subloops.
  for (auto *BB : CurLoop->getBlocks()) {
    // Ignore blocks in subloops.
    if (LI->getLoopFor(BB) != CurLoop)
      continue;

    MadeChange |= runOnLoopBlock(BB, BECount, ExitBlocks);
  }
  return MadeChange;
}

static unsigned getStoreSizeInBytes(StoreInst *SI, const DataLayout *DL) {
  uint64_t SizeInBits = DL->getTypeSizeInBits(SI->getValueOperand()->getType());
  assert(((SizeInBits & 7) || (SizeInBits >> 32) == 0) &&
         "Don't overflow unsigned.");
  return (unsigned)SizeInBits >> 3;
}

static APInt getStoreStride(const SCEVAddRecExpr *StoreEv) {
  const SCEVConstant *ConstStride = cast<SCEVConstant>(StoreEv->getOperand(1));
  return ConstStride->getAPInt();
}

/// getMemSetPatternValue - If a strided store of the specified value is safe to
/// turn into a memset_pattern16, return a ConstantArray of 16 bytes that should
/// be passed in.  Otherwise, return null.
///
/// Note that we don't ever attempt to use memset_pattern8 or 4, because these
/// just replicate their input array and then pass on to memset_pattern16.
static Constant *getMemSetPatternValue(Value *V, const DataLayout *DL) {
  // If the value isn't a constant, we can't promote it to being in a constant
  // array.  We could theoretically do a store to an alloca or something, but
  // that doesn't seem worthwhile.
  Constant *C = dyn_cast<Constant>(V);
  if (!C)
    return nullptr;

  // Only handle simple values that are a power of two bytes in size.
  uint64_t Size = DL->getTypeSizeInBits(V->getType());
  if (Size == 0 || (Size & 7) || (Size & (Size - 1)))
    return nullptr;

  // Don't care enough about darwin/ppc to implement this.
  if (DL->isBigEndian())
    return nullptr;

  // Convert to size in bytes.
  Size /= 8;

  // TODO: If CI is larger than 16-bytes, we can try slicing it in half to see
  // if the top and bottom are the same (e.g. for vectors and large integers).
  if (Size > 16)
    return nullptr;

  // If the constant is exactly 16 bytes, just use it.
  if (Size == 16)
    return C;

  // Otherwise, we'll use an array of the constants.
  unsigned ArraySize = 16 / Size;
  ArrayType *AT = ArrayType::get(V->getType(), ArraySize);
  return ConstantArray::get(AT, std::vector<Constant *>(ArraySize, C));
}

bool LoopIdiomRecognize::isLegalStore(StoreInst *SI, bool &ForMemset,
                                      bool &ForMemsetPattern, bool &ForMemcpy) {
  // Don't touch volatile stores.
  if (!SI->isSimple())
    return false;

  // Avoid merging nontemporal stores.
  if (SI->getMetadata(LLVMContext::MD_nontemporal))
    return false;

  Value *StoredVal = SI->getValueOperand();
  Value *StorePtr = SI->getPointerOperand();

  // Reject stores that are so large that they overflow an unsigned.
  uint64_t SizeInBits = DL->getTypeSizeInBits(StoredVal->getType());
  if ((SizeInBits & 7) || (SizeInBits >> 32) != 0)
    return false;

  // See if the pointer expression is an AddRec like {base,+,1} on the current
  // loop, which indicates a strided store.  If we have something else, it's a
  // random store we can't handle.
  const SCEVAddRecExpr *StoreEv =
      dyn_cast<SCEVAddRecExpr>(SE->getSCEV(StorePtr));
  if (!StoreEv || StoreEv->getLoop() != CurLoop || !StoreEv->isAffine())
    return false;

  // Check to see if we have a constant stride.
  if (!isa<SCEVConstant>(StoreEv->getOperand(1)))
    return false;

  // See if the store can be turned into a memset.

  // If the stored value is a byte-wise value (like i32 -1), then it may be
  // turned into a memset of i8 -1, assuming that all the consecutive bytes
  // are stored.  A store of i32 0x01020304 can never be turned into a memset,
  // but it can be turned into memset_pattern if the target supports it.
  Value *SplatValue = isBytewiseValue(StoredVal);
  Constant *PatternValue = nullptr;

  // If we're allowed to form a memset, and the stored value would be
  // acceptable for memset, use it.
  if (HasMemset && SplatValue &&
      // Verify that the stored value is loop invariant.  If not, we can't
      // promote the memset.
      CurLoop->isLoopInvariant(SplatValue)) {
    // It looks like we can use SplatValue.
    ForMemset = true;
    return true;
  } else if (HasMemsetPattern &&
             // Don't create memset_pattern16s with address spaces.
             StorePtr->getType()->getPointerAddressSpace() == 0 &&
             (PatternValue = getMemSetPatternValue(StoredVal, DL))) {
    // It looks like we can use PatternValue!
    ForMemsetPattern = true;
    return true;
  }

  // Otherwise, see if the store can be turned into a memcpy.
  if (HasMemcpy) {
    // Check to see if the stride matches the size of the store.  If so, then we
    // know that every byte is touched in the loop.
    APInt Stride = getStoreStride(StoreEv);
    unsigned StoreSize = getStoreSizeInBytes(SI, DL);
    if (StoreSize != Stride && StoreSize != -Stride)
      return false;

    // The store must be feeding a non-volatile load.
    LoadInst *LI = dyn_cast<LoadInst>(SI->getValueOperand());
    if (!LI || !LI->isSimple())
      return false;

    // See if the pointer expression is an AddRec like {base,+,1} on the current
    // loop, which indicates a strided load.  If we have something else, it's a
    // random load we can't handle.
    const SCEVAddRecExpr *LoadEv =
        dyn_cast<SCEVAddRecExpr>(SE->getSCEV(LI->getPointerOperand()));
    if (!LoadEv || LoadEv->getLoop() != CurLoop || !LoadEv->isAffine())
      return false;

    // The store and load must share the same stride.
    if (StoreEv->getOperand(1) != LoadEv->getOperand(1))
      return false;

    // Success.  This store can be converted into a memcpy.
    ForMemcpy = true;
    return true;
  }
  // This store can't be transformed into a memset/memcpy.
  return false;
}

void LoopIdiomRecognize::collectStores(BasicBlock *BB) {
  StoreRefsForMemset.clear();
  StoreRefsForMemsetPattern.clear();
  StoreRefsForMemcpy.clear();
  for (Instruction &I : *BB) {
    StoreInst *SI = dyn_cast<StoreInst>(&I);
    if (!SI)
      continue;

    bool ForMemset = false;
    bool ForMemsetPattern = false;
    bool ForMemcpy = false;
    // Make sure this is a strided store with a constant stride.
    if (!isLegalStore(SI, ForMemset, ForMemsetPattern, ForMemcpy))
      continue;

    // Save the store locations.
    if (ForMemset) {
      // Find the base pointer.
      Value *Ptr = GetUnderlyingObject(SI->getPointerOperand(), *DL);
      StoreRefsForMemset[Ptr].push_back(SI);
    } else if (ForMemsetPattern) {
      // Find the base pointer.
      Value *Ptr = GetUnderlyingObject(SI->getPointerOperand(), *DL);
      StoreRefsForMemsetPattern[Ptr].push_back(SI);
    } else if (ForMemcpy)
      StoreRefsForMemcpy.push_back(SI);
  }
}

/// runOnLoopBlock - Process the specified block, which lives in a counted loop
/// with the specified backedge count.  This block is known to be in the current
/// loop and not in any subloops.
bool LoopIdiomRecognize::runOnLoopBlock(
    BasicBlock *BB, const SCEV *BECount,
    SmallVectorImpl<BasicBlock *> &ExitBlocks) {
  // We can only promote stores in this block if they are unconditionally
  // executed in the loop.  For a block to be unconditionally executed, it has
  // to dominate all the exit blocks of the loop.  Verify this now.
  for (unsigned i = 0, e = ExitBlocks.size(); i != e; ++i)
    if (!DT->dominates(BB, ExitBlocks[i]))
      return false;

  bool MadeChange = false;
  // Look for store instructions, which may be optimized to memset/memcpy.
  collectStores(BB);

  // Look for a single store or sets of stores with a common base, which can be
  // optimized into a memset (memset_pattern).  The latter most commonly happens
  // with structs and handunrolled loops.
  for (auto &SL : StoreRefsForMemset)
    MadeChange |= processLoopStores(SL.second, BECount, true);

  for (auto &SL : StoreRefsForMemsetPattern)
    MadeChange |= processLoopStores(SL.second, BECount, false);

  // Optimize the store into a memcpy, if it feeds an similarly strided load.
  for (auto &SI : StoreRefsForMemcpy)
    MadeChange |= processLoopStoreOfLoopLoad(SI, BECount);

  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E;) {
    Instruction *Inst = &*I++;
    // Look for memset instructions, which may be optimized to a larger memset.
    if (MemSetInst *MSI = dyn_cast<MemSetInst>(Inst)) {
      WeakVH InstPtr(&*I);
      if (!processLoopMemSet(MSI, BECount))
        continue;
      MadeChange = true;

      // If processing the memset invalidated our iterator, start over from the
      // top of the block.
      if (!InstPtr)
        I = BB->begin();
      continue;
    }
  }

  return MadeChange;
}

/// processLoopStores - See if this store(s) can be promoted to a memset.
bool LoopIdiomRecognize::processLoopStores(SmallVectorImpl<StoreInst *> &SL,
                                           const SCEV *BECount,
                                           bool ForMemset) {
  // Try to find consecutive stores that can be transformed into memsets.
  SetVector<StoreInst *> Heads, Tails;
  SmallDenseMap<StoreInst *, StoreInst *> ConsecutiveChain;

  // Do a quadratic search on all of the given stores and find
  // all of the pairs of stores that follow each other.
  SmallVector<unsigned, 16> IndexQueue;
  for (unsigned i = 0, e = SL.size(); i < e; ++i) {
    assert(SL[i]->isSimple() && "Expected only non-volatile stores.");

    Value *FirstStoredVal = SL[i]->getValueOperand();
    Value *FirstStorePtr = SL[i]->getPointerOperand();
    const SCEVAddRecExpr *FirstStoreEv =
        cast<SCEVAddRecExpr>(SE->getSCEV(FirstStorePtr));
    APInt FirstStride = getStoreStride(FirstStoreEv);
    unsigned FirstStoreSize = getStoreSizeInBytes(SL[i], DL);

    // See if we can optimize just this store in isolation.
    if (FirstStride == FirstStoreSize || -FirstStride == FirstStoreSize) {
      Heads.insert(SL[i]);
      continue;
    }

    Value *FirstSplatValue = nullptr;
    Constant *FirstPatternValue = nullptr;

    if (ForMemset)
      FirstSplatValue = isBytewiseValue(FirstStoredVal);
    else
      FirstPatternValue = getMemSetPatternValue(FirstStoredVal, DL);

    assert((FirstSplatValue || FirstPatternValue) &&
           "Expected either splat value or pattern value.");

    IndexQueue.clear();
    // If a store has multiple consecutive store candidates, search Stores
    // array according to the sequence: from i+1 to e, then from i-1 to 0.
    // This is because usually pairing with immediate succeeding or preceding
    // candidate create the best chance to find memset opportunity.
    unsigned j = 0;
    for (j = i + 1; j < e; ++j)
      IndexQueue.push_back(j);
    for (j = i; j > 0; --j)
      IndexQueue.push_back(j - 1);

    for (auto &k : IndexQueue) {
      assert(SL[k]->isSimple() && "Expected only non-volatile stores.");
      Value *SecondStorePtr = SL[k]->getPointerOperand();
      const SCEVAddRecExpr *SecondStoreEv =
          cast<SCEVAddRecExpr>(SE->getSCEV(SecondStorePtr));
      APInt SecondStride = getStoreStride(SecondStoreEv);

      if (FirstStride != SecondStride)
        continue;

      Value *SecondStoredVal = SL[k]->getValueOperand();
      Value *SecondSplatValue = nullptr;
      Constant *SecondPatternValue = nullptr;

      if (ForMemset)
        SecondSplatValue = isBytewiseValue(SecondStoredVal);
      else
        SecondPatternValue = getMemSetPatternValue(SecondStoredVal, DL);

      assert((SecondSplatValue || SecondPatternValue) &&
             "Expected either splat value or pattern value.");

      if (isConsecutiveAccess(SL[i], SL[k], *DL, *SE, false)) {
        if (ForMemset) {
          if (FirstSplatValue != SecondSplatValue)
            continue;
        } else {
          if (FirstPatternValue != SecondPatternValue)
            continue;
        }
        Tails.insert(SL[k]);
        Heads.insert(SL[i]);
        ConsecutiveChain[SL[i]] = SL[k];
        break;
      }
    }
  }

  // We may run into multiple chains that merge into a single chain. We mark the
  // stores that we transformed so that we don't visit the same store twice.
  SmallPtrSet<Value *, 16> TransformedStores;
  bool Changed = false;

  // For stores that start but don't end a link in the chain:
  for (SetVector<StoreInst *>::iterator it = Heads.begin(), e = Heads.end();
       it != e; ++it) {
    if (Tails.count(*it))
      continue;

    // We found a store instr that starts a chain. Now follow the chain and try
    // to transform it.
    SmallPtrSet<Instruction *, 8> AdjacentStores;
    StoreInst *I = *it;

    StoreInst *HeadStore = I;
    unsigned StoreSize = 0;

    // Collect the chain into a list.
    while (Tails.count(I) || Heads.count(I)) {
      if (TransformedStores.count(I))
        break;
      AdjacentStores.insert(I);

      StoreSize += getStoreSizeInBytes(I, DL);
      // Move to the next value in the chain.
      I = ConsecutiveChain[I];
    }

    Value *StoredVal = HeadStore->getValueOperand();
    Value *StorePtr = HeadStore->getPointerOperand();
    const SCEVAddRecExpr *StoreEv = cast<SCEVAddRecExpr>(SE->getSCEV(StorePtr));
    APInt Stride = getStoreStride(StoreEv);

    // Check to see if the stride matches the size of the stores.  If so, then
    // we know that every byte is touched in the loop.
    if (StoreSize != Stride && StoreSize != -Stride)
      continue;

    bool NegStride = StoreSize == -Stride;

    if (processLoopStridedStore(StorePtr, StoreSize, HeadStore->getAlignment(),
                                StoredVal, HeadStore, AdjacentStores, StoreEv,
                                BECount, NegStride)) {
      TransformedStores.insert(AdjacentStores.begin(), AdjacentStores.end());
      Changed = true;
    }
  }

  return Changed;
}

/// processLoopMemSet - See if this memset can be promoted to a large memset.
bool LoopIdiomRecognize::processLoopMemSet(MemSetInst *MSI,
                                           const SCEV *BECount) {
  // We can only handle non-volatile memsets with a constant size.
  if (MSI->isVolatile() || !isa<ConstantInt>(MSI->getLength()))
    return false;

  // If we're not allowed to hack on memset, we fail.
  if (!HasMemset)
    return false;

  Value *Pointer = MSI->getDest();

  // See if the pointer expression is an AddRec like {base,+,1} on the current
  // loop, which indicates a strided store.  If we have something else, it's a
  // random store we can't handle.
  const SCEVAddRecExpr *Ev = dyn_cast<SCEVAddRecExpr>(SE->getSCEV(Pointer));
  if (!Ev || Ev->getLoop() != CurLoop || !Ev->isAffine())
    return false;

  // Reject memsets that are so large that they overflow an unsigned.
  uint64_t SizeInBytes = cast<ConstantInt>(MSI->getLength())->getZExtValue();
  if ((SizeInBytes >> 32) != 0)
    return false;

  // Check to see if the stride matches the size of the memset.  If so, then we
  // know that every byte is touched in the loop.
  const SCEVConstant *ConstStride = dyn_cast<SCEVConstant>(Ev->getOperand(1));
  if (!ConstStride)
    return false;

  APInt Stride = ConstStride->getAPInt();
  if (SizeInBytes != Stride && SizeInBytes != -Stride)
    return false;

  // Verify that the memset value is loop invariant.  If not, we can't promote
  // the memset.
  Value *SplatValue = MSI->getValue();
  if (!SplatValue || !CurLoop->isLoopInvariant(SplatValue))
    return false;

  SmallPtrSet<Instruction *, 1> MSIs;
  MSIs.insert(MSI);
  bool NegStride = SizeInBytes == -Stride;
  return processLoopStridedStore(Pointer, (unsigned)SizeInBytes,
                                 MSI->getAlignment(), SplatValue, MSI, MSIs, Ev,
                                 BECount, NegStride);
}

/// mayLoopAccessLocation - Return true if the specified loop might access the
/// specified pointer location, which is a loop-strided access.  The 'Access'
/// argument specifies what the verboten forms of access are (read or write).
static bool
mayLoopAccessLocation(Value *Ptr, ModRefInfo Access, Loop *L,
                      const SCEV *BECount, unsigned StoreSize,
                      AliasAnalysis &AA,
                      SmallPtrSetImpl<Instruction *> &IgnoredStores) {
  // Get the location that may be stored across the loop.  Since the access is
  // strided positively through memory, we say that the modified location starts
  // at the pointer and has infinite size.
  uint64_t AccessSize = MemoryLocation::UnknownSize;

  // If the loop iterates a fixed number of times, we can refine the access size
  // to be exactly the size of the memset, which is (BECount+1)*StoreSize
  if (const SCEVConstant *BECst = dyn_cast<SCEVConstant>(BECount))
    AccessSize = (BECst->getValue()->getZExtValue() + 1) * StoreSize;

  // TODO: For this to be really effective, we have to dive into the pointer
  // operand in the store.  Store to &A[i] of 100 will always return may alias
  // with store of &A[100], we need to StoreLoc to be "A" with size of 100,
  // which will then no-alias a store to &A[100].
  MemoryLocation StoreLoc(Ptr, AccessSize);

  for (Loop::block_iterator BI = L->block_begin(), E = L->block_end(); BI != E;
       ++BI)
    for (Instruction &I : **BI)
      if (IgnoredStores.count(&I) == 0 &&
          (AA.getModRefInfo(&I, StoreLoc) & Access))
        return true;

  return false;
}

// If we have a negative stride, Start refers to the end of the memory location
// we're trying to memset.  Therefore, we need to recompute the base pointer,
// which is just Start - BECount*Size.
static const SCEV *getStartForNegStride(const SCEV *Start, const SCEV *BECount,
                                        Type *IntPtr, unsigned StoreSize,
                                        ScalarEvolution *SE) {
  const SCEV *Index = SE->getTruncateOrZeroExtend(BECount, IntPtr);
  if (StoreSize != 1)
    Index = SE->getMulExpr(Index, SE->getConstant(IntPtr, StoreSize),
                           SCEV::FlagNUW);
  return SE->getMinusSCEV(Start, Index);
}

/// processLoopStridedStore - We see a strided store of some value.  If we can
/// transform this into a memset or memset_pattern in the loop preheader, do so.
bool LoopIdiomRecognize::processLoopStridedStore(
    Value *DestPtr, unsigned StoreSize, unsigned StoreAlignment,
    Value *StoredVal, Instruction *TheStore,
    SmallPtrSetImpl<Instruction *> &Stores, const SCEVAddRecExpr *Ev,
    const SCEV *BECount, bool NegStride) {
  Value *SplatValue = isBytewiseValue(StoredVal);
  Constant *PatternValue = nullptr;

  if (!SplatValue)
    PatternValue = getMemSetPatternValue(StoredVal, DL);

  assert((SplatValue || PatternValue) &&
         "Expected either splat value or pattern value.");

  // The trip count of the loop and the base pointer of the addrec SCEV is
  // guaranteed to be loop invariant, which means that it should dominate the
  // header.  This allows us to insert code for it in the preheader.
  unsigned DestAS = DestPtr->getType()->getPointerAddressSpace();
  BasicBlock *Preheader = CurLoop->getLoopPreheader();
  IRBuilder<> Builder(Preheader->getTerminator());
  SCEVExpander Expander(*SE, *DL, "loop-idiom");

  Type *DestInt8PtrTy = Builder.getInt8PtrTy(DestAS);
  Type *IntPtr = Builder.getIntPtrTy(*DL, DestAS);

  const SCEV *Start = Ev->getStart();
  // Handle negative strided loops.
  if (NegStride)
    Start = getStartForNegStride(Start, BECount, IntPtr, StoreSize, SE);

  // Okay, we have a strided store "p[i]" of a splattable value.  We can turn
  // this into a memset in the loop preheader now if we want.  However, this
  // would be unsafe to do if there is anything else in the loop that may read
  // or write to the aliased location.  Check for any overlap by generating the
  // base pointer and checking the region.
  Value *BasePtr =
      Expander.expandCodeFor(Start, DestInt8PtrTy, Preheader->getTerminator());
  if (mayLoopAccessLocation(BasePtr, MRI_ModRef, CurLoop, BECount, StoreSize,
                            *AA, Stores)) {
    Expander.clear();
    // If we generated new code for the base pointer, clean up.
    RecursivelyDeleteTriviallyDeadInstructions(BasePtr, TLI);
    return false;
  }

  // Okay, everything looks good, insert the memset.

  // The # stored bytes is (BECount+1)*Size.  Expand the trip count out to
  // pointer size if it isn't already.
  BECount = SE->getTruncateOrZeroExtend(BECount, IntPtr);

  const SCEV *NumBytesS =
      SE->getAddExpr(BECount, SE->getOne(IntPtr), SCEV::FlagNUW);
  if (StoreSize != 1) {
    NumBytesS = SE->getMulExpr(NumBytesS, SE->getConstant(IntPtr, StoreSize),
                               SCEV::FlagNUW);
  }

  Value *NumBytes =
      Expander.expandCodeFor(NumBytesS, IntPtr, Preheader->getTerminator());

  CallInst *NewCall;
  if (SplatValue) {
    NewCall =
        Builder.CreateMemSet(BasePtr, SplatValue, NumBytes, StoreAlignment);
  } else {
    // Everything is emitted in default address space
    Type *Int8PtrTy = DestInt8PtrTy;

    Module *M = TheStore->getModule();
    Value *MSP =
        M->getOrInsertFunction("memset_pattern16", Builder.getVoidTy(),
                               Int8PtrTy, Int8PtrTy, IntPtr, (void *)nullptr);
    inferLibFuncAttributes(*M->getFunction("memset_pattern16"), *TLI);

    // Otherwise we should form a memset_pattern16.  PatternValue is known to be
    // an constant array of 16-bytes.  Plop the value into a mergable global.
    GlobalVariable *GV = new GlobalVariable(*M, PatternValue->getType(), true,
                                            GlobalValue::PrivateLinkage,
                                            PatternValue, ".memset_pattern");
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global); // Ok to merge these.
    GV->setAlignment(16);
    Value *PatternPtr = ConstantExpr::getBitCast(GV, Int8PtrTy);
    NewCall = Builder.CreateCall(MSP, {BasePtr, PatternPtr, NumBytes});
  }

  DEBUG(dbgs() << "  Formed memset: " << *NewCall << "\n"
               << "    from store to: " << *Ev << " at: " << *TheStore << "\n");
  NewCall->setDebugLoc(TheStore->getDebugLoc());

  // Okay, the memset has been formed.  Zap the original store and anything that
  // feeds into it.
  for (auto *I : Stores)
    deleteDeadInstruction(I);
  ++NumMemSet;
  return true;
}

/// If the stored value is a strided load in the same loop with the same stride
/// this may be transformable into a memcpy.  This kicks in for stuff like
///   for (i) A[i] = B[i];
bool LoopIdiomRecognize::processLoopStoreOfLoopLoad(StoreInst *SI,
                                                    const SCEV *BECount) {
  assert(SI->isSimple() && "Expected only non-volatile stores.");

  Value *StorePtr = SI->getPointerOperand();
  const SCEVAddRecExpr *StoreEv = cast<SCEVAddRecExpr>(SE->getSCEV(StorePtr));
  APInt Stride = getStoreStride(StoreEv);
  unsigned StoreSize = getStoreSizeInBytes(SI, DL);
  bool NegStride = StoreSize == -Stride;

  // The store must be feeding a non-volatile load.
  LoadInst *LI = cast<LoadInst>(SI->getValueOperand());
  assert(LI->isSimple() && "Expected only non-volatile stores.");

  // See if the pointer expression is an AddRec like {base,+,1} on the current
  // loop, which indicates a strided load.  If we have something else, it's a
  // random load we can't handle.
  const SCEVAddRecExpr *LoadEv =
      cast<SCEVAddRecExpr>(SE->getSCEV(LI->getPointerOperand()));

  // The trip count of the loop and the base pointer of the addrec SCEV is
  // guaranteed to be loop invariant, which means that it should dominate the
  // header.  This allows us to insert code for it in the preheader.
  BasicBlock *Preheader = CurLoop->getLoopPreheader();
  IRBuilder<> Builder(Preheader->getTerminator());
  SCEVExpander Expander(*SE, *DL, "loop-idiom");

  const SCEV *StrStart = StoreEv->getStart();
  unsigned StrAS = SI->getPointerAddressSpace();
  Type *IntPtrTy = Builder.getIntPtrTy(*DL, StrAS);

  // Handle negative strided loops.
  if (NegStride)
    StrStart = getStartForNegStride(StrStart, BECount, IntPtrTy, StoreSize, SE);

  // Okay, we have a strided store "p[i]" of a loaded value.  We can turn
  // this into a memcpy in the loop preheader now if we want.  However, this
  // would be unsafe to do if there is anything else in the loop that may read
  // or write the memory region we're storing to.  This includes the load that
  // feeds the stores.  Check for an alias by generating the base address and
  // checking everything.
  Value *StoreBasePtr = Expander.expandCodeFor(
      StrStart, Builder.getInt8PtrTy(StrAS), Preheader->getTerminator());

  SmallPtrSet<Instruction *, 1> Stores;
  Stores.insert(SI);
  if (mayLoopAccessLocation(StoreBasePtr, MRI_ModRef, CurLoop, BECount,
                            StoreSize, *AA, Stores)) {
    Expander.clear();
    // If we generated new code for the base pointer, clean up.
    RecursivelyDeleteTriviallyDeadInstructions(StoreBasePtr, TLI);
    return false;
  }

  const SCEV *LdStart = LoadEv->getStart();
  unsigned LdAS = LI->getPointerAddressSpace();

  // Handle negative strided loops.
  if (NegStride)
    LdStart = getStartForNegStride(LdStart, BECount, IntPtrTy, StoreSize, SE);

  // For a memcpy, we have to make sure that the input array is not being
  // mutated by the loop.
  Value *LoadBasePtr = Expander.expandCodeFor(
      LdStart, Builder.getInt8PtrTy(LdAS), Preheader->getTerminator());

  if (mayLoopAccessLocation(LoadBasePtr, MRI_Mod, CurLoop, BECount, StoreSize,
                            *AA, Stores)) {
    Expander.clear();
    // If we generated new code for the base pointer, clean up.
    RecursivelyDeleteTriviallyDeadInstructions(LoadBasePtr, TLI);
    RecursivelyDeleteTriviallyDeadInstructions(StoreBasePtr, TLI);
    return false;
  }

  // Okay, everything is safe, we can transform this!

  // The # stored bytes is (BECount+1)*Size.  Expand the trip count out to
  // pointer size if it isn't already.
  BECount = SE->getTruncateOrZeroExtend(BECount, IntPtrTy);

  const SCEV *NumBytesS =
      SE->getAddExpr(BECount, SE->getOne(IntPtrTy), SCEV::FlagNUW);
  if (StoreSize != 1)
    NumBytesS = SE->getMulExpr(NumBytesS, SE->getConstant(IntPtrTy, StoreSize),
                               SCEV::FlagNUW);

  Value *NumBytes =
      Expander.expandCodeFor(NumBytesS, IntPtrTy, Preheader->getTerminator());

  CallInst *NewCall =
      Builder.CreateMemCpy(StoreBasePtr, LoadBasePtr, NumBytes,
                           std::min(SI->getAlignment(), LI->getAlignment()));
  NewCall->setDebugLoc(SI->getDebugLoc());

  DEBUG(dbgs() << "  Formed memcpy: " << *NewCall << "\n"
               << "    from load ptr=" << *LoadEv << " at: " << *LI << "\n"
               << "    from store ptr=" << *StoreEv << " at: " << *SI << "\n");

  // Okay, the memcpy has been formed.  Zap the original store and anything that
  // feeds into it.
  deleteDeadInstruction(SI);
  ++NumMemCpy;
  return true;
}

bool LoopIdiomRecognize::runOnNoncountableLoop() {
  return recognizePopcount();
}

/// Check if the given conditional branch is based on the comparison between
/// a variable and zero, and if the variable is non-zero, the control yields to
/// the loop entry. If the branch matches the behavior, the variable involved
/// in the comparion is returned. This function will be called to see if the
/// precondition and postcondition of the loop are in desirable form.
static Value *matchCondition(BranchInst *BI, BasicBlock *LoopEntry) {
  if (!BI || !BI->isConditional())
    return nullptr;

  ICmpInst *Cond = dyn_cast<ICmpInst>(BI->getCondition());
  if (!Cond)
    return nullptr;

  ConstantInt *CmpZero = dyn_cast<ConstantInt>(Cond->getOperand(1));
  if (!CmpZero || !CmpZero->isZero())
    return nullptr;

  ICmpInst::Predicate Pred = Cond->getPredicate();
  if ((Pred == ICmpInst::ICMP_NE && BI->getSuccessor(0) == LoopEntry) ||
      (Pred == ICmpInst::ICMP_EQ && BI->getSuccessor(1) == LoopEntry))
    return Cond->getOperand(0);

  return nullptr;
}

/// Return true iff the idiom is detected in the loop.
///
/// Additionally:
/// 1) \p CntInst is set to the instruction counting the population bit.
/// 2) \p CntPhi is set to the corresponding phi node.
/// 3) \p Var is set to the value whose population bits are being counted.
///
/// The core idiom we are trying to detect is:
/// \code
///    if (x0 != 0)
///      goto loop-exit // the precondition of the loop
///    cnt0 = init-val;
///    do {
///       x1 = phi (x0, x2);
///       cnt1 = phi(cnt0, cnt2);
///
///       cnt2 = cnt1 + 1;
///        ...
///       x2 = x1 & (x1 - 1);
///        ...
///    } while(x != 0);
///
/// loop-exit:
/// \endcode
static bool detectPopcountIdiom(Loop *CurLoop, BasicBlock *PreCondBB,
                                Instruction *&CntInst, PHINode *&CntPhi,
                                Value *&Var) {
  // step 1: Check to see if the look-back branch match this pattern:
  //    "if (a!=0) goto loop-entry".
  BasicBlock *LoopEntry;
  Instruction *DefX2, *CountInst;
  Value *VarX1, *VarX0;
  PHINode *PhiX, *CountPhi;

  DefX2 = CountInst = nullptr;
  VarX1 = VarX0 = nullptr;
  PhiX = CountPhi = nullptr;
  LoopEntry = *(CurLoop->block_begin());

  // step 1: Check if the loop-back branch is in desirable form.
  {
    if (Value *T = matchCondition(
            dyn_cast<BranchInst>(LoopEntry->getTerminator()), LoopEntry))
      DefX2 = dyn_cast<Instruction>(T);
    else
      return false;
  }

  // step 2: detect instructions corresponding to "x2 = x1 & (x1 - 1)"
  {
    if (!DefX2 || DefX2->getOpcode() != Instruction::And)
      return false;

    BinaryOperator *SubOneOp;

    if ((SubOneOp = dyn_cast<BinaryOperator>(DefX2->getOperand(0))))
      VarX1 = DefX2->getOperand(1);
    else {
      VarX1 = DefX2->getOperand(0);
      SubOneOp = dyn_cast<BinaryOperator>(DefX2->getOperand(1));
    }
    if (!SubOneOp)
      return false;

    Instruction *SubInst = cast<Instruction>(SubOneOp);
    ConstantInt *Dec = dyn_cast<ConstantInt>(SubInst->getOperand(1));
    if (!Dec ||
        !((SubInst->getOpcode() == Instruction::Sub && Dec->isOne()) ||
          (SubInst->getOpcode() == Instruction::Add &&
           Dec->isAllOnesValue()))) {
      return false;
    }
  }

  // step 3: Check the recurrence of variable X
  {
    PhiX = dyn_cast<PHINode>(VarX1);
    if (!PhiX ||
        (PhiX->getOperand(0) != DefX2 && PhiX->getOperand(1) != DefX2)) {
      return false;
    }
  }

  // step 4: Find the instruction which count the population: cnt2 = cnt1 + 1
  {
    CountInst = nullptr;
    for (BasicBlock::iterator Iter = LoopEntry->getFirstNonPHI()->getIterator(),
                              IterE = LoopEntry->end();
         Iter != IterE; Iter++) {
      Instruction *Inst = &*Iter;
      if (Inst->getOpcode() != Instruction::Add)
        continue;

      ConstantInt *Inc = dyn_cast<ConstantInt>(Inst->getOperand(1));
      if (!Inc || !Inc->isOne())
        continue;

      PHINode *Phi = dyn_cast<PHINode>(Inst->getOperand(0));
      if (!Phi || Phi->getParent() != LoopEntry)
        continue;

      // Check if the result of the instruction is live of the loop.
      bool LiveOutLoop = false;
      for (User *U : Inst->users()) {
        if ((cast<Instruction>(U))->getParent() != LoopEntry) {
          LiveOutLoop = true;
          break;
        }
      }

      if (LiveOutLoop) {
        CountInst = Inst;
        CountPhi = Phi;
        break;
      }
    }

    if (!CountInst)
      return false;
  }

  // step 5: check if the precondition is in this form:
  //   "if (x != 0) goto loop-head ; else goto somewhere-we-don't-care;"
  {
    auto *PreCondBr = dyn_cast<BranchInst>(PreCondBB->getTerminator());
    Value *T = matchCondition(PreCondBr, CurLoop->getLoopPreheader());
    if (T != PhiX->getOperand(0) && T != PhiX->getOperand(1))
      return false;

    CntInst = CountInst;
    CntPhi = CountPhi;
    Var = T;
  }

  return true;
}

/// Recognizes a population count idiom in a non-countable loop.
///
/// If detected, transforms the relevant code to issue the popcount intrinsic
/// function call, and returns true; otherwise, returns false.
bool LoopIdiomRecognize::recognizePopcount() {
  if (TTI->getPopcntSupport(32) != TargetTransformInfo::PSK_FastHardware)
    return false;

  // Counting population are usually conducted by few arithmetic instructions.
  // Such instructions can be easily "absorbed" by vacant slots in a
  // non-compact loop. Therefore, recognizing popcount idiom only makes sense
  // in a compact loop.

  // Give up if the loop has multiple blocks or multiple backedges.
  if (CurLoop->getNumBackEdges() != 1 || CurLoop->getNumBlocks() != 1)
    return false;

  BasicBlock *LoopBody = *(CurLoop->block_begin());
  if (LoopBody->size() >= 20) {
    // The loop is too big, bail out.
    return false;
  }

  // It should have a preheader containing nothing but an unconditional branch.
  BasicBlock *PH = CurLoop->getLoopPreheader();
  if (!PH)
    return false;
  if (&PH->front() != PH->getTerminator())
    return false;
  auto *EntryBI = dyn_cast<BranchInst>(PH->getTerminator());
  if (!EntryBI || EntryBI->isConditional())
    return false;

  // It should have a precondition block where the generated popcount instrinsic
  // function can be inserted.
  auto *PreCondBB = PH->getSinglePredecessor();
  if (!PreCondBB)
    return false;
  auto *PreCondBI = dyn_cast<BranchInst>(PreCondBB->getTerminator());
  if (!PreCondBI || PreCondBI->isUnconditional())
    return false;

  Instruction *CntInst;
  PHINode *CntPhi;
  Value *Val;
  if (!detectPopcountIdiom(CurLoop, PreCondBB, CntInst, CntPhi, Val))
    return false;

  transformLoopToPopcount(PreCondBB, CntInst, CntPhi, Val);
  return true;
}

static CallInst *createPopcntIntrinsic(IRBuilder<> &IRBuilder, Value *Val,
                                       const DebugLoc &DL) {
  Value *Ops[] = {Val};
  Type *Tys[] = {Val->getType()};

  Module *M = IRBuilder.GetInsertBlock()->getParent()->getParent();
  Value *Func = Intrinsic::getDeclaration(M, Intrinsic::ctpop, Tys);
  CallInst *CI = IRBuilder.CreateCall(Func, Ops);
  CI->setDebugLoc(DL);

  return CI;
}

void LoopIdiomRecognize::transformLoopToPopcount(BasicBlock *PreCondBB,
                                                 Instruction *CntInst,
                                                 PHINode *CntPhi, Value *Var) {
  BasicBlock *PreHead = CurLoop->getLoopPreheader();
  auto *PreCondBr = dyn_cast<BranchInst>(PreCondBB->getTerminator());
  const DebugLoc DL = CntInst->getDebugLoc();

  // Assuming before transformation, the loop is following:
  //  if (x) // the precondition
  //     do { cnt++; x &= x - 1; } while(x);

  // Step 1: Insert the ctpop instruction at the end of the precondition block
  IRBuilder<> Builder(PreCondBr);
  Value *PopCnt, *PopCntZext, *NewCount, *TripCnt;
  {
    PopCnt = createPopcntIntrinsic(Builder, Var, DL);
    NewCount = PopCntZext =
        Builder.CreateZExtOrTrunc(PopCnt, cast<IntegerType>(CntPhi->getType()));

    if (NewCount != PopCnt)
      (cast<Instruction>(NewCount))->setDebugLoc(DL);

    // TripCnt is exactly the number of iterations the loop has
    TripCnt = NewCount;

    // If the population counter's initial value is not zero, insert Add Inst.
    Value *CntInitVal = CntPhi->getIncomingValueForBlock(PreHead);
    ConstantInt *InitConst = dyn_cast<ConstantInt>(CntInitVal);
    if (!InitConst || !InitConst->isZero()) {
      NewCount = Builder.CreateAdd(NewCount, CntInitVal);
      (cast<Instruction>(NewCount))->setDebugLoc(DL);
    }
  }

  // Step 2: Replace the precondition from "if (x == 0) goto loop-exit" to
  //   "if (NewCount == 0) loop-exit". Without this change, the intrinsic
  //   function would be partial dead code, and downstream passes will drag
  //   it back from the precondition block to the preheader.
  {
    ICmpInst *PreCond = cast<ICmpInst>(PreCondBr->getCondition());

    Value *Opnd0 = PopCntZext;
    Value *Opnd1 = ConstantInt::get(PopCntZext->getType(), 0);
    if (PreCond->getOperand(0) != Var)
      std::swap(Opnd0, Opnd1);

    ICmpInst *NewPreCond = cast<ICmpInst>(
        Builder.CreateICmp(PreCond->getPredicate(), Opnd0, Opnd1));
    PreCondBr->setCondition(NewPreCond);

    RecursivelyDeleteTriviallyDeadInstructions(PreCond, TLI);
  }

  // Step 3: Note that the population count is exactly the trip count of the
  // loop in question, which enable us to to convert the loop from noncountable
  // loop into a countable one. The benefit is twofold:
  //
  //  - If the loop only counts population, the entire loop becomes dead after
  //    the transformation. It is a lot easier to prove a countable loop dead
  //    than to prove a noncountable one. (In some C dialects, an infinite loop
  //    isn't dead even if it computes nothing useful. In general, DCE needs
  //    to prove a noncountable loop finite before safely delete it.)
  //
  //  - If the loop also performs something else, it remains alive.
  //    Since it is transformed to countable form, it can be aggressively
  //    optimized by some optimizations which are in general not applicable
  //    to a noncountable loop.
  //
  // After this step, this loop (conceptually) would look like following:
  //   newcnt = __builtin_ctpop(x);
  //   t = newcnt;
  //   if (x)
  //     do { cnt++; x &= x-1; t--) } while (t > 0);
  BasicBlock *Body = *(CurLoop->block_begin());
  {
    auto *LbBr = dyn_cast<BranchInst>(Body->getTerminator());
    ICmpInst *LbCond = cast<ICmpInst>(LbBr->getCondition());
    Type *Ty = TripCnt->getType();

    PHINode *TcPhi = PHINode::Create(Ty, 2, "tcphi", &Body->front());

    Builder.SetInsertPoint(LbCond);
    Instruction *TcDec = cast<Instruction>(
        Builder.CreateSub(TcPhi, ConstantInt::get(Ty, 1),
                          "tcdec", false, true));

    TcPhi->addIncoming(TripCnt, PreHead);
    TcPhi->addIncoming(TcDec, Body);

    CmpInst::Predicate Pred =
        (LbBr->getSuccessor(0) == Body) ? CmpInst::ICMP_UGT : CmpInst::ICMP_SLE;
    LbCond->setPredicate(Pred);
    LbCond->setOperand(0, TcDec);
    LbCond->setOperand(1, ConstantInt::get(Ty, 0));
  }

  // Step 4: All the references to the original population counter outside
  //  the loop are replaced with the NewCount -- the value returned from
  //  __builtin_ctpop().
  CntInst->replaceUsesOutsideBlock(NewCount, Body);

  // step 5: Forget the "non-computable" trip-count SCEV associated with the
  //   loop. The loop would otherwise not be deleted even if it becomes empty.
  SE->forgetLoop(CurLoop);
}
