/* SPDX-License-Identifier: GPL-2.0-only */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <linux/fb.h>
/* The target glibc predates the build headers' time64 ioctl redirection. */
extern int ioctl(int, unsigned long, ...) __asm__("ioctl");
#include "font8x16.h"
#include "font4x6.h"
#define W 168
#define H 42
#ifndef DIRPATH
#define DIRPATH "/var/run/hh-frontpanel"
#endif
#define SOCKETPATH DIRPATH "/control"
static const unsigned led_ids[]={111,54,53,106,107,108,109,110};
static int leds=-1,fb=-1,screen_blank=-1,stride=W,inputfd=-1,simulate;
static unsigned char pixels[H][W],previous[H][W];
static volatile sig_atomic_t stopping,reload_requested;
static const char *configpath=DIRPATH "/config";
static double monotime(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void signal_handler(int n){if(n==SIGHUP)reload_requested=1;else stopping=1;}
#include "panel_model.h"
static int led_cache[8]={-1,-1,-1,-1,-1,-1,-1,-1};
static void led_write(int i,int value){if(leds<0||led_cache[i]==value)return;uint32_t v[3]={led_ids[i],(unsigned)value,0};if(ioctl(leds,0xc004cc06UL,v)==0)led_cache[i]=value;else perror("panel LED");}
static void update_leds(void){int v[8];led_values(v);for(int i=0;i<8;i++)if(!v[i])led_write(i,0);for(int i=0;i<8;i++)if(v[i])led_write(i,1);}
static int open_screen(void){fb=open("/dev/fb0",O_RDWR|O_CLOEXEC);if(fb<0)return -1;struct fb_var_screeninfo var;struct fb_fix_screeninfo fix;if(ioctl(fb,FBIOGET_VSCREENINFO,&var)||ioctl(fb,FBIOGET_FSCREENINFO,&fix)||var.xres!=W||var.yres!=H||var.bits_per_pixel!=8||fix.line_length<W){close(fb);fb=-1;errno=EINVAL;return -1;}stride=fix.line_length;return 0;}
static void present(void){if(fb<0)return;int blank=st.blank||!cfg.screen;if(!blank&&(screen_blank!=0||memcmp(previous,pixels,sizeof(pixels)))){if(stride==W){if(pwrite(fb,pixels,sizeof(pixels),0)!=(ssize_t)sizeof(pixels))perror("panel framebuffer write");}else{for(int y=0;y<H;y++)if(pwrite(fb,pixels[y],W,(off_t)y*stride)!=W){perror("panel framebuffer write");break;}}memcpy(previous,pixels,sizeof(pixels));}
 /* Stock driver: 0 means all pixels ON; 1 normal, 4 all pixels OFF. */
 if(screen_blank!=blank){if(ioctl(fb,FBIOBLANK,blank?4:1)<0)perror("panel blank");else screen_blank=blank;}}
static int main_input(void){DIR *d=opendir("/sys/class/input");if(!d)return -1;struct dirent *e;int fd=-1;while((e=readdir(d))){if(strncmp(e->d_name,"event",5))continue;char path[300],name[80];snprintf(path,sizeof(path),"/sys/class/input/%s/device/name",e->d_name);FILE *f=fopen(path,"r");if(!f)continue;int match=fgets(name,sizeof(name),f)&&!strncmp(name,"mcu-keys",8);fclose(f);if(match){snprintf(path,sizeof(path),"/dev/input/%s",e->d_name);fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);break;}}closedir(d);return fd;}
struct debounce {int candidate,stable;double since;};
static int debounce_update(struct debounce *d,int pressed,double now){if(pressed!=d->candidate){d->candidate=pressed;d->since=now;}if(d->stable!=pressed&&now-d->since>=.06){d->stable=pressed;return pressed;}return 0;}
static void mcu_event(unsigned type,unsigned code,int value,double now){if(type==1&&code==247&&value>=0&&value<=3)key_level(3,value!=0,now);}
static void poll_keys(double now){static struct debounce d[3];unsigned indices[]={8,9,10};for(int i=0;i<3;i++){uint32_t v[2]={indices[i],0};if(ioctl(leds,0xc004cc03UL,v))continue;debounce_update(&d[i],!!v[1],now);key_level(i,d[i].stable,now);}
 /* Use the verified ARM32 event layout, independent of build-host time ABI. */
 struct {int32_t sec,usec;uint16_t type,code;int32_t value;} e;
 while(inputfd>=0&&read(inputfd,&e,sizeof(e))==sizeof(e)){mcu_event(e.type,e.code,e.value,now);}
}
static int hardware_led_state(void){int fd=open("/dev/led",O_RDWR|O_CLOEXEC);if(fd<0)return 1;fputc('[',stdout);for(int i=0;i<8;i++){uint32_t v[3]={led_ids[i],0,0};if(ioctl(fd,0xc004cc07UL,v)){close(fd);return 1;}printf("%s%u",i?",":"",v[1]);}puts("]");close(fd);return 0;}
static int send_command(const char *cmd){if(strlen(cmd)>127)return 2;int fd=socket(AF_UNIX,SOCK_DGRAM,0);if(fd<0)return 1;struct sockaddr_un a={.sun_family=AF_UNIX};strcpy(a.sun_path,SOCKETPATH);int rc=sendto(fd,cmd,strlen(cmd),0,(struct sockaddr*)&a,sizeof(a));if(rc<0)perror("panel control");close(fd);return rc<0;}
/* ARM32 vendor glibc exports the legacy versioned stat entry points. */
static int runtime_lstat(const char *path,struct stat *value){
#if defined(__arm__) && !defined(__aarch64__)
 extern int __lxstat(int,const char *,struct stat *);
 return __lxstat(3,path,value);
#else
 return lstat(path,value);
#endif
}
static int runtime_private(void){struct stat value;return !runtime_lstat(DIRPATH,&value)&&S_ISDIR(value.st_mode)&&value.st_uid==geteuid()&&!(value.st_mode&0077);}
static int selftest(void){int count=0;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}count++;}while(0)
 defaults();memset(&st,0,sizeof(st));memset(keys,0,sizeof(keys));action("previous",1);CHECK(st.page==4);action("next",2);CHECK(st.page==0);action("select",3);CHECK(st.menu&&st.selection==0);action("next",4);action("select",5);CHECK(!st.menu&&st.page==1);action("blank",6);CHECK(st.blank);action("next",7);CHECK(!st.blank&&st.page==1);action("home",8);CHECK(st.page==0);CHECK(control("bad-command",9)==-1);CHECK(control("wifi",10)==0&&st.page==2);config_item("brightness","500");CHECK(cfg.brightness==60);config_item("title","A longer {wan} template");CHECK(!strcmp(cfg.title,"A longer {wan} template"));config_item("up_action","reboot");CHECK(!strcmp(cfg.actions[0],"previous"));
 char text[513];setvar("lan","192.168.1.1");setvar("wan","");setvar("wan_up","0");setvar("hostname","Test router");setvar("title","{hostname}");expand(text,sizeof(text),"LAN {lan}");CHECK(!strcmp(text,"LAN 192.168.1.1"));expand(text,sizeof(text),"{wan|waiting}");CHECK(!strcmp(text,"waiting"));expand(text,sizeof(text),"{wan_up?on:off}");CHECK(!strcmp(text,"off"));setvar("wan_up","1");expand(text,sizeof(text),"{wan_up?on:off}");CHECK(!strcmp(text,"on"));expand(text,sizeof(text),"{title}");CHECK(!strcmp(text,"Test router"));expand(text,sizeof(text),"{unknown}");CHECK(!strcmp(text,"{unknown}"));expand(text,sizeof(text),"{{lan}}");CHECK(!strcmp(text,"{lan}"));setvar("cycle","{cycle}");expand(text,sizeof(text),"{cycle}");CHECK(!strcmp(text,"{cycle}"));
 config_item("pages_begin","1");config_item("page","alpha");config_item("page_line1","A {lan}");config_item("page","beta");config_item("page_line1","B {wan|waiting}");CHECK(cfg.npages==2);st.page=0;action("previous",11);CHECK(st.page==1);action("next",12);CHECK(st.page==0);action("page:beta",13);CHECK(st.page==1);st.menu=0;st.blank=0;app_has_frame=0;render();CHECK(!strcmp(rendered[0],"B waiting"));CHECK(sizeof(pixels)==7056);
 cfg.screen=0;render();unsigned sum=0;for(int y=0;y<H;y++)for(int x=0;x<W;x++)sum+=pixels[y][x];CHECK(!sum);cfg.screen=1;
 struct debounce deb={0};CHECK(!debounce_update(&deb,1,1));CHECK(!debounce_update(&deb,0,1.02));CHECK(!debounce_update(&deb,1,2));CHECK(debounce_update(&deb,1,2.07));CHECK(!debounce_update(&deb,1,3));CHECK(!debounce_update(&deb,0,4));CHECK(!debounce_update(&deb,0,4.07));
 defaults();st.blank=0;st.page=0;st.menu=0;unsigned ev=st.events;mcu_event(1,247,2,20);mcu_event(1,247,2,20.1);mcu_event(1,247,1,20.2);mcu_event(1,247,0,20.3);CHECK(st.events==ev+3);CHECK(!strcmp(events[(st.events-2)%32].gesture,"tap"));strcpy(cfg.holds[3],"next");mcu_event(1,247,1,21);key_tick(22.1);CHECK(st.page==1);mcu_event(1,247,0,22.2);CHECK(st.events==ev+6);CHECK(!strcmp(events[(st.events-2)%32].gesture,"hold"));
 double now=monotime();strcpy(cfg.mode,"python");CHECK(!control("FRAME\ttest1\nT\t84\t2\t1\tcenter\t255\t{lan}\nR\t-4\t-4\t20\t20\t255\t1\nL\t-512\t-512\t512\t512\t255\nP\t0\t0\t255\nLED\tmain\tblue\nEND\n",now));CHECK(appframe.count==4&&app_has_frame);render();CHECK(pixels[0][0]>0);int leds_test[8];led_values(leds_test);CHECK(leds_test[2]==1&&leds_test[0]==0);CHECK(control("FRAME\tbad\nR\t0\t0\t999999\t20\t255\t1\nEND\n",now)==-1);CHECK(!strcmp(appframe.id,"test1"));CHECK(control("FRAME\tbad\nT\t0\t0\t1\twrong\t255\tx\nEND\n",now)==-1);CHECK(control("FRAME\tbad\nP\t0\t0\t255\n",now)==-1);CHECK(control("FRAME\tbad\nEND\nP\t0\t0\t255\n",now)==-1);CHECK(control("VARS\tbad\nV\twan\tspoofed\nEND\n",now)==-1);CHECK(!control("VARS\tvars1\nV\tapp.counter\t42\nEND\n",now));expand(text,sizeof(text),"Count {app.counter}");CHECK(!strcmp(text,"Count 42"));CHECK(!control("release",now));CHECK(app_has_frame==0);CHECK(action_valid("app")&&action_valid("page:alpha")&&!action_valid("page:a;reboot"));
 defaults();memset(keys,0,sizeof(keys));st.page=0;unsigned reset_events=st.events;CHECK(cfg.reset_hold_ms==15000);CHECK(!strcmp(cfg.holds[4],"factory-reset"));key_level(4,1,100);key_tick(114.999);CHECK(st.events==reset_events+1);key_tick(115);CHECK(st.events==reset_events+2&&!strcmp(events[(st.events-1)%32].button,"reset")&&!strcmp(events[(st.events-1)%32].action,"factory-reset"));key_tick(116);CHECK(st.events==reset_events+2);key_level(4,0,116);CHECK(st.page==0);CHECK(!control("KEY\treset\t1",120));CHECK(!control("KEY\treset\t0",120.4));CHECK(!strcmp(events[(st.events-2)%32].gesture,"tap"));
 key_level(4,1,130);key_level(4,0,135);CHECK(!strcmp(events[(st.events-2)%32].gesture,"cancel"));CHECK(st.page==0);
 printf("%d panel model/template/frame/gesture checks passed\n",count);return 0;}
int main(int argc,char **argv){double run_for=0;for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--check-runtime")){if(!runtime_private())return 1;puts("RUNTIME_PRIVATE_OK");return 0;}else if(!strcmp(argv[i],"--led-state"))return hardware_led_state();else if(!strcmp(argv[i],"--self-test"))return selftest();else if(!strcmp(argv[i],"--command")&&i+1<argc)return send_command(argv[i+1]);else if(!strcmp(argv[i],"--config")&&i+1<argc)configpath=argv[++i];else if(!strcmp(argv[i],"--simulate"))simulate=1;else if(!strcmp(argv[i],"--run-for")&&i+1<argc)run_for=atoi(argv[++i]);else{fprintf(stderr,"invalid argument\n");return 2;}}
 umask(077);
 snprintf(instance,sizeof(instance),"%ld-%.6f",(long)getpid(),monotime());if(mkdir(DIRPATH,0700)&&errno!=EEXIST){perror("panel runtime directory");return 1;}if(!runtime_private()){fprintf(stderr,"panel runtime directory must be private and owned by this user\n");return 1;}int lock=open(DIRPATH "/lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){perror("panel lock");return 1;}load_config();int initial_page=page_index(cfg.startpage);if(initial_page>=0)st.page=initial_page;
 if(!simulate){leds=open("/dev/led",O_RDWR|O_CLOEXEC);if(leds<0){perror("panel LED device");return 1;}uint32_t probe[3]={111,0,0};if(ioctl(leds,0xc004cc07UL,probe)<0){perror("panel GPIO interface");return 1;}if(open_screen()<0){perror("panel framebuffer");return 1;}inputfd=main_input();if(inputfd<0)fprintf(stderr,"main button input unavailable\n");}
 int sock=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(sock<0)return 1;struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,SOCKETPATH);unlink(SOCKETPATH);if(bind(sock,(struct sockaddr*)&addr,sizeof(addr))){perror("panel socket");return 1;}chmod(SOCKETPATH,0600);
 signal(SIGTERM,signal_handler);signal(SIGINT,signal_handler);signal(SIGHUP,signal_handler);double start=monotime(),next_update=0;st.activity=start;read_status();
 while(!stopping){double now=monotime();if(run_for>0&&now-start>=run_for)break;if(reload_requested){reload_requested=0;load_config();next_update=0;}char cmd[MAX_PACKET+2];ssize_t n;int drained=0;while(drained++<32&&(n=recv(sock,cmd,sizeof(cmd)-1,0))>0){if(n>MAX_PACKET||memchr(cmd,0,n)){strcpy(api_error,"Oversized or invalid packet");continue;}cmd[n]=0;if(control(cmd,now))fprintf(stderr,"ignored invalid panel command\n");next_update=0;}unsigned before=st.events;if(leds>=0)poll_keys(now);key_tick(now);if(!strcmp(cfg.mode,"pages")&&cfg.rotate&&!st.menu&&!st.blank&&now>=next_rotation){st.page=(st.page+1)%cfg.npages;next_rotation=now+cfg.rotate;next_update=0;}if(st.events!=before)next_update=0;if(!strcmp(cfg.mode,"pages")&&cfg.idle&&now-st.activity>=cfg.idle)st.blank=1;
 if(now>=next_update){read_status();render();present();update_leds();publish();next_update=now+1;}update_leds();usleep(20000);}
 st.blank=1;present();for(int i=0;i<8;i++)led_write(i,0);close(sock);unlink(SOCKETPATH);unlink(DIRPATH "/state.json");if(fb>=0)close(fb);if(leds>=0)close(leds);if(inputfd>=0)close(inputfd);close(lock);return 0;}
