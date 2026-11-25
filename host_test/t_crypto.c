#include "bs_crypto.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static void hex(const uint8_t*p,size_t n){for(size_t i=0;i<n;i++)printf("%02x",p[i]);printf("\n");}
int main(int argc,char**argv){
  const char*m=argv[1]; size_t n=strlen(m); uint8_t o[64];
  bs_sha256(m,n,o);hex(o,32);
  bs_sha512_ctx c;bs_sha512_init(&c);bs_sha512_update(&c,m,n);bs_sha512_final(&c,o);hex(o,64);
  bs_ripemd160(m,n,o);hex(o,20);
  bs_hmac_sha512((const uint8_t*)"key",3,(const uint8_t*)m,n,o);hex(o,64);
  bs_pbkdf2_hmac_sha512((const uint8_t*)m,n,(const uint8_t*)"mnemonicTREZOR",14,2048,o);hex(o,64);
}
