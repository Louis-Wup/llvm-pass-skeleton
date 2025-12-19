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

struct DCEPass : public PassInfoMixin<DCEPass>{
    bool runTDCE(Function &F){
        bool changed = false;
        bool dceChanged = true;
        while(dceChanged){
            dceChanged = false;
            std::set<Value *> used;
            std::vector<Instruction *> instsToRemove;

            for(auto &BB : F){
                for(auto &I : BB){
                    for(auto &op : I.operands()){
                        used.insert(op);
                    }
                }
            }

            for(auto &BB : F){
                for(auto &I : BB){
                    if(!I.getType()->isVoidTy() && used.find(&I) == used.end() && !I.mayHaveSideEffects()){
                        instsToRemove.push_back(&I);
                    }
                    else if(auto *AI = dyn_cast<AllocaInst>(&I)){
                        bool isRead = false;
                        for(auto *U : AI->users()){
                            if(auto *SI = dyn_cast<StoreInst>(U)){
                                if(SI->getValueOperand() == AI){
                                    isRead = true;
                                    break;
                                }
                            }
                            else{
                                isRead = true;
                                break;
                            }
                        }

                        if(!isRead){
                            for(auto *U : AI->users()){
                                if(auto *SI = dyn_cast<StoreInst>(U)){
                                    instsToRemove.push_back(SI);
                                }
                            }
                            instsToRemove.push_back(AI);
                        }
                    }
                }
            }

            for(auto *I : instsToRemove){
                if(I->getParent()){
                    I->eraseFromParent();
                    dceChanged = true;
                    changed = true;
                }
            }
        }
        return changed;
    }

    bool runLocalDCE(Function &F){
        bool changed = false;
        for(auto &BB : F){
            std::map<Value *, Instruction *> lastStore;
            std::vector<Instruction *> storesToRemove;

            for(auto &I : BB){
                if(auto *LI = dyn_cast<LoadInst>(&I)){
                    Value *ptr = LI->getPointerOperand();
                    lastStore.erase(ptr);
                }
                else if(auto *SI = dyn_cast<StoreInst>(&I)){
                    Value *ptr = SI->getPointerOperand();
                    if(lastStore.count(ptr)){
                        storesToRemove.push_back(lastStore[ptr]);
                    }
                    else{
                        lastStore.erase(ptr);
                    }
                    lastStore[ptr] = SI;
                }
            }

            for(auto *I : storesToRemove){
                I->eraseFromParent();
                changed = true;
            }
        }
        return changed;
    }

    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM){
        errs() << "Running DCE on " << M.getName() << "\n";
        bool changed = false;
        bool passChanged = true;

        while(passChanged){
            passChanged = false;

            for(auto &F : M.functions()){
                if(F.isDeclaration()){
                    continue;
                }

                if(runTDCE(F)){
                    passChanged = true;
                    changed = true;
                }

                if(runLocalDCE(F)){
                    passChanged = true;
                    changed = true;
                }
            }
        }

        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
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
