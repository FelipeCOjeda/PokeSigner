#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
uint16_t FB[172][320];
#include "../main/display.c"
static int g_shot;
static void save(void){char fn[64];snprintf(fn,sizeof fn,"shot_%02d.ppm",g_shot++);FILE*f=fopen(fn,"wb");fprintf(f,"P6 320 172 255\n");
  for(int y=0;y<172;y++)for(int x=0;x<320;x++){uint16_t c=FB[y][x];unsigned char rgb[3]={(c>>11)<<3,((c>>5)&63)<<2,(c&31)<<3};fwrite(rgb,1,3,f);}fclose(f);}
#include "../main/ui.c"
#include "bs_signer.h"
#include "bs_bip39.h"
void bs_platform_random(uint8_t *b, size_t n){for(size_t i=0;i<n;i++)b[i]=rand();}
static uint8_t buf[65536];
int main(int argc,char**argv){
  (void)argc;
  ui_init(0); save();
  ui_set_identity("PokeSigner-A1B2",0x73c5da0a);
  s_disp=1;
  ui_idle(UI_SECURE); save(); ui_passkey(48213); save();
  ui_seed_words("abandon ability able about above absent absorb abstract absurd abuse access accident account accuse achieve acid acoustic acquire across act action actor actress actual"); save();
  char norm[400]; bs_bip39_normalize(argv[1],norm,sizeof norm); uint8_t seed[64]; bs_bip39_seed(norm,"",seed); bs_signer_init(seed);
  FILE*f=fopen(argv[2],"rb"); size_t n=fread(buf,1,sizeof buf,f); fclose(f);
  static bs_psbt_t P; int r=bs_psbt_analyze(buf,n,&P); if(r){printf("err %d\n",r);return 1;}
  int total = 1 + (int)count_dest(&P) + (P.change_total > 0) + 1;
  for(int i=0;i<total;i++){ build_page(&P,i,total,"482913"); g_show(); save();}
  ui_msg("FUGIU!", "O PSBT fugiu! (tempo", "esgotado ou desconexao)", C_ORANGE); save();
  return 0;
}
