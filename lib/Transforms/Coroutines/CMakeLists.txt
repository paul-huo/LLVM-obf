add_llvm_library(LLVMCoroutines
  Coroutines.cpp
  CoroCleanup.cpp
  CoroEarly.cpp
  CoroElide.cpp
  CoroSplit.cpp
  )

add_dependencies(LLVMCoroutines intrinsics_gen)
