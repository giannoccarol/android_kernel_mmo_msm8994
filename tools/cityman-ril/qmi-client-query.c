/* SPDX-License-Identifier: GPL-2.0-only */
/* QMI queries through qmuxd, without taking its SMD file descriptor. */
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef int (*init_fn)(void *,unsigned,void *,void *,void *,unsigned,void **);
typedef int (*raw_fn)(void *,unsigned,void *,unsigned,void *,unsigned,unsigned *,unsigned);
typedef int (*release_fn)(void *);
int main(int argc,char **argv)
{
 void *cci,*services,*object,*client=NULL;
 union {max_align_t align;unsigned char bytes[1024];} params={0};
 unsigned char response[8192],request[256];unsigned len=0,i,request_len=0;int ret;
 if(argc<3||argc>6){fprintf(stderr,"usage: %s SERVICE_OBJECT MESSAGE_ID [TLV_HEX [HOLD_SECONDS [POLL_MESSAGE]]]\n",argv[0]);return 2;}
 unsigned hold=0,poll_message=0;
 if(argc>=5){char *end;unsigned long n=strtoul(argv[4],&end,10);if(!*argv[4]||*end||n>3600)return 2;hold=n;}
 if(argc==6){char *end;unsigned long n=strtoul(argv[5],&end,0);if(!*argv[5]||*end||!n||n>65535||!hold)return 2;poll_message=n;}
 if(argc>=4){
  size_t count=strlen(argv[3]);
  if(count%2||count>sizeof(request)*2)return 2;
  for(i=0;i<count;i+=2){char pair[3]={argv[3][i],argv[3][i+1],0},*end;long n=strtol(pair,&end,16);if(*end||n<0||n>255)return 2;request[request_len++]=n;}
 }
 cci=dlopen("/vendor/lib64/libqmi_cci.so",RTLD_NOW|RTLD_GLOBAL);
 services=dlopen("/vendor/lib64/libqmiservices.so",RTLD_NOW|RTLD_GLOBAL);
 if(!cci||!services){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
 object=dlsym(services,argv[1]);
 init_fn init=(init_fn)dlsym(cci,"qmi_client_init_instance");
 raw_fn raw=(raw_fn)dlsym(cci,"qmi_client_send_raw_msg_sync");
 release_fn release=(release_fn)dlsym(cci,"qmi_client_release");
 if(!object||!init||!raw||!release)return 1;
 ret=init(object,0xffff,NULL,NULL,&params,8000,&client);
 printf("client_init=%d\n",ret);fflush(stdout);if(ret)return 1;
 char *message_end;unsigned long parsed_message=strtoul(argv[2],&message_end,0);
 if(!*argv[2]||*message_end||parsed_message>65535){release(client);return 2;}
 unsigned message=parsed_message;
 ret=raw(client,message,request_len?request:NULL,request_len,response,sizeof(response),&len,message==0x21?45000:10000);
 printf("request=%d bytes=%u\n",ret,len);
 if(len<=sizeof(response))for(i=0;i<len;i++)printf("%02x%s",response[i],(i%24==23||i+1==len)?"\n":" ");
 fflush(stdout);
 if(!ret&&hold){
  printf("Holding client for %u seconds\n",hold);fflush(stdout);
  while(hold){
   unsigned step=hold>5?5:hold;sleep(step);hold-=step;
   if(poll_message){
    len=0;ret=raw(client,poll_message,NULL,0,response,sizeof(response),&len,10000);
    printf("poll=0x%x request=%d bytes=%u\n",poll_message,ret,len);
    if(len<=sizeof(response))for(i=0;i<len;i++)printf("%02x%s",response[i],(i%24==23||i+1==len)?"\n":" ");
    fflush(stdout);if(ret)break;
   }
  }
 }
 release(client);return ret?1:0;
}
