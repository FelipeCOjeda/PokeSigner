#include "bs_signer.h"
#include "bs_bip39.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void bs_platform_random(uint8_t *b, size_t n){ FILE*f=fopen("/dev/urandom","rb"); fread(b,1,n,f); fclose(f);}
static uint8_t inbuf[65536], outbuf[70000];
static bs_psbt_t P;
int main(int argc,char**argv){
  // argv: mnemonic passphrase cmd [args]
  char norm[400]; if(bs_bip39_normalize(argv[1],norm,sizeof norm)){fprintf(stderr,"bad mnemonic\n");return 2;}
  uint8_t seed[64]; bs_bip39_seed(norm,argv[2],seed);
  if(bs_signer_init(seed)) return 3;
  if(!strcmp(argv[3],"info")){ char j[4096]; int r=bs_signer_get_info(atoi(argv[4]),0,j,sizeof j); printf("%s\n",j); return r;}
  if(!strcmp(argv[3],"sign")){
    FILE*f=fopen(argv[4],"rb"); size_t n=fread(inbuf,1,sizeof inbuf,f); fclose(f);
    int r=bs_psbt_analyze(inbuf,n,&P); if(r){printf("ERR %d %s | %s\n",r,bs_strerror(r),bs_psbt_detail()); return 1;}
    char s[4096]; bs_psbt_summary(&P,s,sizeof s); fputs(s,stderr);
    size_t ol; r=bs_psbt_sign(&P,outbuf,sizeof outbuf,&ol); if(r){printf("ERR %d %s\n",r,bs_strerror(r));return 1;}
    f=fopen(argv[5],"wb"); fwrite(outbuf,1,ol,f); fclose(f); printf("OK\n"); return 0;}
  if(!strcmp(argv[3],"mnemonic")){ char m[400]; uint8_t e[32]={0}; for(int i=0;i<32;i++)e[i]=i; bs_bip39_from_entropy(e,32,m,sizeof m); printf("%s\n",m); return 0;}
  return 1;
}
