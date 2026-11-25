#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "board.h"
uint16_t FB[LCD_H][LCD_W];
static int g_cap, g_n;
void on_delay(int ms){
  if(!g_cap) return;
  char fn[64]; snprintf(fn,sizeof fn,"anim_%03d_%d.ppm",g_n++,ms); FILE*f=fopen(fn,"wb"); fprintf(f,"P6 %d %d 255\n",LCD_W,LCD_H);
  for(int y=0;y<LCD_H;y++)for(int x=0;x<LCD_W;x++){uint16_t c=FB[y][x];unsigned char rgb[3]={(c>>11)<<3,((c>>5)&63)<<2,(c&31)<<3};fwrite(rgb,1,3,f);}fclose(f);
}
#include "../main/display.c"
#include "../main/ui.c"
#include "bs_signer.h"
void bs_platform_random(uint8_t *b, size_t n){for(size_t i=0;i<n;i++)b[i]=rand();}
int main(void){
  g_cap=1;
  ui_init(0);                     // splash (captured on its delays)
  ui_set_identity("PokeSigner-F3EA",0x94f97265);
  s_disp=1; s_modal=0;
  ui_idle(UI_SECURE); on_delay(1500);
  idle_sprite(SIGNY_BLINK); on_delay(140); idle_sprite(SIGNY_OPEN); on_delay(1200);
  ui_celebrate("Enviado ao celular.", NULL);
  return 0;
}
