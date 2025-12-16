#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

struct LVNPass : public PassInfoMixin<LVNPass> {
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
        errs() << "I am LVN Pass!\n";
        return PreservedAnalyses::all();
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
                            MPM.addPass(
                                createModuleToFunctionPassAdaptor(LVNPass()));
                            return true;
                        }
                        return false;
                    });
                PB.registerPipelineStartEPCallback([](ModulePassManager &MPM,
                                                      OptimizationLevel Level) {
                    MPM.addPass(createModuleToFunctionPassAdaptor(LVNPass()));
                });
            }};
}
