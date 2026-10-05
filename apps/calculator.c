#include "picoapp.h"
#define WINW 320
#define WINH 390
#define TITLE 30
static int wx=160,wy=45,drag=0,dx,dy;
static char display[32]="0"; static int dlen=1,acc=0,have_acc=0,newnum=1; static char op=0;
static void rect3(int x,int y,int w,int h,int fill,int edge){api->fill_rect(x+5,y+5,w,h,8);api->fill_rect(x,y,w,h,fill);api->fill_rect(x,y,w,2,edge);api->fill_rect(x,y,2,h,edge);api->fill_rect(x,y+h-2,w,2,0);api->fill_rect(x+w-2,y,2,h,0);}
static void txt(int x,int y,const char*s,int fg,int bg){api->text(x,y,s,fg,bg);} static int hit(int x,int y,int w,int h,int mx,int my){return mx>=x&&mx<x+w&&my>=y&&my<y+h;}
static int val(void){return api_atoi(display);} static void setval(int v){app_itoa(v,display);dlen=app_strlen(display);}
static void clear_all(void){display[0]='0';display[1]=0;dlen=1;acc=0;have_acc=0;op=0;newnum=1;}
static void digit(int k){if(newnum){display[0]='0'+k;display[1]=0;dlen=1;newnum=0;return;}if(dlen<10&&!(dlen==1&&display[0]=='0')){display[dlen++]='0'+k;display[dlen]=0;}}
static void apply(char next){int v=val(),r=acc;if(have_acc){if(op=='+')r=acc+v;else if(op=='-')r=acc-v;else if(op=='*')r=acc*v;else if(op=='/'&&v)r=acc/v;else if(op=='/'&&!v){setval(0);have_acc=0;op=0;newnum=1;return;}acc=r;setval(r);}else{acc=v;have_acc=1;}op=next;newnum=1;}
int app_main(int argc,char**argv){
 (void)argc;(void)argv;int W=640,H=480;if(!api->gfx_mode(1))return 1;api->gfx_size(&W,&H);api->cursor(0);clear_all();
 int running=1,prev=0,mx=0,my=0,mb=0,dirty=1;
 static const char*lab[16]={"7","8","9","/","4","5","6","*","1","2","3","-","0","C","<-","+"};
 static const int key[16]={7,8,9,'/',4,5,6,'*',1,2,3,'-',0,-1,-2,'+'};
 while(running){
  int bw=58,bh=45,g=8,sx=wx+22,sy=wy+118,ex=sx,ey=sy+4*(bh+g),ew=4*bw+3*g;
  if(dirty){
   api->fill_rect(0,0,W,H,1);api->fill_rect(0,0,W,26,8);txt(12,8,"PicoOS  •  Calculator",15,8);
   rect3(wx,wy,WINW,WINH,15,9);api->fill_rect(wx+2,wy+2,WINW-4,TITLE-2,9);txt(wx+12,wy+9,"Calculator",15,9);txt(wx+WINW-45,wy+9,"X",15,9);
   rect3(wx+18,wy+48,WINW-36,52,0,7);txt(wx+30,wy+64,display,15,0);if(op){char o[2]={op,0};txt(wx+WINW-48,wy+65,o,14,0);}
   for(int i=0;i<16;i++){int r=i/4,c=i%4,x=sx+c*(bw+g),y=sy+r*(bh+g);int active=(mx>=x&&mx<x+bw&&my>=y&&my<y+bh&&mb);int fill=(i==13)?8:((i%4==3)?11:7);rect3(x,y,bw,bh,active?14:fill,active?15:9);txt(x+24,y+14,lab[i],(i%4==3||i==13)?15:0,active?14:fill);}
   rect3(ex,ey,ew,48,(mb&&hit(ex,ey,ew,48,mx,my))?14:9,15);txt(ex+ew/2-8,ey+15,"=",15,9);txt(wx+18,wy+WINH-24,"Drag title bar  •  keyboard works too",8,15);dirty=0;
  }
  int tx=mx,ty=my,tb=0,hm=api->mouse_px(&tx,&ty,&tb);if(hm){mx=tx;my=ty;}
  if(hm&&(tb&1)&&!(prev&1)){
   if(hit(wx,wy,WINW,TITLE,mx,my)){drag=1;dx=mx-wx;dy=my-wy;}
   else if(hit(wx+WINW-58,wy,58,TITLE,mx,my))running=0;
   else{
    int handled=0;
    for(int i=0;i<16;i++){int r=i/4,c=i%4,x=sx+c*(bw+g),y=sy+r*(bh+g);if(hit(x,y,bw,bh,mx,my)){int k=key[i];handled=1;if(k>=0&&k<=9)digit(k);else if(k==-1)clear_all();else if(k==-2){if(dlen>1)display[--dlen]=0;else display[0]='0';}else apply((char)k);dirty=1;break;}}
    if(!handled&&hit(ex,ey,ew,48,mx,my)){if(have_acc&&op){apply(0);op=0;have_acc=0;}newnum=1;dirty=1;}
   }
  }
  if(!(tb&1))drag=0;if(drag){wx=mx-dx;wy=my-dy;if(wx<8)wx=8;if(wy<30)wy=30;if(wx+WINW>W-8)wx=W-8-WINW;if(wy+WINH>H-8)wy=H-8-WINH;dirty=1;}prev=tb;
  int k=api->poll();
  if(k==27||k==0x93)running=0;else if(k>='0'&&k<='9'){digit(k-'0');dirty=1;}else if(k=='c'||k=='C'){clear_all();dirty=1;}else if(k=='+'||k=='-'||k=='*'||k=='/'){apply((char)k);dirty=1;}else if(k=='='||k=='\n'||k=='\r'){if(have_acc&&op){apply(0);op=0;have_acc=0;}newnum=1;dirty=1;}else if(k=='\b'){if(dlen>1)display[--dlen]=0;else display[0]='0';dirty=1;}
  api->sleep(25);
 }
 api->cursor(1);api->gfx_mode(0);return 0;
}
