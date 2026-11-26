idf_component_register(
    SRCS src/bs_crypto.c src/bs_bip39.c src/bs_wordlist.c src/bs_encode.c src/bs_signer.c src/bs_ur.c
    INCLUDE_DIRS include
    REQUIRES secp256k1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
