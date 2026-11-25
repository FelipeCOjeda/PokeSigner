// Renders review screens to PNG-able PPM using the real ui.c layout + font.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "display.h"
extern const uint8_t FONT8X16[95][16];
static uint16_t fb[320][172];
static int g_shot;
int disp_init(void){return 0;}
int disp_cols(int s){return 172/(8*s);}
void disp_page(const disp_line_t *lines, size_t n){
  for(int y=0;y<320;y++)for(int x=0;x<172;x++)fb[y][x]=0;
  int y=0;
  for(size_t i=0;i<n&&y<320;i++){const disp_line_t*l=&lines[i];int sc=l->scale?l->scale:1;
    size_t len=l->text?strlen(l->text):0;int cols=disp_cols(sc);if((int)len>cols)len=cols;
    int x0=l->center?(172-(int)len*8*sc)/2:2;
    for(size_t k=0;k<len;k++){const uint8_t*g=FONT8X16[(unsigned char)l->text[k]-32];
      for(int gy=0;gy<16;gy++)for(int gx=0;gx<8;gx++)if(g[gy]&(0x80>>gx))
        for(int dy=0;dy<sc;dy++)for(int dx=0;dx<sc;dx++){int px=x0+(int)k*8*sc+gx*sc+dx,py=y+gy*sc+dy;if(px>=0&&px<172&&py<320)fb[py][px]=l->fg;}}
    y+=16*sc;}
  char fn[64];snprintf(fn,sizeof fn,"shot_%02d.ppm",g_shot++);FILE*f=fopen(fn,"wb");fprintf(f,"P6 172 320 255\n");
  for(int yy=0;yy<320;yy++)for(int x=0;x<172;x++){uint16_t c=fb[yy][x];unsigned char rgb[3]={(c>>11)<<3,((c>>5)&63)<<2,(c&31)<<3};fwrite(rgb,1,3,f);}fclose(f);
}

#include "../main/ui.c"

#include "bs_signer.h"
#include "bs_bip39.h"
void bs_platform_random(uint8_t *b, size_t n){for(size_t i=0;i<n;i++)b[i]=rand();}
static uint8_t buf[65536];
int main(int argc,char**argv){
  s_disp=1;
  ui_set_identity("PokeSigner-A1B2",0x73c5da0a);
  ui_idle(UI_SECURE); ui_passkey(48213);
  ui_seed_words("abandon ability able about above absent absorb abstract absurd abuse access accident account accuse achieve acid acoustic acquire across act action actor actress actual");
  char norm[400]; bs_bip39_normalize(argv[1],norm,sizeof norm); uint8_t seed[64]; bs_bip39_seed(norm,"",seed); bs_signer_init(seed);
  FILE*f=fopen(argv[2],"rb"); size_t n=fread(buf,1,sizeof buf,f); fclose(f);
  static bs_psbt_t P; int r=bs_psbt_analyze(buf,n,&P); if(r){printf("err %d\n",r);return 1;}
  int total = 1 + (int)count_dest(&P) + (P.change_total > 0) + 1;
  for(int i=0;i<total;i++){ build_page(&P,i,total,&s_pages[0]); disp_page(s_pages[0].l,s_pages[0].n);}
  return 0;
}
