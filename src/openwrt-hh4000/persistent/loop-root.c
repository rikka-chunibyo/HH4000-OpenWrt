/* SPDX-License-Identifier: GPL-2.0-only */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
extern int ioctl(int, unsigned long, ...) __asm__("ioctl");
int main(void) {
 int image=open("/root.squashfs",O_RDONLY), loop=open("/dev/loop0",O_RDWR);
 if(image<0||loop<0){perror("open RAM root loop");return 1;}
 if(ioctl(loop,0x4c00UL,image)<0){perror("LOOP_SET_FD");return 1;}
 close(image);close(loop);return 0;
}
