// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned le32(const unsigned char *p) {return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24;}
int main(int argc,char **argv){
 unsigned char b[8192];size_t pos=32;long patch=-1;int gbe=0;
 if(argc!=4||strlen(argv[3])!=4||memcmp(argv[3],"eth",3)||argv[3][3]<'0'||argv[3][3]>'4')return 2;
 FILE *f=fopen(argv[1],"rb");if(!f)return 3;
 size_t n=fread(b,1,sizeof(b),f);int extra=fgetc(f);fclose(f);
 if(n!=sizeof(b)||extra!=EOF||memcmp(b,"gOGoBrCm",8)||le32(b+8)!=1)return 4;
 while(pos+20<=n&&b[pos]){
  unsigned len=le32(b+pos+16);if(!memchr(b+pos,0,16)||!len||len>n-pos-20)return 5;
  if(!strcmp((char *)b+pos,"RdpaWanType"))gbe=len==3&&!memcmp(b+pos+20,"GBE",3);
  if(!strcmp((char *)b+pos,"WanOEMac")){
   if(patch!=-1||len!=5||memcmp(b+pos+20,"EMAC",4)||b[pos+24]<'0'||b[pos+24]>'4')return 6;
   patch=(long)pos+24;
  }
  pos+=20+((len+3)&~3u);
 }
 if(patch<0||!gbe)return 7;
 b[patch]=(unsigned char)argv[3][3];
 f=fopen(argv[2],"wb");if(!f)return 8;
 if(fwrite(b,1,n,f)!=n){fclose(f);return 9;}if(fclose(f))return 10;
 printf("Hardware WAN selected: %s\n",argv[3]);return 0;
}
