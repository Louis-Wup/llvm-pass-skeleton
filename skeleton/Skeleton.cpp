#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <unordered_map>
#include <set>

using namespace llvm;

namespace {

struct SkeletonPass : public PassInfoMixin<SkeletonPass> {
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
        bool changed = false;
        for (auto &F : M.functions()) {
            std::set<Value *> used;
            // 1. Collect used values
            for (auto &B : F) {
                for (auto &I : B) {
                    for (auto &op : I.operands()) {
                        used.insert(op);
                    }
                }
            }

            // 2. Identify dead instructions
            std::vector<Instruction *> instsToRemove;
            for (auto &B : F) {
                for (auto &I : B) {
                    // Check if instruction produces a value (not void)
                    // AND is not used
                    // AND does not have side effects
                    if (!I.getType()->isVoidTy() &&
                        used.find(&I) == used.end() &&
                        !I.mayHaveSideEffects()) {
                        instsToRemove.push_back(&I);
                    }
                }
            }

            // 3. Remove them
            for (auto *I : instsToRemove) {
                I->eraseFromParent();
                changed = true;
            }
        }
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
