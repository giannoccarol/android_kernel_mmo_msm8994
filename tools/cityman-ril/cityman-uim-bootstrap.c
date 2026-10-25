/* SPDX-License-Identifier: GPL-2.0-only */
/* Complete the Windows modem's primary USIM startup using the existing qmuxd.
 * No PIN handling, NV writes, card identifiers in logs, or raw SMD access.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

#define LOG(...) do { __android_log_print(ANDROID_LOG_INFO,"CitymanUIM",__VA_ARGS__); fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while(0)
typedef int (*init_fn)(void *,unsigned,void *,void *,void *,unsigned,void **);
typedef int (*raw_fn)(void *,unsigned,void *,unsigned,void *,unsigned,unsigned *,unsigned);
typedef int (*release_fn)(void *);
static raw_fn send_raw;
static uint8_t response[8192];static unsigned response_len;
static unsigned le16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static const uint8_t *tlv(unsigned tag,unsigned *len)
{
 for(unsigned i=0;i+3<=response_len;){
  unsigned n=le16(response+i+1);if(n>response_len-i-3)return NULL;
  if(response[i]==tag){*len=n;return response+i+3;}i+=n+3;
 }
 return NULL;
}
static int request(void *client,unsigned msg,const void *data,unsigned size)
{
 unsigned n;response_len=0;
 int ret=send_raw(client,msg,(void *)data,size,response,sizeof(response),&response_len,5000);
 if(ret||response_len>sizeof(response)){LOG("QMI message 0x%04x transport=%d",msg,ret);return -1;}
 const uint8_t *p=tlv(2,&n);
 if(!p||n!=4)return -1;
 if(le16(p)){LOG("QMI message 0x%04x error=%u",msg,le16(p+2));return -1;}
 return 0;
}
int main(int argc,char **argv)
{
 if(argc>2||(argc==2&&strcmp(argv[1],"--observe-only")&&strcmp(argv[1],"--activate-radio"))){fprintf(stderr,"usage: %s [--observe-only|--activate-radio]\n",argv[0]);return 2;}
 int observe=argc==2&&!strcmp(argv[1],"--observe-only");
 int radio=argc==2&&!strcmp(argv[1],"--activate-radio");
 char compatible[256]={0};FILE *f=fopen("/proc/device-tree/compatible","rb");
 if(!f)f=fopen("/sys/firmware/devicetree/base/compatible","rb");
 size_t count=0;if(f){count=fread(compatible,1,sizeof(compatible)-1,f);fclose(f);}
 int cityman=0;for(size_t i=0;i+11<=count;i++)if(!memcmp(compatible+i,"mmo,cityman",11))cityman=1;
 /* This legacy kernel does not expose its device tree as a filesystem. */
 if(!cityman){
  unsigned soc=0;f=fopen("/sys/devices/soc0/soc_id","r");if(f){if(fscanf(f,"%u",&soc)!=1)soc=0;fclose(f);}
  f=fopen("/proc/cmdline","r");compatible[0]=0;
  char cmdline[2048]={0};if(f){fread(cmdline,1,sizeof(cmdline)-1,f);fclose(f);}
  const char *index=strstr(cmdline,"androidboot.dtb_idx=1");
  cityman=soc==207&&index&&(index==cmdline||index[-1]==' ')&&(index[21]==0||index[21]==' '||index[21]=='\n')&&access("/vendor/lib64/libcityman_ds_target.so",R_OK)==0;
 }
 if(!cityman){LOG("Unsupported device; no QMI changes");return 1;}
 void *cci=dlopen("/vendor/lib64/libqmi_cci.so",RTLD_NOW|RTLD_GLOBAL);
 void *services=dlopen("/vendor/lib64/libqmiservices.so",RTLD_NOW|RTLD_GLOBAL);
 if(!cci||!services){LOG("Cannot load vendor QMI libraries");return 1;}
 init_fn init=(init_fn)dlsym(cci,"qmi_client_init_instance");
 send_raw=(raw_fn)dlsym(cci,"qmi_client_send_raw_msg_sync");
 release_fn release=(release_fn)dlsym(cci,"qmi_client_release");
 void *object=dlsym(services,"uim_qmi_idl_service_object_v01"),*client=NULL;
 if(!init||!send_raw||!release||!object)return 1;
 union {max_align_t align;uint8_t bytes[1024];} params[3]={0};
 void *nas=NULL,*dms=NULL;
 void *nas_object=dlsym(services,"nas_qmi_idl_service_object_v01");
 void *dms_object=dlsym(services,"dms_qmi_idl_service_object_v01");
 if(radio&&(!nas_object||!dms_object))return 1;
 for(unsigned attempt=0;attempt<30;attempt++){
  int ret=init(object,0xffff,NULL,NULL,&params[0],8000,&client);
  if(!ret)break;LOG("Waiting for UIM transport: %d",ret);sleep(2);
 }
 if(!client)return 1;
 int last=-1,done=1,radio_attempted=0;unsigned failures=0;
 for(;;){
  unsigned n;if(request(client,0x2f,NULL,0)){if(++failures==6)break;sleep(5);continue;}failures=0;
  const uint8_t *p=tlv(0x10,&n);
  if(!p||n<29||p[8]!=1||p[9]!=1||p[14]!=1||p[15]!=2||p[21]>32||n<29+(unsigned)p[21]){
   if(observe){LOG("No single present USIM application");break;}sleep(5);continue;
  }
  unsigned index=le16(p),state=p[16],perso=p[17],aid_len=p[21],pin=p[23+aid_len];
  int signature=state|(pin<<8)|(perso<<16);
  if(signature!=last){LOG("USIM state=%u PIN-state=%u personalization=%u",state,pin,perso);if(state==7)LOG("USIM ready");last=signature;}
  if(state==7){
   if(!radio){done=0;break;}
   if(!dms&&init(dms_object,0xffff,NULL,NULL,&params[1],8000,&dms)){sleep(5);continue;}
   if(!nas&&init(nas_object,0xffff,NULL,NULL,&params[2],8000,&nas)){sleep(5);continue;}
   /* Observe Android's radio power decision; never force DMS online. */
   if(request(dms,0x2d,NULL,0))break;
   const uint8_t *mode=tlv(1,&n);if(!mode||n!=1)break;
   if(*mode!=0)radio_attempted=0;
   else if(!radio_attempted){
    /* One activation attempt per ready/online transition, not a force loop. */
    radio_attempted=1;
    if(request(nas,0x5c,NULL,0))break;
    const uint8_t *mask=tlv(0x15,&n);if(!mask||n!=8)break;
    uint8_t active[11]={0x14,8,0};memcpy(active+3,mask,8);
    if(!(active[3]&1)){
     active[3]|=1;
     if(!request(nas,0x4b,active,sizeof(active)))LOG("Primary radio subscription enabled");
    }
   }
   sleep(5);continue;
  }
  if(nas){release(nas);nas=NULL;}
  if(dms){release(dms);dms=NULL;}
  radio_attempted=0;
  if(observe){done=0;break;}
  if(index==65535&&state==1){
   uint8_t activation[64]={1,2,0,0,1,0x10,0,0,1,0};
   if(!aid_len){LOG("Missing application identifier; no activation");break;}
   activation[6]=aid_len+2;activation[9]=aid_len;memcpy(activation+10,p+22,aid_len);
   if(!request(client,0x38,activation,10+aid_len))LOG("Primary GW USIM activated");
  }else if(index==0&&state==4&&perso==2&&(pin==2||pin==3)){
   /* Publish only after PIN verification/disablement and personalization ready. */
   const uint8_t ok[]={1,2,0,0,0,2,1,0,1};
   if(!request(client,0x40,ok,sizeof(ok)))LOG("Primary GW Subscription OK accepted");
  }
  /* PIN-required/PUK-required states are observed only; the user handles them. */
  sleep(state==2||state==3?5:2);
 }
 if(nas)release(nas);if(dms)release(dms);
 release(client);return done;
}
