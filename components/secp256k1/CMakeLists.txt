idf_component_register(
    SRCS lib/src/secp256k1.c lib/src/precomputed_ecmult.c lib/src/precomputed_ecmult_gen.c
    INCLUDE_DIRS lib/include)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ENABLE_MODULE_EXTRAKEYS=1 ENABLE_MODULE_SCHNORRSIG=1 ECMULT_WINDOW_SIZE=4)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function -Wno-maybe-uninitialized)
