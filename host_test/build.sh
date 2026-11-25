#!/bin/sh
# Compila o núcleo para o PC (aritmética 32-bit do secp256k1, igual ao ESP32-C6)
S=../components/secp256k1/lib
gcc -O2 -Wall -Wno-unused-function -Wno-unused-result \
  -I../components/btcsigner/include -I$S/include \
  -DENABLE_MODULE_EXTRAKEYS=1 -DENABLE_MODULE_SCHNORRSIG=1 -DECMULT_WINDOW_SIZE=4 -DUSE_FORCE_WIDEMUL_INT64 \
  t_sign.c ../components/btcsigner/src/*.c $S/src/secp256k1.c $S/src/precomputed_ecmult.c $S/src/precomputed_ecmult_gen.c -o t_sign
gcc -O1 -g -fsanitize=address,undefined -Wno-unused-result \
  -I../components/btcsigner/include -I$S/include \
  -DENABLE_MODULE_EXTRAKEYS=1 -DENABLE_MODULE_SCHNORRSIG=1 -DECMULT_WINDOW_SIZE=4 \
  t_sign.c ../components/btcsigner/src/*.c $S/src/secp256k1.c $S/src/precomputed_ecmult.c $S/src/precomputed_ecmult_gen.c -o t_asan
