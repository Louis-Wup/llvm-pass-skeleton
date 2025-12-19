#include "llvm/IR/Constants.h"
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

struct Expression{
    int opcode;
    int lhs;
    int rhs;

    bool operator<(const Expression &expr) const{
        return std::tie(opcode, lhs, rhs) < std::tie(expr.opcode, expr.lhs, expr.rhs);
    }
};

struct LVNContext{
    std::map<Value *, int> var2num; // variable -> number
    std::map<Expression, int> expr2num; // expression -> number
    std::map<int, Value *> num2var; // number -> variable
    std::map<int, int> varnum2valnum; // variable number -> value number(notice constant also seen as variable, need use num2const get value)
    std::map<int, Constant *> num2const; // number -> constant value(but in LLVM form)
    int numberCnt = 0;

    int getVarNumber(Value *var){
        if (var2num.count(var)){
            return var2num[var];
        }
        int number = numberCnt++;
        var2num[var] = number;
        num2var[number] = var;

        if (auto *c = dyn_cast<Constant>(var)){
            num2const[number] = c;
        }

        return number;
    }
};

struct LVNPass : public PassInfoMixin<LVNPass>{
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM){
        errs() << "Running LVN on module " << M.getName() << "\n";
        bool changed = false;

        for(auto &F : M){
            if(F.isDeclaration()){
                continue;
            }

            for(auto &BB : F){
                LVNContext ctx;

                for(auto &I : BB){
                    if(auto *binOp = dyn_cast<BinaryOperator>(&I)){
                        Value *l = binOp->getOperand(0);
                        Value *r = binOp->getOperand(1);

                        int numL = ctx.getVarNumber(l);
                        int numR = ctx.getVarNumber(r);

                        if(binOp->isCommutative() && numL > numR){
                            std::swap(numL, numR);
                        }

                        Expression expr{binOp->getOpcode(), numL, numR};

                        if(ctx.expr2num.count(expr)){
                            int numFound = ctx.expr2num[expr];
                            Value *replacement = ctx.num2var[numFound];

                            binOp->replaceAllUsesWith(replacement);
                            changed = true;

                            ctx.var2num[binOp] = numFound;
                        }
                        else{
                            if(ctx.num2const.count(numL) && ctx.num2const.count(numR)){
                                Constant *cl = ctx.num2const[numL];
                                Constant *cr = ctx.num2const[numR];
                                Constant *folded = ConstantExpr::get(binOp->getOpcode(), cl, cr);

                                binOp->replaceAllUsesWith(folded);
                                changed = true;

                                int newNum = ctx.getVarNumber(folded);
                                ctx.var2num[binOp] = newNum;
                                ctx.expr2num[expr] = newNum;
                            }
                            else{
                                int newNum = ctx.getVarNumber(binOp);
                                ctx.expr2num[expr] = newNum;
                            }
                        }
                    }
                    else if(auto *alloca = dyn_cast<AllocaInst>(&I)){
                        ctx.getVarNumber(alloca);
                    }
                    else if(auto *store = dyn_cast<StoreInst>(&I)){
                        Value *value = store->getValueOperand();
                        Value *var = store->getPointerOperand();

                        int numValue = ctx.getVarNumber(value);
                        int numVar = ctx.getVarNumber(var);

                        ctx.varnum2valnum[numVar] = numValue;
                    }
                    else if(auto *load = dyn_cast<LoadInst>(&I)){
                        Value *var = load->getPointerOperand();
                        int numVar = ctx.getVarNumber(var);

                        if(ctx.varnum2valnum.count(numVar)){
                            int numValue = ctx.varnum2valnum[numVar];
                            Value *replacement = ctx.num2var[numValue];

                            load->replaceAllUsesWith(replacement);
                            changed = true;

                            ctx.var2num[load] = numValue;
                        }
                        else{
                            int numValue = ctx.getVarNumber(load);
                            ctx.varnum2valnum[numVar] = numValue;
                        }
                    }
                    else{
                        ctx.getVarNumber(&I);
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
