#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Analysis/ValueTracking.h"

#include <map>
#include <set>
#include <vector>
#include <utility>

using namespace llvm;

namespace {

struct DCEPass : public PassInfoMixin<DCEPass> {
    bool runTDCE(Function &F) {
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

    // Helper to decompose a pointer into its base and indices
    struct PointerDecomposition {
        Value *Base;
        std::vector<Value *> Indices;

        bool operator<(const PointerDecomposition &other) const {
            if (Base != other.Base)
                return Base < other.Base;
            return Indices < other.Indices;
        }
    };

    PointerDecomposition decomposePointer(Value *Ptr) {
        std::vector<Value *> Indices;
        Value *Current = Ptr;

        std::set<Value *> Visited; // Prevent infinite loops
        while (true) {
            if (Visited.count(Current)) {
                break;
            }
            Visited.insert(Current);

            if (auto *GEP = dyn_cast<GetElementPtrInst>(Current)) {
                for (auto &idx : GEP->indices()) {
                    Indices.push_back(idx.get());
                }
                Current = GEP->getPointerOperand();
            } else if (auto *BC = dyn_cast<BitCastInst>(Current)) {
                Current = BC->getOperand(0);
            } else {
                break;
            }
        }
        return {Current, Indices};
    }

    bool runLocalDCE(Function &F) {
        bool changed = false;
        for (auto &B : F) {
            DisjointSet dsu;
            std::map<Value *, Value *> memory_map; // Ptr -> Value stored there

            // 1. Build Alias Sets (DSU)
            // We only track value flow here (BitCast, Load/Store forwarding).
            // GEP offsets are handled by decomposition.
            for (auto &I : B) {
                if (auto *BC = dyn_cast<BitCastInst>(&I)) {
                    dsu.unionSets(BC, BC->getOperand(0));
                } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
                    // Only 0-index GEPs alias the base pointer value directly
                    if (GEP->hasAllZeroIndices()) {
                        dsu.unionSets(GEP, GEP->getPointerOperand());
                    }
                } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Value *ptr = SI->getPointerOperand();
                    Value *val = SI->getValueOperand();
                    if (val->getType()->isPointerTy() && !SI->isVolatile()) {
                        memory_map[dsu.find(ptr)] = val;
                    }
                } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    if (LI->getType()->isPointerTy()) {
                        Value *ptr = LI->getPointerOperand();
                        Value *root = dsu.find(ptr);
                        if (memory_map.count(root)) {
                            dsu.unionSets(LI, memory_map[root]);
                        }
                    }
                } else if (isa<CallInst>(&I) || isa<InvokeInst>(&I)) {
                    memory_map.clear();
                }
            }

            // 2. Dead Store Elimination
            std::map<PointerDecomposition, Instruction *> last_def;
            std::vector<Instruction *> storesToRemove;

            for (auto &I : B) {
                // Check Uses (inst.src)
                if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    Value *ptr = LI->getPointerOperand();
                    Value *root = dsu.find(ptr);
                    PointerDecomposition key = decomposePointer(root);
                    
                    if (last_def.count(key)) {
                        last_def.erase(key);
                    }
                } else if (auto *CI = dyn_cast<CallInst>(&I)) {
                    // Conservative: Clear everything that might escape
                    // For simplicity in this assignment, we clear everything
                    // or we could implement the escape analysis again.
                    // Let's reuse the logic: check if base object is Alloca and not escaped.
                    
                    for (auto it = last_def.begin(); it != last_def.end();) {
                        Value *base = it->first.Base;
                        const Value *obj = getUnderlyingObject(base);

                        bool shouldRemove = false;
                        if (!isa<AllocaInst>(obj)) {
                            shouldRemove = true;
                        } else {
                            // Check if passed to call
                             for (auto &arg : CI->args()) {
                                if (arg->getType()->isPointerTy()) {
                                    const Value *argObj = getUnderlyingObject(arg);
                                    if (argObj == obj) {
                                        shouldRemove = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (shouldRemove) {
                            it = last_def.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                // Check Defs (inst.dest)
                if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Value *dest = SI->getPointerOperand();
                    Value *root = dsu.find(dest);
                    PointerDecomposition key = decomposePointer(root);

                    if (last_def.count(key)) {
                        storesToRemove.push_back(last_def[key]);
                    }
                    if (!SI->isVolatile() && !SI->isAtomic()) {
                        last_def[key] = SI;
                    } else {
                        last_def.erase(key);
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
                        if (auto *SI = dyn_cast<StoreInst>(U)) {
                            if (SI->getValueOperand() == AI) {
                                isRead = true; // Stored as value (escapes)
                                break;
                            }
                            // If AI is pointer operand, it's a write.
                        } else {
                            // Any other user (Load, Call, GEP, BitCast, etc.)
                            // Assume it keeps the alloca live.
                            // We could recurse for GEP/BitCast to check if they are only used by stores,
                            // but for now, let's be safe to avoid crashes.
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

                if (runTDCE(F)) {
                    passChanged = true;
                    changed = true;
                }

                if (runLocalDCE(F)) {
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
