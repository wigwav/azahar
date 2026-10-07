#include <stdio.h>
#include <stdlib.h>
#include "zstd.h"
int main(int c, char** v){ FILE* f=fopen(v[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,atol(v[3]),SEEK_SET); n-=atol(v[3]);
 char* in=malloc(n); fread(in,1,n,f); fclose(f);
 ZSTD_DStream* ds=ZSTD_createDStream(); ZSTD_initDStream(ds); FILE* o=fopen(v[2],"wb");
 size_t osz=ZSTD_DStreamOutSize(); char* ob=malloc(osz); ZSTD_inBuffer ib={in,n,0};
 while(ib.pos<ib.size){ ZSTD_outBuffer obuf={ob,osz,0}; size_t r=ZSTD_decompressStream(ds,&obuf,&ib); if(ZSTD_isError(r)){fprintf(stderr,"%s\n",ZSTD_getErrorName(r));return 1;} fwrite(ob,1,obuf.pos,o);} fclose(o); return 0;}
