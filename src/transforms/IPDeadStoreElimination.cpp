/* 
   Inter-procedural Dead Store Elimination.

   Consider only global variables whose addresses have not been taken.

   1. Run seadsa ShadowMem pass to instrument code with shadow.mem
      function calls.
   2. Follow inter-procedural def-use chains to check if a store to a
      singleton global variable has no use. If yes, the store is dead
      and it can be removed.
   3. Remove shadow.mem function calls.

*/

#include "analysis/MemorySSA.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"

#include "sea_dsa/ShadowMem.hh"

#include <boost/functional/hash.hpp>
#include <boost/unordered_set.hpp>

// For now, only singleton global variables.
// TODO: we can also support regions that contain single types if they
// are always fully accessed.
static llvm::cl::opt<bool>
OnlySingleton("ip-dse-only-singleton",
   llvm::cl::desc("IP DSE: remove store only if operand is a singleton global var"),
   llvm::cl::Hidden,
   llvm::cl::init(true));

static llvm::cl::opt<unsigned>
MaxLenDefUse("ip-dse-max-def-use",
   llvm::cl::desc("IP DSE: maximum length of the def-use chain"),
   llvm::cl::Hidden,
   llvm::cl::init(UINT_MAX));

//#define DSE_LOG(...) __VA_ARGS__
#define DSE_LOG(...)

namespace previrt {
namespace transforms {

using namespace llvm;
using namespace analysis;
  
static bool hasFunctionPtrParam(Function* F) {
  FunctionType* FTy = F->getFunctionType();
  for(unsigned i=0, e=FTy->getNumParams(); i<e; ++i) {
    if (PointerType* PT = dyn_cast<PointerType>(FTy->getParamType(i))) {
      if (isa<FunctionType>(PT->getElementType())) {
	return true;
      }
    }
  }
  return false;
}
  
class IPDeadStoreElimination: public ModulePass {
  
  struct QueueElem {
    const Instruction *shadow_mem_inst;
    StoreInst *store_inst;
    // number of steps between store_inst and shadow_mem_inst
    unsigned length;
    
    QueueElem(const Instruction *inst, StoreInst *si, unsigned len)
      : shadow_mem_inst(inst), store_inst(si), length(len) { }
    
    size_t hash() const {
      size_t val = 0;
      boost::hash_combine(val, shadow_mem_inst);
      boost::hash_combine(val, store_inst);
      return val;
    }
    
    bool operator==(const QueueElem& o) const {
      return (shadow_mem_inst == o.shadow_mem_inst &&
	      store_inst == o.store_inst);
    }
    
    void write(raw_ostream &o) const {
      o << "(" << *shadow_mem_inst << ", " << *store_inst << ")";
    }
    
    friend raw_ostream& operator<<(raw_ostream &o, const QueueElem &e) {
      e.write(o);
      return o;
    }
  };

  struct QueueElemHasher {
    size_t operator()(const QueueElem& e) const {
      return e.hash();
    }
  };
  
  // Map a store instruction into a boolean. If true then the
  // instruction cannot be deleted.
  DenseMap<StoreInst*, bool> m_store_map;
  
  template<class Q, class QE> 
  inline void enqueue(Q& queue, QE e) {
    DSE_LOG(errs() << "\tEnqueued " << e << "\n");
    queue.push_back(e);
  }
  
  inline void markStoreToKeep(StoreInst *SI) {
    m_store_map[SI] = true;
  }
  
  inline void markStoreToRemove(StoreInst *SI) {
    m_store_map[SI] = false;
  }
  
  // Given a call to shadow.mem.arg.XXX it founds the nearest actual
  // callsite from the original program and return the calleed
  // function.
  const Function* findCalledFunction(ImmutableCallSite &MemSsaCS) {
    const Instruction *I = MemSsaCS.getInstruction();
    for (auto it = I->getIterator(), et = I->getParent()->end(); it != et; ++it) {
      if (const CallInst *CI = dyn_cast<const CallInst>(&*it)) {
	ImmutableCallSite CS(CI);
	if (!CS.getCalledFunction()) {
	  return nullptr;
	}
	
	if (CS.getCalledFunction()->getName().startswith("shadow.mem")) {
	  continue;
	} else {
	  return CS.getCalledFunction();
	}
      }
    }
    return nullptr;
  }
  
public:
  
  static char ID;
  
  IPDeadStoreElimination(): ModulePass(ID) {}
  
  virtual bool runOnModule(Module& M) override {
    if (M.begin () == M.end ()) {
      return false;
    }

    errs() << "Started ip-dse ... \n";
    unsigned skipped_chains = 0;
    
    // Worklist: collect all shadow.mem store instructions whose
    // pointer operand is a global variable.
    std::vector<QueueElem> queue;
    for (auto& F: M) {
      for (auto &I: instructions(&F)) {
	if (isMemSSAStore(&I, OnlySingleton)) {
	  llvm::errs() << I << "\n";
	  auto it = I.getIterator();
	  ++it;
	  llvm::errs() << *it << "\n";
	  if (StoreInst *SI = dyn_cast<StoreInst>(&*it)) {
	    queue.push_back(QueueElem(&I, SI, 0));
	    // All the store instructions will be removed unless the
	    // opposite is proven.
	    markStoreToRemove(SI);
	  } else {
	    report_fatal_error("[IP-DSE] after shadow.mem.store we expect a StoreInst");
	  }
	}
      }
    }
    
    if (!queue.empty()) {

      errs() << "Number of stores: " << queue.size() << "\n";
      MemorySSACallsManager MMan(M, *this, OnlySingleton);
      
      DSE_LOG(errs() << "[IP-DSE] BEGIN initial queue: \n";
	      for(auto &e: queue) {
		errs () << "\t" << e << "\n";
	      }
	      errs () << "[IP-DSE] END initial queue\n";);
      
      boost::unordered_set<QueueElem, QueueElemHasher> visited;
      while (!queue.empty()) {
	QueueElem w = queue.back();
	DSE_LOG(errs() << "[IP-DSE] Processing " << *(w.shadow_mem_inst) << "\n");
	queue.pop_back();
	
	if (!visited.insert(w).second) {
	  markStoreToKeep(w.store_inst);
	  continue;
	}
	
	if (w.length == MaxLenDefUse) {
	  skipped_chains++;
	  markStoreToKeep(w.store_inst);
	  continue;
	}
	if (hasMemSSALoadUser(w.shadow_mem_inst, OnlySingleton)) {
	  DSE_LOG(errs() << "\thas a load user: CANNOT be removed.\n");
	  markStoreToKeep(w.store_inst);
	  continue;
	} 
	
	for (auto &U: w.shadow_mem_inst->uses()) {
	  Instruction *I = dyn_cast<Instruction>(U.getUser());
	  if (!I) continue;
	  DSE_LOG(errs() << "\tChecking user " << *I << "\n");
	  
	  if (PHINode *PHI = dyn_cast<PHINode>(I)) {
	    DSE_LOG(errs () << "\tPHI node: enqueuing lhs\n");
	    enqueue(queue, QueueElem(PHI, w.store_inst, w.length+1));
	  } else if (isa<CallInst>(I)) {
	    ImmutableCallSite CS(I);
	    if (!CS.getCalledFunction()) continue;
	    if (isMemSSAStore(CS, OnlySingleton)) {
	      DSE_LOG(errs() << "\tstore: skipped\n");
	      continue;
	    } else if (isMemSSAArgRef(CS, OnlySingleton)) { 
	      DSE_LOG(errs() << "\targ ref: CANNOT be removed\n");
	      markStoreToKeep(w.store_inst);		
	    } else if (isMemSSAArgMod(CS, OnlySingleton)) {
	      DSE_LOG(errs() << "\targ mod: skipped\n");
	    continue;
	    } else if (isMemSSAArgRefMod(CS, OnlySingleton)) {
	      DSE_LOG(errs() << "\tRecurse inter-procedurally in the callee\n");
	      // Inter-procedural step: we recurse on the uses of
	      // the corresponding formal (non-primed) variable in
	      // the callee.
	      
	      int64_t idx = getMemSSAParamIdx(CS);
	      if (idx < 0) {
		report_fatal_error("[IP-DSE] cannot find index in shadow.mem function");
	      }
	      // HACK: find the actual callsite associated with shadow.mem.arg.ref_mod(...)
	      const Function *calleeF = findCalledFunction(CS);
	      if (!calleeF) {
		report_fatal_error("[IP-DSE] cannot find callee with shadow.mem.XXX function");
	      }
	      const MemorySSAFunction* MemSsaFun = MMan.getFunction(calleeF);
	      if (!MemSsaFun) {
		report_fatal_error("[IP-DSE] cannot find MemorySSAFunction");
	      }
	      
	      if (MemSsaFun->getNumInFormals() == 0) {
		// Probably the function has only shadow.mem.arg.init
		errs() << "TODO: unexpected case function without shadow.mem.in.\n";
		markStoreToKeep(w.store_inst);
		continue;
	      }
	      
	      const Value* calleeInitArgV = MemSsaFun->getInFormal(idx);
	      if (!calleeInitArgV) {
		report_fatal_error("[IP-DSE] getInFormal returned nullptr");
	      }
	      
	      if (const Instruction* calleeInitArg =
		  dyn_cast<const Instruction>(calleeInitArgV)) {
		enqueue(queue, QueueElem(calleeInitArg, w.store_inst, w.length+1));
	      } else {
		report_fatal_error("[IP-DSE] expected to enqueue from callee");
	      }
	      
	    } else if (isMemSSAFunIn(CS, OnlySingleton)) {
	      DSE_LOG(errs() << "\tin: skipped\n");
	      // do nothing
	    } else if (isMemSSAFunOut(CS, OnlySingleton)) {
	      DSE_LOG(errs() << "\tRecurse inter-procedurally in the caller\n");
	      // Inter-procedural step: we recurse on the uses of
	      // the corresponding actual (primed) variable in the
	      // caller.
	      
	      int64_t idx = getMemSSAParamIdx(CS);
	      if (idx < 0) {
		report_fatal_error("[IP-DSE] cannot find index in shadow.mem function");
	      }
	      
	      // Find callers
	      Function *F = I->getParent()->getParent();	    
	      for (auto &U: F->uses()) {
		if (CallInst *CI = dyn_cast<CallInst>(U.getUser())) {
		  const MemorySSACallSite* MemSsaCS = MMan.getCallSite(CI);
		  if (!MemSsaCS) {
		  report_fatal_error("[IP-DSE] cannot find MemorySSACallSite");
		  }
		  
		  // make things easier ...		
		  CallSite CS(CI);
		  assert(CS.getCalledFunction());
		  if (hasFunctionPtrParam(CS.getCalledFunction())) {
		    markStoreToKeep(w.store_inst);
		    continue;
		  }
		  
		  if(idx >= MemSsaCS->numParams()) {
		    // It's possible that the function has formal
		    // parameters but the call site does not have actual
		    // parameters. E.g., llvm can remove the return
		    // parameter from the callsite if it's not used.
		    errs() << "TODO: unexpected case of callsite with no actual parameters.\n";
		    markStoreToKeep(w.store_inst);
		    break;
		  }
		  

		  if (OnlySingleton) {
		    if ((!MemSsaCS->isRefMod(idx)) &&
			(!MemSsaCS->isMod(idx)) &&
			(!MemSsaCS->isNew(idx))) {
		      // XXX: if OnlySingleton then isRefMod, isMod, and
		      // isNew can only return true if the corresponding
		      // memory region is a singleton. We saw cases
		      // (e.g., curl) where we start from store to a
		      // singleton region but after following its
		      // def-use chain we end up having other shadow.mem
		      // instructions that do not correspond to a
		      // singleton region. This is a sea-dsa issue. For
		      // now, we play conservative and give up by
		      // keeping the store.
		      markStoreToKeep(w.store_inst);
		      break;
		    }
		  }
		  
		  assert(OnlySingleton ||
			 MemSsaCS->isRefMod(idx) ||
			 MemSsaCS->isMod(idx) ||
			 MemSsaCS->isNew(idx));		
		  if (const Instruction* caller_primed =
		      dyn_cast<const Instruction>(MemSsaCS->getPrimed(idx))) {
		    enqueue(queue, QueueElem(caller_primed, w.store_inst, w.length+1));
		  } else {
		    report_fatal_error("[IP-DSE] expected to enqueue from caller");
		  }
		}
	      }
	  } else {
	      errs () << "Warning: unexpected case during worklist processing " << *I << "\n";
	    }
	  }
	}
      }
      
      // Finally, we remove dead store instructions
      unsigned num_deleted = 0;
      for (auto &kv: m_store_map) {
	if (!kv.second) {	
	  DSE_LOG(errs() << "[IP-DSE] DELETED " <<  *(kv.first) << "\n");
	  kv.first->eraseFromParent();
	  num_deleted++;
	} 
      }
      
      errs() << "\tNumber of deleted stores " << num_deleted << "\n";
      errs() << "\tSkipped " << skipped_chains
	     << " def-use chains because they were too long\n";
      errs() << "Finished ip-dse\n";
    }
    
    // Make sure that we remove all the shadow.mem functions
    errs() << "Removing shadow.mem functions ... \n";
    sea_dsa::StripShadowMemPass SSMP;
    SSMP.runOnModule(M);
    
    return false;
  }
  
  virtual void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll ();
    // This pass will instrument the code with shadow.mem calls
    AU.addRequired<sea_dsa::ShadowMemPass>();
    AU.addRequired<llvm::UnifyFunctionExitNodes>();      
  }

  virtual StringRef getPassName() const override {
    return "Interprocedural Dead Store Elimination";
  }
  
};

  char IPDeadStoreElimination::ID = 0;
}
}

static llvm::RegisterPass<previrt::transforms::IPDeadStoreElimination>
X("ip-dse", "Inter-procedural Dead Store Elimination");
