#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <tuple>

using namespace llvm;

namespace {

struct Expression {
    unsigned Opcode;
    int LHS, RHS;

    bool operator<(const Expression &O) const {
        return std::tie(Opcode, LHS, RHS) < std::tie(O.Opcode, O.LHS, O.RHS);
    }
};

struct LVNPass : public PassInfoMixin<LVNPass> {
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
        errs() << "Running LVN on module " << M.getName() << "\n";
        bool changed = false;

        for (auto &F : M) {
            if (F.isDeclaration())
                continue;

            // errs() << "  Processing function: " << F.getName() << "\n";

            for (auto &BB : F) {
                std::map<Value *, int> valueNumMap;
                std::map<Expression, int> exprNumMap;
                std::map<int, Value *> numToValueMap;
                std::map<int, int> memoryMap; // VN_Ptr -> VN_Value
                int nextVN = 1;

                auto getValueNumber = [&](Value *V) -> int {
                    if (valueNumMap.count(V))
                        return valueNumMap[V];
                    int vn = nextVN++;
                    valueNumMap[V] = vn;
                    numToValueMap[vn] = V;
                    return vn;
                };

                for (auto &I : BB) {
                    if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
                        Value *L = BinOp->getOperand(0);
                        Value *R = BinOp->getOperand(1);

                        int vnL = getValueNumber(L);
                        int vnR = getValueNumber(R);

                        Expression expr{BinOp->getOpcode(), vnL, vnR};

                        if (exprNumMap.count(expr)) {
                            // Redundancy found!
                            int vnFound = exprNumMap[expr];
                            Value *replacement = numToValueMap[vnFound];

                            // Replace all uses of this instruction with the
                            // found value. This effectively makes 'BinOp' dead
                            // code.
                            BinOp->replaceAllUsesWith(replacement);
                            changed = true;

                            // Map the redundant instruction to the existing VN
                            valueNumMap[BinOp] = vnFound;
                        } else {
                            // New expression
                            int newVN = getValueNumber(BinOp); // Assigns nextVN
                            exprNumMap[expr] = newVN;
                        }
                    } else if (auto *Alloca = dyn_cast<AllocaInst>(&I)) {
                        // Unique VN for each alloca (pointer)
                        getValueNumber(Alloca);
                    } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
                        Value *Val = Store->getValueOperand();
                        Value *Ptr = Store->getPointerOperand();

                        int vnVal = getValueNumber(Val);
                        int vnPtr = getValueNumber(Ptr);

                        memoryMap[vnPtr] = vnVal;
                    } else if (auto *Load = dyn_cast<LoadInst>(&I)) {
                        Value *Ptr = Load->getPointerOperand();
                        int vnPtr = getValueNumber(Ptr);

                        if (memoryMap.count(vnPtr)) {
                            // We know what's stored here!
                            int vnVal = memoryMap[vnPtr];
                            Value *replacement = numToValueMap[vnVal];

                            Load->replaceAllUsesWith(replacement);
                            changed = true;

                            // Map this load to that value
                            valueNumMap[Load] = vnVal;
                        } else {
                            // Unknown value (first load or unknown memory)
                            int newVN = getValueNumber(Load);
                            memoryMap[vnPtr] = newVN; // Assume consistent
                        }
                    } else {
                        // For other ops, just assign a VN
                        getValueNumber(&I);
                    }
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
            .PluginName = "LVN pass",
            .PluginVersion = "v0.1",
            .RegisterPassBuilderCallbacks = [](PassBuilder &PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager &MPM,
                       ArrayRef<PassBuilder::PipelineElement>) {
                        if (Name == "lvn-pass") {
                            MPM.addPass(LVNPass());
                            return true;
                        }
                        return false;
                    });
                PB.registerPipelineStartEPCallback(
                    [](ModulePassManager &MPM, OptimizationLevel Level) {
                        MPM.addPass(LVNPass());
                    });
            }};
}
