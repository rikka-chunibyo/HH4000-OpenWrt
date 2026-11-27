/* SPDX-License-Identifier: GPL-2.0-only
 * Independent read-only physical RESET monitor. Only this hardware path requests erasure. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
extern int ioctl(int, unsigned long, ...) __asm__("ioctl");
#ifndef RESET_DIR
#define RESET_DIR "/var/run/hh-reset"
#endif
#ifndef PANEL_SOCKET
#define PANEL_SOCKET "/var/run/hh-frontpanel/control"
#endif
static volatile sig_atomic_t stopping;
static double mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static void stop(int sig)
{
	(void)sig;
	stopping = 1;
}
static int private_dir(void)
{
	struct stat st;
#if defined(__arm__) && !defined(__aarch64__)
	extern int __lxstat(int, const char *, struct stat *);
	int rc = __lxstat(3, RESET_DIR, &st);
#else
	int rc = lstat(RESET_DIR, &st);
#endif
	return !rc && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077);
}
static int read_config(int *threshold, int *factory, int *tap)
{
	FILE *f = fopen(RESET_DIR "/config", "r");
	if (!f)
		return -1;
	char key[40], value[80];
	int ms = 15000, reset = 1, reboot = 1;
	while (fscanf(f, "%39s %79s", key, value) == 2) {
		if (!strcmp(key, "hold_ms")) {
			char *end;
			long n = strtol(value, &end, 10);
			if (!*end && n >= 5000 && n <= 60000)
				ms = (int)n;
		} else if (!strcmp(key, "factory"))
			reset = !strcmp(value, "1");
		else if (!strcmp(key, "tap_reboot"))
			reboot = !strcmp(value, "1");
	}
	fclose(f);
	*threshold = ms;
	*factory = reset;
	*tap = reboot;
	return 0;
}
static int due(int armed, int level, int fired, double elapsed, int threshold)
{
	return armed && level && !fired && elapsed >= threshold / 1000.0;
}
static int short_press(int armed, int fired, double elapsed)
{
	return armed && !fired && elapsed >= 0 && elapsed < 1.0;
}
static pid_t request(const char *arg, int fd, int sock)
{
	pid_t pid = fork();
	if (pid == 0) {
		close(fd);
		close(sock);
		execl("/usr/sbin/hh-factory-reset", "hh-factory-reset", arg, (char *)NULL);
		_exit(127);
	}
	return pid;
}
static void send_key(int sock, int down)
{
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	if (sizeof(PANEL_SOCKET) > sizeof(a.sun_path))
		return;
	strcpy(a.sun_path, PANEL_SOCKET);
	const char *text = down ? "KEY\treset\t1" : "KEY\treset\t0";
	(void)sendto(sock, text, strlen(text), MSG_DONTWAIT, (struct sockaddr *)&a, sizeof(a));
}
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--check-runtime"))
		return private_dir() ? 0 : 1;
	if (argc == 2 && !strcmp(argv[1], "--self-test")) {
		if (due(1, 1, 0, 14.999, 15000) || !due(1, 1, 0, 15, 15000) ||
		    due(0, 1, 0, 30, 15000) || due(1, 0, 0, 30, 15000) || due(1, 1, 1, 30, 15000) ||
		    !due(1, 1, 0, 5, 5000))
			return 1;
		if (!short_press(1, 0, .46) || short_press(1, 0, 1) || short_press(1, 0, 14) ||
		    short_press(0, 0, .46) || short_press(1, 1, .46))
			return 1;
		puts("11 physical reset timing/arming/action checks passed");
		return 0;
	}
	if (argc != 1)
		return 2;
	umask(077);
	if (!private_dir()) {
		fputs("Reset runtime must be private\n", stderr);
		return 1;
	}
	int fd = open("/dev/led", O_RDONLY | O_CLOEXEC),
	    sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0 || sock < 0)
		return 1;
	uint32_t v[2] = {0, 0};
	if (ioctl(fd, 0xc004cc03UL, v))
		return 1;
	int stable = !!v[1], candidate = stable, armed = !stable, fired = 0, threshold = 15000,
	    factory = 1, tap = 1;
	double since = mono(), pressed = stable ? since : 0, next = 0;
	pid_t child = -1;
	int helper_rc = 0;
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	while (!stopping) {
		double t = mono();
		v[0] = v[1] = 0;
		if (ioctl(fd, 0xc004cc03UL, v)) {
			perror("Reset GPIO read");
			return 1;
		}
		int level = !!v[1];
		if (level != candidate) {
			candidate = level;
			since = t;
		}
		if (stable != candidate && t - since >= .06) {
			stable = candidate;
			fprintf(stderr, "HH_RESET_EDGE down=%d uptime=%.3f\n", stable, t);
			if (stable) {
				pressed = t;
				fired = 0;
			} else {
				if (tap && short_press(armed, fired, t - pressed)) {
					fprintf(stderr, "HH_RESET_SHORT_PRESS %.3f\n", t - pressed);
					child = request("--reboot", fd, sock);
				}
				armed = 1;
				fired = 0;
			}
			send_key(sock, stable);
			next = 0;
		}
		if (child > 0) {
			int status;
			pid_t done = waitpid(child, &status, WNOHANG);
			if (done == child) {
				helper_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
				child = -1;
			}
		}
		if (t >= next) {
			int ms = threshold, reset = factory, reboot = tap;
			if (read_config(&ms, &reset, &reboot) == 0 &&
			    (ms != threshold || reset != factory || reboot != tap)) {
				threshold = ms;
				factory = reset;
				tap = reboot;
				if (stable)
					armed = 0;
			}
			send_key(sock, stable);
			FILE *f = fopen(RESET_DIR "/state.new", "w");
			if (f) {
				double held = stable ? t - pressed : 0;
				fprintf(f,
				        "{\"pressed\":%s,\"armed\":%s,\"held_seconds\":%.1f,\"remaining_seconds\":%.1f,\"hold_ms\":%d,\"factory_reset\":%s,\"fired\":%s,\"helper_exit\":%d}\n",
				        stable ? "true" : "false", armed ? "true" : "false", held,
				        held < threshold / 1000.0 ? threshold / 1000.0 - held : 0,
				        threshold, factory ? "true" : "false",
				        fired ? "true" : "false", helper_rc);
				fclose(f);
				rename(RESET_DIR "/state.new", RESET_DIR "/state.json");
			}
			FILE *vf = fopen(RESET_DIR "/variables.new", "w");
			if (vf) {
				double held = stable ? t - pressed : 0;
				fprintf(vf,
				        "reset_pressed\t%d\nreset_held\t%.1f\nreset_remaining\t%.1f\nreset_armed\t%d\n",
				        stable, held,
				        held < threshold / 1000.0 ? threshold / 1000.0 - held : 0,
				        armed);
				fclose(vf);
				rename(RESET_DIR "/variables.new", RESET_DIR "/variables");
			}
			next = t + .25;
		}
		if (factory && due(armed, stable, fired, t - pressed, threshold)) {
			fired = 1;
			fprintf(stderr, "HH_RESET_LONG_HOLD %.3f\n", t - pressed);
			child = request("--request", fd, sock);
			if (child < 0) {
				helper_rc = 127;
				perror("Reset helper fork");
			}
			next = 0;
		}
		usleep(20000);
	}
	close(fd);
	close(sock);
	return 0;
}
