/* Read-only HH4000 BSP reset-key logger. No GPIO writes or reset actions. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <string.h>
extern int ioctl(int,unsigned long,...) __asm__("ioctl");
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(void){int fd=open("/dev/led",O_RDONLY);if(fd<0){perror("open");return 1;}setvbuf(stdout,NULL,_IOLBF,0);uint32_t v[2]={0,0};if(ioctl(fd,0xc004cc03UL,v)){perror("reset read");return 1;}int stable=!!v[1],candidate=stable;double since=now(),start=since,pressed=0;printf("{\"ready\":true,\"bsp_index\":0,\"initial_pressed\":%s,\"uptime\":%.3f}\n",stable?"true":"false",start);while(now()-start<1800){v[0]=0;v[1]=0;if(ioctl(fd,0xc004cc03UL,v)){perror("reset read");return 1;}double t=now();int level=!!v[1];if(level!=candidate){candidate=level;since=t;}if(stable!=candidate&&t-since>=.06){stable=candidate;if(stable)pressed=t;printf("{\"button\":\"reset\",\"event\":\"%s\",\"uptime\":%.3f,\"duration\":%.3f}\n",stable?"press":"release",t,stable?0:pressed?t-pressed:0);}usleep(20000);}puts("{\"listener_expired\":true}");close(fd);return 0;}
