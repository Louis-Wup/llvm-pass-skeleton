#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <map>
#include <set>

using namespace llvm;

namespace {

struct DCEPass : public PassInfoMixin<DCEPass> {
    bool runIterativeDCE(Function &F) {
        bool changed = false;
        bool dceChanged = true;
        while (dceChanged) {
            dceChanged = false;
            std::set<Value *> used;

            // Collect used values
            for (auto &B : F) {
                for (auto &I : B) {
                    for (auto &op : I.operands()) {
                        used.insert(op);
                    }
                }
            }

            // Identify dead instructions
            std::vector<Instruction *> instsToRemove;
            for (auto &B : F) {
                for (auto &I : B) {
                    if (!I.getType()->isVoidTy() &&
                        used.find(&I) == used.end() &&
                        !I.mayHaveSideEffects()) {
                        instsToRemove.push_back(&I);
                    }
                }
            }

            // Remove them
            for (auto *I : instsToRemove) {
                I->eraseFromParent();
                dceChanged = true;
                changed = true;
            }
        }
        return changed;
    }

    struct DisjointSet {
        std::map<Value *, Value *> parent;

        Value *find(Value *v) {
            if (parent.find(v) == parent.end() || parent[v] == v)
                return parent[v] = v;
            return parent[v] = find(parent[v]);
        }

        void unionSets(Value *a, Value *b) {
            Value *rootA = find(a);
            Value *rootB = find(b);
            if (rootA != rootB) {
                parent[rootA] = rootB;
            }
        }
    };

    bool runLocalDSE(Function &F) {
        bool changed = false;
        for (auto &B : F) {
            DisjointSet dsu;
            std::map<Value *, Value *> memory_map; // Ptr -> Value stored there

            // Build alias sets for the block
            for (auto &I : B) {
                if (auto *BC = dyn_cast<BitCastInst>(&I)) {
                    dsu.unionSets(BC, BC->getOperand(0));
                } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
                    if (GEP->hasAllZeroIndices()) {
                        dsu.unionSets(GEP, GEP->getPointerOperand());
                    }
                } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Value *ptr = SI->getPointerOperand();
                    Value *val = SI->getValueOperand();
                    // Only track simple pointer stores for alias propagation
                    if (val->getType()->isPointerTy() && !SI->isVolatile()) {
                        memory_map[dsu.find(ptr)] = val;
                        // errs() << "MemMap[" << dsu.find(ptr) << "] = " << val
                        // << " (from " << *SI << ")\n";
                    } else {
                        // If storing non-pointer or volatile, we might want to
                        // invalidate, but for 'ptr to ptr' tracking, mostly
                        // overwriting is enough. Conservative: if we don't know
                        // what we are bringing in, we can't propagate.
                    }
                } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    if (LI->getType()->isPointerTy()) {
                        Value *ptr = LI->getPointerOperand();
                        Value *root = dsu.find(ptr);
                        if (memory_map.count(root)) {
                            dsu.unionSets(LI, memory_map[root]);
                            // errs() << "Union(" << *LI << ", " <<
                            // *memory_map[root] << ")\n";
                        }
                    }
                } else if (isa<CallInst>(&I) || isa<InvokeInst>(&I)) {
                    // Calls might modify memory. Conservatively clear our
                    // knowledge.
                    memory_map.clear();
                }
            }

            std::map<Value *, Instruction *>
                last_def; // Representative Pointer -> StoreInst
            std::vector<Instruction *> storesToRemove;

            for (auto &I : B) {
                // Check Uses (inst.src)
                if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    Value *ptr = LI->getPointerOperand();
                    Value *root = dsu.find(ptr);
                    if (last_def.count(root)) {
                        last_def.erase(root);
                    }
                } else if (auto *CI = dyn_cast<CallInst>(&I)) {
                    // CallInst might read from globals or arguments.
                    for (auto it = last_def.begin(); it != last_def.end();) {
                        if (isa<GlobalValue>(it->first)) {
                            it = last_def.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    for (auto &op : CI->args()) {
                        if (op->getType()->isPointerTy()) { // Only check
                                                            // pointer arguments
                            Value *root = dsu.find(op);
                            if (last_def.count(root)) {
                                last_def.erase(root);
                            }
                        }
                    }
                }

                // Check Defs (inst.dest)
                if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Value *dest = SI->getPointerOperand();
                    Value *root = dsu.find(dest);

                    if (last_def.count(root)) {
                        storesToRemove.push_back(last_def[root]);
                    }
                    if (!SI->isVolatile()) {
                        last_def[root] = SI;
                    } else {
                        last_def.erase(root);
                    }
                }
            }

            for (auto *I : storesToRemove) {
                I->eraseFromParent();
                changed = true;
            }
        }
        return changed;
    }

    bool runDeadAllocaElimination(Function &F) {
        bool changed = false;
        std::vector<AllocaInst *> deadAllocas;

        for (auto &B : F) {
            for (auto &I : B) {
                if (auto *AI = dyn_cast<AllocaInst>(&I)) {
                    bool isRead = false;
                    for (auto *U : AI->users()) {
                        if (isa<LoadInst>(U)) {
                            isRead = true;
                            break;
                        }
                        // If passed to a call, assume read
                        if (isa<CallInst>(U)) {
                            isRead = true;
                            break;
                        }
                    }

                    if (!isRead) {
                        deadAllocas.push_back(AI);
                    }
                }
            }
        }

        for (auto *AI : deadAllocas) {
            // Remove all stores to this alloca
            std::vector<Instruction *> usersToRemove;
            for (auto *U : AI->users()) {
                if (isa<StoreInst>(U)) {
                    usersToRemove.push_back(cast<Instruction>(U));
                }
            }

            for (auto *I : usersToRemove) {
                I->eraseFromParent();
            }

            AI->eraseFromParent();
            changed = true;
        }

        return changed;
    }

    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
        errs() << "Running DCE on " << M.getName() << "\n";
        bool changed = false;
        bool passChanged;

        do {
            passChanged = false;

            for (auto &F : M.functions()) {
                if (F.isDeclaration())
                    continue;

                if (runIterativeDCE(F)) {
                    passChanged = true;
                    changed = true;
                }

                if (runLocalDSE(F)) {
                    passChanged = true;
                    changed = true;
                }

                if (runDeadAllocaElimination(F)) {
                    passChanged = true;
                    changed = true;
                }
            }

        } while (passChanged);

        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    };
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
    return {.APIVersion = LLVM_PLUGIN_API_VERSION,
            .PluginName = "DCE pass",
            .PluginVersion = "v0.1",
            .RegisterPassBuilderCallbacks = [](PassBuilder &PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager &MPM,
                       ArrayRef<PassBuilder::PipelineElement>) {
                        if (Name == "dce-pass") {
                            MPM.addPass(DCEPass());
                            return true;
                        }
                        return false;
                    });
                PB.registerPipelineStartEPCallback(
                    [](ModulePassManager &MPM, OptimizationLevel Level) {
                        MPM.addPass(DCEPass());
                    });
            }};
}
