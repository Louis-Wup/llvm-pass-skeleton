add_llvm_pass_plugin(DCEPass
    PARTIAL_SOURCES_INTENDED
    DCE.cpp
)

add_llvm_pass_plugin(LVNPass
    PARTIAL_SOURCES_INTENDED
    LVN.cpp
)
