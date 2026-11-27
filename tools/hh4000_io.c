#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
extern int ioctl(int, unsigned long, ...) __asm__("ioctl");
#include <linux/input.h>
#include <time.h>
#include <signal.h>
static int fd;
static volatile sig_atomic_t stop;
static const unsigned ids[] = {0x6f, 0x36, 0x35, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e};
static const char *names[] = {"main-white", "main-red", "main-blue", "up-white",
                              "ok-white",   "ok-red",   "ok-yellow", "down-white"};
static int get(unsigned id)
{
	unsigned v[3] = {id, 0, 0};
	if (ioctl(fd, 0xc004cc07UL, v) < 0) {
		perror("gpio get");
		return -1;
	}
	return v[1];
}
static void set(unsigned id, unsigned v)
{
	unsigned x[3] = {id, v, 0};
	if (ioctl(fd, 0xc004cc06UL, x) < 0)
		perror("gpio set");
}
static void sig(int n)
{
	(void)n;
	stop = 1;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, 0, _IONBF, 0);
	fd = open("/dev/led", O_RDWR);
	if (fd < 0) {
		perror("led");
		return 1;
	}
	if (argc < 2)
		return 2;
	if (!strcmp(argv[1], "info")) {
		for (int i = 0; i < 8; i++)
			printf("LED %s id=%u state=%d\n", names[i], ids[i], get(ids[i]));
		for (unsigned i = 21; i <= 29; i++) {
			unsigned v[2] = {i, 0};
			int rc = ioctl(fd, 0xc004cc02UL, v);
			printf("managed LED %u=%u rc=%d\n", i, v[1], rc);
		}
		return 0;
	}
	if (!strcmp(argv[1], "scan")) {
		int saved[8];
		for (int i = 0; i < 8; i++)
			saved[i] = get(ids[i]);
		signal(SIGINT, sig);
		signal(SIGTERM, sig);
		for (int i = 0; i < 8 && !stop; i++) {
			for (int j = 0; j < 8; j++)
				set(ids[j], 0);
			set(ids[i], 1);
			printf("LED_PHASE %d %s\n", i, names[i]);
			sleep(4);
		}
		for (int i = 0; i < 8; i++)
			if (saved[i] >= 0)
				set(ids[i], saved[i]);
		puts("LED_SCAN_RESTORED");
		return 0;
	}
	if (!strcmp(argv[1], "watch")) {
		unsigned buttons[] = {0, 8, 9, 10};
		int previous[4] = {-1, -1, -1, -1};
		int ev = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
		signal(SIGINT, sig);
		signal(SIGTERM, sig);
		unsigned loops = 0;
		while (!stop && loops++ < 30000) {
			for (int i = 0; i < 4; i++) {
				unsigned v[2] = {buttons[i], 0};
				if (ioctl(fd, 0xc004cc03UL, v) == 0 && (int)v[1] != previous[i]) {
					printf("BUTTON index=%u state=%u\n", buttons[i], v[1]);
					previous[i] = v[1];
				}
			}
			struct {
				int32_t sec, usec;
				uint16_t type, code;
				int32_t value;
			} e;
			while (ev >= 0 && read(ev, &e, sizeof(e)) == sizeof(e))
				printf("INPUT type=%u code=%u value=%d\n", e.type, e.code, e.value);
			usleep(20000);
		}
		return 0;
	}
	return 2;
}
