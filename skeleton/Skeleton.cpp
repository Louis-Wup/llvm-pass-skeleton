#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <unordered_map>
#include <set>
#include <map>

using namespace llvm;

namespace {

struct SkeletonPass : public PassInfoMixin<SkeletonPass> {
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

    bool runLocalDSE(Function &F) {
        bool changed = false;
        for (auto &B : F) {
            std::map<Value *, Instruction *> last_def; // Pointer -> StoreInst
            std::vector<Instruction *> storesToRemove;

            for (auto &I : B) {
                // Check Uses (inst.src)
                if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    Value *ptr = LI->getPointerOperand();
                    if (last_def.count(ptr)) {
                        last_def.erase(ptr);
                    }
                }
                else if (auto *CI = dyn_cast<CallInst>(&I)) {
                    // CallInst might read from globals or arguments.
                    for (auto it = last_def.begin(); it != last_def.end(); ) {
                        if (isa<GlobalValue>(it->first)) {
                            it = last_def.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    for (auto &op : CI->args()) {
                        if (last_def.count(op)) {
                            last_def.erase(op);
                        }
                    }
                }

                // Check Defs (inst.dest)
                if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Value *dest = SI->getPointerOperand();
                    if (last_def.count(dest)) {
                        storesToRemove.push_back(last_def[dest]);
                    }
                    if (!SI->isVolatile()) {
                        last_def[dest] = SI;
                    } else {
                        last_def.erase(dest);
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

    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
        bool changed = false;
        bool passChanged;
        
        do {
            passChanged = false;
            
            for (auto &F : M.functions()) {
                if (F.isDeclaration()) continue;
                
                if (runIterativeDCE(F)) {
                    passChanged = true;
                    changed = true;
                }

                if (runLocalDSE(F)) {
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
            .PluginName = "Skeleton pass",
            .PluginVersion = "v0.1",
            .RegisterPassBuilderCallbacks = [](PassBuilder &PB) {
                PB.registerPipelineStartEPCallback(
                    [](ModulePassManager &MPM, OptimizationLevel Level) {
                        MPM.addPass(SkeletonPass());
                    });
            }};
}
