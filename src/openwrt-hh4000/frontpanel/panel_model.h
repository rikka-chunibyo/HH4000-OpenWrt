/* SPDX-License-Identifier: GPL-2.0-only */
/* UI model/protocol; hardware access remains in frontpanel.c. */
#define MAX_PAGES 16
#define MAX_OPS 48
#define MAX_PACKET 8192
#define MAX_TEXT 256
#define MAX_VARS 64
#define MAX_CUSTOM 16
struct page {
	char id[40], label[MAX_TEXT + 1], line[4][MAX_TEXT + 1], align[8];
	int font;
};
struct config {
	int screen, brightness, idle, nav, rotate, hold_ms, reset_hold_ms, npages;
	char mode[16], title[MAX_TEXT + 1], line1[MAX_TEXT + 1], line2[MAX_TEXT + 1], main[80],
	        ok[80], up[80], down[80], actions[5][64], holds[5][64], startpage[40];
	struct page pages[MAX_PAGES];
};
struct state {
	int page, menu, selection, blank;
	unsigned events;
	char last[16], source[16];
	double activity;
};
struct variable {
	char key[40], value[MAX_TEXT + 1];
};
struct event {
	unsigned id;
	char button[8], gesture[12], action[64], source[16];
};
struct draw_op {
	char kind;
	int x, y, a, b, color, font, fill;
	char align[8], text[MAX_TEXT + 1];
};
struct frame {
	int count, nvars;
	struct draw_op op[MAX_OPS];
	struct variable vars[MAX_CUSTOM];
	char led[4][80], id[64];
};
static struct config cfg;
static struct state st;
static struct variable vars[MAX_VARS], custom[MAX_CUSTOM];
static int nvars, ncustom;
static struct event events[32];
static struct {
	int down, held;
	double since;
} keys[5];
static struct frame appframe;
static int app_has_frame, custom_active;
static double next_rotation;
static int page_cursor = -1, pages_started;
static char instance[64], api_error[80], rendered[4][513], led_colors[4][16];
static char ip[64] = "LAN starting", ports[128] = "Waiting for network",
            wifi1[128] = "Wi-Fi starting", wifi2[128] = "", health[16] = "pending";
static void ascii(char *out, size_t cap, const char *in)
{
	size_t i = 0;
	for (; in[i] && i + 1 < cap; i++) {
		unsigned char c = in[i];
		out[i] = (c >= 32 && c <= 126) ? c : '?';
	}
	out[i] = 0;
}
static int number(const char *v, int lo, int hi, int fallback)
{
	char *end;
	long n = strtol(v, &end, 10);
	return !*v || *end || n < lo || n > hi ? fallback : (int)n;
}
static int token_valid(const char *s)
{
	if (!*s || strlen(s) > 39)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.'))
			return 0;
	return 1;
}
static void setvar(const char *k, const char *v)
{
	int i;
	for (i = 0; i < nvars; i++)
		if (!strcmp(vars[i].key, k))
			break;
	if (i == MAX_VARS)
		return;
	if (i == nvars)
		nvars++;
	ascii(vars[i].key, sizeof(vars[i].key), k);
	ascii(vars[i].value, sizeof(vars[i].value), v);
}
static const char *getvar(const char *k)
{
	if (custom_active && !strcmp(cfg.mode, "python"))
		for (int i = 0; i < ncustom; i++)
			if (!strcmp(custom[i].key, k))
				return custom[i].value;
	for (int i = 0; i < nvars; i++)
		if (!strcmp(vars[i].key, k))
			return vars[i].value;
	return NULL;
}
static int truth(const char *v)
{
	return v && *v && strcmp(v, "0") && strcmp(v, "false") && strcmp(v, "off") &&
	       strcmp(v, "down") && strcmp(v, "pending") && strcmp(v, "error");
}
static void expand_depth(char *out, size_t cap, const char *in, int depth)
{
	size_t used = 0;
	out[0] = 0;
	for (size_t i = 0; in[i] && used + 1 < cap;) {
		if (in[i] == '{' && in[i + 1] == '{') {
			out[used++] = '{';
			i += 2;
			continue;
		}
		if (in[i] == '}' && in[i + 1] == '}') {
			out[used++] = '}';
			i += 2;
			continue;
		}
		if (in[i] != '{') {
			out[used++] = in[i++];
			continue;
		}
		const char *end = strchr(in + i + 1, '}');
		if (!end || end - (in + i) > 300) {
			out[used++] = in[i++];
			continue;
		}
		char expr[301];
		size_t n = (size_t)(end - (in + i + 1));
		memcpy(expr, in + i + 1, n);
		expr[n] = 0;
		char *q = strchr(expr, '?'), *fallback = strchr(expr, '|'), *value = NULL;
		const char *found;
		if (q) {
			*q++ = 0;
			char *colon = strchr(q, ':');
			if (colon) {
				*colon++ = 0;
				found = getvar(expr);
				value = (char *)(truth(found) ? q : colon);
			}
		} else if (fallback) {
			*fallback++ = 0;
			found = getvar(expr);
			value = (char *)(found && *found ? found : fallback);
		} else {
			found = getvar(expr);
			if (found)
				value = (char *)found;
		}
		if (!value) {
			size_t take = (size_t)(end - (in + i) + 1);
			if (take > cap - used - 1)
				take = cap - used - 1;
			memcpy(out + used, in + i, take);
			used += take;
		} else {
			char nested[513];
			if (depth < 4 && strchr(value, '{')) {
				expand_depth(nested, sizeof(nested), value, depth + 1);
				value = nested;
			}
			size_t take = strlen(value);
			if (take > cap - used - 1)
				take = cap - used - 1;
			memcpy(out + used, value, take);
			used += take;
		}
		i = (size_t)(end - in) + 1;
	}
	out[used] = 0;
}
static void expand(char *out, size_t cap, const char *in)
{
	expand_depth(out, cap, in, 0);
}
static int action_valid(const char *v)
{
	const char *a[] = {"previous", "next", "menu", "select", "home",
	                   "blank",    "wake", "none", "app"};
	for (unsigned i = 0; i < 9; i++)
		if (!strcmp(v, a[i]))
			return 1;
	return !strncmp(v, "page:", 5) && token_valid(v + 5);
}
static void add_page(const char *id, const char *label, const char *a, const char *b)
{
	if (cfg.npages == MAX_PAGES)
		return;
	struct page *p = &cfg.pages[cfg.npages++];
	memset(p, 0, sizeof(*p));
	ascii(p->id, sizeof(p->id), id);
	ascii(p->label, sizeof(p->label), label);
	ascii(p->line[0], sizeof(p->line[0]), a);
	ascii(p->line[1], sizeof(p->line[1]), b);
	strcpy(p->align, "center");
	p->font = 1;
}
static void defaults(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.screen = 1;
	cfg.brightness = 60;
	cfg.idle = 60;
	cfg.nav = 1;
	cfg.hold_ms = 1000;
	cfg.reset_hold_ms = 15000;
	strcpy(cfg.mode, "pages");
	strcpy(cfg.title, "OpenWrt HH4000");
	strcpy(cfg.line1, "Welcome");
	strcpy(cfg.main, "status");
	strcpy(cfg.ok, "white");
	strcpy(cfg.up, "white");
	strcpy(cfg.down, "white");
	const char *a[] = {"previous", "next", "select", "home"};
	for (int i = 0; i < 4; i++) {
		strcpy(cfg.actions[i], a[i]);
		strcpy(cfg.holds[i], "none");
	}
	strcpy(cfg.actions[4], "reboot");
	strcpy(cfg.holds[4], "factory-reset");
	add_page("home", "Home", "{title}", "{ip|LAN waiting}");
	add_page("lan", "LAN ports", "LAN 1 2 3 4 10G", "{ports}");
	add_page("wifi", "Wi-Fi", "{wifi1}", "{wifi2}");
	add_page("system", "System", "Up {uptime}", "Load {load} RAM {memory}%");
	add_page("message", "Message", "{message_line1}", "{message_line2}");
	page_cursor = -1;
	pages_started = 0;
}
static void config_item(const char *k, const char *v)
{
	if (!strcmp(k, "screen"))
		cfg.screen = number(v, 0, 1, 1);
	else if (!strcmp(k, "brightness"))
		cfg.brightness = number(v, 5, 100, 60);
	else if (!strcmp(k, "idle"))
		cfg.idle = number(v, 0, 3600, 60);
	else if (!strcmp(k, "navigation_leds"))
		cfg.nav = number(v, 0, 2, 1);
	else if (!strcmp(k, "rotate"))
		cfg.rotate = number(v, 0, 3600, 0);
	else if (!strcmp(k, "hold_ms"))
		cfg.hold_ms = number(v, 300, 5000, 1000);
	else if (!strcmp(k, "reset_hold_ms"))
		cfg.reset_hold_ms = number(v, 5000, 60000, 15000);
	else if (!strcmp(k, "mode")) {
		if (!strcmp(v, "pages") || !strcmp(v, "python"))
			ascii(cfg.mode, sizeof(cfg.mode), v);
	} else if (!strcmp(k, "startpage"))
		ascii(cfg.startpage, sizeof(cfg.startpage), v);
	else if (!strcmp(k, "title"))
		ascii(cfg.title, sizeof(cfg.title), v);
	else if (!strcmp(k, "line1"))
		ascii(cfg.line1, sizeof(cfg.line1), v);
	else if (!strcmp(k, "line2"))
		ascii(cfg.line2, sizeof(cfg.line2), v);
	else if (!strcmp(k, "main_led"))
		ascii(cfg.main, sizeof(cfg.main), v);
	else if (!strcmp(k, "ok_led"))
		ascii(cfg.ok, sizeof(cfg.ok), v);
	else if (!strcmp(k, "up_led"))
		ascii(cfg.up, sizeof(cfg.up), v);
	else if (!strcmp(k, "down_led"))
		ascii(cfg.down, sizeof(cfg.down), v);
	else if (!strcmp(k, "pages_begin")) {
		cfg.npages = 0;
		pages_started = 1;
		page_cursor = -1;
	} else if (!strcmp(k, "page")) {
		page_cursor = -1;
		if (token_valid(v) && cfg.npages < MAX_PAGES) {
			for (int i = 0; i < cfg.npages; i++)
				if (!strcmp(cfg.pages[i].id, v))
					return;
			add_page(v, v, "", "");
			page_cursor = cfg.npages - 1;
		}
	} else if (!strncmp(k, "page_", 5) && page_cursor >= 0) {
		struct page *p = &cfg.pages[page_cursor];
		if (!strcmp(k, "page_label"))
			ascii(p->label, sizeof(p->label), v);
		else if (!strcmp(k, "page_font"))
			p->font = !strcmp(v, "small") ? 0 : !strcmp(v, "large") ? 2 : 1;
		else if (!strcmp(k, "page_align") &&
		         (!strcmp(v, "left") || !strcmp(v, "center") || !strcmp(v, "right")))
			ascii(p->align, sizeof(p->align), v);
		else
			for (int i = 0; i < 4; i++) {
				char key[32];
				snprintf(key, sizeof(key), "page_line%d", i + 1);
				if (!strcmp(k, key))
					ascii(p->line[i], sizeof(p->line[i]), v);
			}
	} else {
		const char *key[] = {"up", "down", "ok", "main", "reset"};
		for (int i = 0; i < 5; i++) {
			char name[32];
			snprintf(name, sizeof(name), "%s_action", key[i]);
			if (!strcmp(k, name) &&
			    (action_valid(v) || (i == 4 && !strcmp(v, "reboot"))))
				ascii(cfg.actions[i], sizeof(cfg.actions[i]), v);
			snprintf(name, sizeof(name), "%s_hold", key[i]);
			if (!strcmp(k, name) &&
			    (action_valid(v) || (i == 4 && !strcmp(v, "factory-reset"))))
				ascii(cfg.holds[i], sizeof(cfg.holds[i]), v);
		}
	}
}
static int page_index(const char *id)
{
	for (int i = 0; i < cfg.npages; i++)
		if (!strcmp(id, cfg.pages[i].id))
			return i;
	return -1;
}
static void load_config(void)
{
	char old_mode[16];
	ascii(old_mode, sizeof(old_mode), cfg.mode);
	defaults();
	FILE *f = fopen(configpath, "r");
	if (f) {
		char line[600];
		while (fgets(line, sizeof(line), f)) {
			char *v = strchr(line, '\t');
			if (!v)
				continue;
			*v++ = 0;
			v[strcspn(v, "\r\n")] = 0;
			config_item(line, v);
		}
		fclose(f);
	}
	if (!cfg.npages)
		add_page("home", "Home", "{title}", "No pages enabled");
	if (st.page >= cfg.npages)
		st.page = 0;
	if (st.selection >= cfg.npages)
		st.selection = 0;
	if (strcmp(old_mode, cfg.mode)) {
		app_has_frame = custom_active = 0;
		memset(&appframe, 0, sizeof(appframe));
		st.menu = st.blank = 0;
	}
	next_rotation = monotime() + cfg.rotate;
}
static void action(const char *name, double now)
{
	if (!strcmp(name, "factory-reset") || !strcmp(name, "reboot"))
		return;
	if (!strcmp(name, "none"))
		return;
	st.activity = now;
	next_rotation = now + cfg.rotate;
	if (!strcmp(name, "app"))
		return;
	if (!strcmp(name, "wake")) {
		st.blank = 0;
		return;
	}
	if (!strcmp(name, "blank")) {
		st.blank = !st.blank;
		return;
	}
	if (st.blank) {
		st.blank = 0;
		return;
	}
	if (!strcmp(name, "home")) {
		st.menu = 0;
		int p = page_index(cfg.startpage);
		st.page = p < 0 ? 0 : p;
		return;
	}
	if (!strncmp(name, "page:", 5)) {
		int p = page_index(name + 5);
		if (p >= 0) {
			st.menu = 0;
			st.page = p;
		}
		return;
	}
	if (!strcmp(name, "menu")) {
		st.selection = st.page;
		st.menu = !st.menu;
		return;
	}
	if (!strcmp(name, "select")) {
		if (st.menu) {
			st.page = st.selection;
			st.menu = 0;
		} else {
			st.menu = 1;
			st.selection = st.page;
		}
		return;
	}
	int step = !strcmp(name, "previous") ? -1 : 1;
	if (st.menu)
		st.selection = (st.selection + cfg.npages + step) % cfg.npages;
	else
		st.page = (st.page + cfg.npages + step) % cfg.npages;
}
static void emit_event(int index, const char *gesture, const char *act, const char *source)
{
	const char *names[] = {"up", "down", "ok", "main", "reset"};
	st.events++;
	ascii(st.last, sizeof(st.last), names[index]);
	ascii(st.source, sizeof(st.source), source);
	struct event *e = &events[(st.events - 1) % 32];
	memset(e, 0, sizeof(*e));
	e->id = st.events;
	ascii(e->button, sizeof(e->button), names[index]);
	ascii(e->gesture, sizeof(e->gesture), gesture);
	ascii(e->action, sizeof(e->action), act);
	ascii(e->source, sizeof(e->source), source);
}
static void button(int index, double now, const char *source)
{
	const char *a = index == 4 && !strcmp(cfg.actions[4], "reboot") ? "reboot"
	                : !strcmp(cfg.mode, "python")                   ? "app"
	                                                                : cfg.actions[index];
	emit_event(index, "tap", a, source);
	if (!strcmp(cfg.mode, "pages"))
		action(a, now);
}
static void key_level(int index, int down, double now)
{
	if (down && !keys[index].down) {
		keys[index].down = 1;
		keys[index].held = 0;
		keys[index].since = now;
		st.activity = now;
		emit_event(index, "press", "none", "hardware");
	} else if (!down && keys[index].down) {
		keys[index].down = 0;
		if (!keys[index].held) {
			if (index == 4 && now - keys[index].since >= 1)
				emit_event(index, "cancel", "none", "hardware");
			else
				button(index, now, "hardware");
		}
		emit_event(index, "release", "none", "hardware");
	}
}
static void key_tick(double now)
{
	for (int i = 0; i < 5; i++)
		if (keys[i].down && !keys[i].held &&
		    now - keys[i].since >= (i == 4 ? cfg.reset_hold_ms : cfg.hold_ms) / 1000.0) {
			keys[i].held = 1;
			const char *a = i == 4 && !strcmp(cfg.holds[i], "factory-reset")
			                        ? "factory-reset"
			                : !strcmp(cfg.mode, "python") ? "app"
			                                              : cfg.holds[i];
			emit_event(i, "hold", a, "hardware");
			if (!strcmp(cfg.mode, "pages"))
				action(a, now);
		}
}
static void update_reset_vars(void)
{
	FILE *f = fopen("/var/run/hh-reset/variables", "r");
	if (f) {
		char key[40], value[40];
		while (fscanf(f, "%39s %39s", key, value) == 2)
			if (!strcmp(key, "reset_pressed") || !strcmp(key, "reset_held") ||
			    !strcmp(key, "reset_remaining") || !strcmp(key, "reset_armed"))
				setvar(key, value);
		fclose(f);
	}
}
static void update_system_vars(void)
{
	double up = 0, load = 0;
	unsigned long total = 0, available = 0, free_mem = 0, buffers = 0, cached = 0;
	FILE *f = fopen("/proc/uptime", "r");
	if (f) {
		if (fscanf(f, "%lf", &up) != 1)
			up = 0;
		fclose(f);
	}
	f = fopen("/proc/loadavg", "r");
	if (f) {
		if (fscanf(f, "%lf", &load) != 1)
			load = 0;
		fclose(f);
	}
	f = fopen("/proc/meminfo", "r");
	if (f) {
		char line[100];
		while (fgets(line, sizeof(line), f)) {
			unsigned long v;
			if (sscanf(line, "MemTotal: %lu", &v) == 1)
				total = v;
			else if (sscanf(line, "MemAvailable: %lu", &v) == 1)
				available = v;
			else if (sscanf(line, "MemFree: %lu", &v) == 1)
				free_mem = v;
			else if (sscanf(line, "Buffers: %lu", &v) == 1)
				buffers = v;
			else if (sscanf(line, "Cached: %lu", &v) == 1)
				cached = v;
		}
		fclose(f);
	}
	if (!available)
		available = free_mem + buffers + cached;
	if (available > total)
		available = total;
	char b[128];
	unsigned long mins = (unsigned long)up / 60;
	snprintf(b, sizeof(b), "%lud %02lu:%02lu", mins / 1440, mins / 60 % 24, mins % 60);
	setvar("uptime", b);
	snprintf(b, sizeof(b), "%lu", (unsigned long)up);
	setvar("uptime_seconds", b);
	snprintf(b, sizeof(b), "%.2f", load);
	setvar("load", b);
	snprintf(b, sizeof(b), "%lu", total ? (total - available) * 100 / total : 0);
	setvar("memory", b);
	snprintf(b, sizeof(b), "%lu MB", available / 1024);
	setvar("free_memory", b);
	f = fopen("/proc/sys/kernel/hostname", "r");
	if (f) {
		if (fgets(b, sizeof(b), f)) {
			b[strcspn(b, "\r\n")] = 0;
			setvar("hostname", b);
		}
		fclose(f);
	}
	f = fopen("/tmp/TZ", "r");
	if (f) {
		if (fgets(b, sizeof(b), f)) {
			b[strcspn(b, "\r\n")] = 0;
			const char *old = getenv("TZ");
			if (!old || strcmp(old, b)) {
				setenv("TZ", b, 1);
				tzset();
			}
		}
		fclose(f);
	}
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	if (tm) {
		strftime(b, sizeof(b), "%H:%M:%S", tm);
		setvar("time", b);
		strftime(b, sizeof(b), "%Y-%m-%d", tm);
		setvar("date", b);
	}
}
static void read_status(void)
{
	nvars = 0;
	double stamp = 0;
	strcpy(health, "pending");
	ip[0] = ports[0] = wifi1[0] = wifi2[0] = 0;
	FILE *f = fopen(DIRPATH "/network", "r");
	if (f) {
		char line[600];
		while (fgets(line, sizeof(line), f)) {
			char *v = strchr(line, '\t');
			if (!v)
				continue;
			*v++ = 0;
			v[strcspn(v, "\r\n")] = 0;
			if (!strcmp(line, "stamp"))
				stamp = atof(v);
			else if (token_valid(line))
				setvar(line, v);
		}
		fclose(f);
	}
	const char *v = getvar("health");
	if (v)
		ascii(health, sizeof(health), v);
	if (monotime() - stamp > 25)
		strcpy(health, "error");
	v = getvar("ip");
	if (v)
		ascii(ip, sizeof(ip), v);
	v = getvar("ports");
	if (v)
		ascii(ports, sizeof(ports), v);
	v = getvar("wifi1");
	if (v)
		ascii(wifi1, sizeof(wifi1), v);
	v = getvar("wifi2");
	if (v)
		ascii(wifi2, sizeof(wifi2), v);
	setvar("status", health);
	setvar("health", health);
	setvar("status_color", !strcmp(health, "ready")   ? "white"
	                       : !strcmp(health, "error") ? "red"
	                                                  : "blue");
	setvar("title", cfg.title);
	setvar("message_line1", cfg.line1);
	setvar("message_line2", cfg.line2);
	setvar("screen_on", !st.blank && cfg.screen ? "1" : "0");
	setvar("page", cfg.pages[st.page].id);
	setvar("button", st.last);
	update_system_vars();
	update_reset_vars();
}
static void pixel(int x, int y, int color)
{
	if (x >= 0 && x < W && y >= 0 && y < H)
		pixels[y][x] = (unsigned char)(color * cfg.brightness / 100);
}
static void draw_text(int x, int y, const char *text, int font, const char *align, int color)
{
	int cw = font == 0 ? 4 : font == 2 ? 16 : 8, ch = font == 0 ? 6 : font == 2 ? 32 : 16;
	size_t n = strlen(text);
	int width = (int)n * cw;
	if (!strcmp(align, "center"))
		x -= width / 2;
	else if (!strcmp(align, "right"))
		x -= width;
	for (size_t k = 0; k < n; k++) {
		if (x + (int)k * cw >= W || x + (int)(k + 1) * cw <= 0)
			continue;
		unsigned c = (unsigned char)text[k];
		if (c < 32 || c > 126)
			c = '?';
		for (int r = 0; r < ch; r++)
			for (int col = 0; col < cw; col++) {
				int ink = 0;
				if (font == 0) {
					ink = font4x6[c][r] & (128 >> col);
				} else
					ink = font8x16[c][r / (font == 2 ? 2 : 1)] &
					      (128 >> (col / (font == 2 ? 2 : 1)));
				if (ink)
					pixel(x + (int)k * cw + col, y + r, color);
			}
	}
}
static void draw_line(int x, int y, int x2, int y2, int color)
{
	int dx = abs(x2 - x), sx = x < x2 ? 1 : -1, dy = -abs(y2 - y), sy = y < y2 ? 1 : -1,
	    err = dx + dy;
	for (;;) {
		pixel(x, y, color);
		if (x == x2 && y == y2)
			break;
		int e = 2 * err;
		if (e >= dy) {
			err += dy;
			x += sx;
		}
		if (e <= dx) {
			err += dx;
			y += sy;
		}
	}
}
static void draw_rect(int x, int y, int w, int h, int color, int fill)
{
	if (fill) {
		int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > W ? W : x + w,
		    y1 = y + h > H ? H : y + h;
		for (int r = y0; r < y1; r++)
			for (int c = x0; c < x1; c++)
				pixel(c, r, color);
	} else {
		draw_line(x, y, x + w - 1, y, color);
		draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
		draw_line(x, y, x, y + h - 1, color);
		draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
	}
}
static void render(void)
{
	memset(pixels, 0, sizeof(pixels));
	memset(rendered, 0, sizeof(rendered));
	if (st.blank || !cfg.screen)
		return;
	if (st.menu && !strcmp(cfg.mode, "pages")) {
		char label[513];
		expand(label, sizeof(label), cfg.pages[st.selection].label);
		snprintf(rendered[0], sizeof(rendered[0]), "> %.20s", label);
		strcpy(rendered[1], "OK choose MAIN back");
		draw_rect(0, 2, W, 16, 255, 1);
		draw_text(W / 2, 2, rendered[0], 1, "center", 0);
		draw_text(W / 2, 24, rendered[1], 1, "center", 255);
		return;
	}
	if (!strcmp(cfg.mode, "python")) {
		if (!app_has_frame) {
			strcpy(rendered[0], "Python mode");
			strcpy(rendered[1], "Waiting for program");
			draw_text(W / 2, 2, rendered[0], 1, "center", 255);
			draw_text(W / 2, 24, rendered[1], 1, "center", 255);
			return;
		}
		int text_row = 0;
		for (int i = 0; i < appframe.count; i++) {
			struct draw_op *o = &appframe.op[i];
			if (o->kind == 'T') {
				char text[513];
				expand(text, sizeof(text), o->text);
				if (text_row < 4)
					ascii(rendered[text_row++], sizeof(rendered[0]), text);
				draw_text(o->x, o->y, text, o->font, o->align, o->color);
			} else if (o->kind == 'L')
				draw_line(o->x, o->y, o->a, o->b, o->color);
			else if (o->kind == 'R')
				draw_rect(o->x, o->y, o->a, o->b, o->color, o->fill);
			else
				pixel(o->x, o->y, o->color);
		}
		return;
	}
	struct page *p = &cfg.pages[st.page];
	int rows = p->font == 0 ? 4 : p->font == 2 ? 1 : 2;
	for (int i = 0; i < rows; i++) {
		expand(rendered[i], sizeof(rendered[i]), p->line[i]);
		int x = !strcmp(p->align, "center") ? W / 2 : !strcmp(p->align, "right") ? W : 0;
		int y = p->font == 0 ? i * 10 + 1 : p->font == 2 ? 5 : i * 22 + 2;
		draw_text(x, y, rendered[i], p->font, p->align, 255);
	}
}
static int lamp(const char *spec, int which, double now, char *resolved)
{
	char b[513];
	expand(b, sizeof(b), spec);
	char *pattern = strchr(b, ':');
	if (pattern)
		*pattern++ = 0;
	if (!strcmp(b, "status"))
		strcpy(b, !strcmp(health, "ready")   ? "white"
		          : !strcmp(health, "error") ? "red"
		          : which == 2               ? "amber"
		                                     : "blue");
	int on = 1;
	if (pattern) {
		if (!strcmp(pattern, "blink"))
			on = (int)(now * 2) % 2 == 0;
		else if (!strcmp(pattern, "fast"))
			on = (int)(now * 8) % 2 == 0;
		else if (!strcmp(pattern, "pulse")) {
			double phase = now - (long)now / 2 * 2;
			on = phase < .12 || (phase > .24 && phase < .36);
		} else if (!strcmp(pattern, "awake"))
			on = !st.blank && cfg.screen;
		else
			on = 0;
	}
	int valid = !strcmp(b, "white") ||
	            (which == 0 && (!strcmp(b, "blue") || !strcmp(b, "red"))) ||
	            (which == 2 && (!strcmp(b, "amber") || !strcmp(b, "red")));
	if (!valid || !on)
		strcpy(b, "off");
	ascii(resolved, 16, b);
	return strcmp(b, "off") != 0;
}
static void led_values(int *v)
{
	memset(v, 0, 8 * sizeof(*v));
	int python = !strcmp(cfg.mode, "python");
	const char *spec[] = {python ? "off" : cfg.main, python ? "off" : cfg.up,
	                      python ? "off" : cfg.ok, python ? "off" : cfg.down};
	for (int k = 0; k < 4; k++) {
		if (python && app_has_frame && appframe.led[k][0])
			spec[k] = appframe.led[k];
		lamp(spec[k], k, monotime(), led_colors[k]);
		if (!python && k && !(cfg.nav == 2 || (cfg.nav == 1 && !st.blank && cfg.screen)))
			strcpy(led_colors[k], "off");
	}
	if (!strcmp(led_colors[0], "white"))
		v[0] = 1;
	else if (!strcmp(led_colors[0], "red"))
		v[1] = 1;
	else if (!strcmp(led_colors[0], "blue"))
		v[2] = 1;
	v[3] = !strcmp(led_colors[1], "white");
	v[7] = !strcmp(led_colors[3], "white");
	if (!strcmp(led_colors[2], "white"))
		v[4] = 1;
	else if (!strcmp(led_colors[2], "red"))
		v[5] = 1;
	else if (!strcmp(led_colors[2], "amber"))
		v[6] = 1;
}
static int split(char *s, char **p, int max)
{
	int n = 0;
	while (n < max && (p[n] = strsep(&s, "\t")) != NULL)
		n++;
	return s ? -1 : n;
}
static int parse_frame(const char *packet, double now)
{
	char data[MAX_PACKET + 1];
	size_t length = strlen(packet);
	if (length > MAX_PACKET)
		return -1;
	memcpy(data, packet, length + 1);
	struct frame next;
	memset(&next, 0, sizeof(next));
	char *save, *line = strtok_r(data, "\n", &save), *p[10];
	if (!line)
		return -1;
	int count = split(line, p, 10);
	if (count != 2)
		return -1;
	int variable_only = !strcmp(p[0], "VARS");
	if (!variable_only && strcmp(p[0], "FRAME"))
		return -1;
	(void)now;
	if (!token_valid(p[1]))
		return -1;
	ascii(next.id, sizeof(next.id), p[1]);
	memcpy(next.led, appframe.led, sizeof(next.led));
	int ended = 0;
	while ((line = strtok_r(NULL, "\n", &save))) {
		if (!strcmp(line, "END")) {
			ended = 1;
			break;
		}
		count = split(line, p, 10);
		if (count < 1)
			return -1;
		if (!strcmp(p[0], "V")) {
			if (count != 3 || next.nvars == MAX_CUSTOM || strncmp(p[1], "app.", 4) ||
			    !token_valid(p[1]) || strlen(p[2]) > MAX_TEXT)
				return -1;
			struct variable *v = &next.vars[next.nvars++];
			ascii(v->key, sizeof(v->key), p[1]);
			ascii(v->value, sizeof(v->value), p[2]);
			continue;
		}
		if (variable_only)
			return -1;
		if (!strcmp(p[0], "LED")) {
			if (count != 3 || strlen(p[2]) >= 80)
				return -1;
			const char *names[] = {"main", "up", "ok", "down"};
			int k;
			for (k = 0; k < 4; k++)
				if (!strcmp(names[k], p[1]))
					break;
			if (k == 4)
				return -1;
			ascii(next.led[k], sizeof(next.led[k]), p[2]);
			continue;
		}
		if (next.count == MAX_OPS)
			return -1;
		struct draw_op *o = &next.op[next.count];
		if (count < 4)
			return -1;
		o->x = number(p[1], -512, 512, -9999);
		o->y = number(p[2], -512, 512, -9999);
		if (o->x == -9999 || o->y == -9999)
			return -1;
		if (!strcmp(p[0], "T")) {
			if (count != 7 || strlen(p[6]) > MAX_TEXT)
				return -1;
			o->kind = 'T';
			o->font = number(p[3], 0, 2, -1);
			if (o->font < 0)
				return -1;
			if (strcmp(p[4], "left") && strcmp(p[4], "right") && strcmp(p[4], "center"))
				return -1;
			ascii(o->align, sizeof(o->align), p[4]);
			o->color = number(p[5], 0, 255, -1);
			ascii(o->text, sizeof(o->text), p[6]);
		} else if (!strcmp(p[0], "P")) {
			if (count != 4)
				return -1;
			o->kind = 'P';
			o->color = number(p[3], 0, 255, -1);
		} else if (!strcmp(p[0], "L") || !strcmp(p[0], "R")) {
			int rect = p[0][0] == 'R';
			if (count != (rect ? 7 : 6))
				return -1;
			o->kind = rect ? 'R' : 'L';
			o->a = number(p[3], rect ? 1 : -512, 512, -9999);
			o->b = number(p[4], rect ? 1 : -512, 512, -9999);
			o->color = number(p[5], 0, 255, -1);
			o->fill = rect ? number(p[6], 0, 1, -1) : 0;
			if (o->a == -9999 || o->b == -9999 || o->fill < 0)
				return -1;
		} else
			return -1;
		if (o->color < 0)
			return -1;
		next.count++;
	}
	if (!ended || strtok_r(NULL, "\n", &save))
		return -1;
	if (next.nvars || variable_only) {
		ncustom = next.nvars;
		memcpy(custom, next.vars, ncustom * sizeof(custom[0]));
		custom_active = 1;
	}
	if (!variable_only) {
		appframe = next;
		app_has_frame = 1;
	}
	api_error[0] = 0;
	return 0;
}
static int control(const char *cmd, double now)
{
	if (!strcmp(cmd, "KEY\treset\t1")) {
		key_level(4, 1, now);
		return 0;
	}
	if (!strcmp(cmd, "KEY\treset\t0")) {
		key_level(4, 0, now);
		return 0;
	}
	if (!strncmp(cmd, "FRAME\t", 6) || !strncmp(cmd, "VARS\t", 5)) {
		if (strcmp(cfg.mode, "python")) {
			strcpy(api_error, "Select Python mode before sending frames");
			return -1;
		}
		int rc = parse_frame(cmd, now);
		if (rc)
			strcpy(api_error, "Invalid frame; previous frame retained");
		return rc;
	}
	if (!strcmp(cmd, "release")) {
		app_has_frame = custom_active = 0;
		memset(&appframe, 0, sizeof(appframe));
		return 0;
	}
	if (!strcmp(cmd, "sleep")) {
		st.blank = 1;
		return 0;
	}
	if (!strcmp(cfg.mode, "python")) {
		if (!strcmp(cmd, "wake")) {
			st.blank = 0;
			return 0;
		}
		if (!strcmp(cmd, "blank")) {
			st.blank = !st.blank;
			return 0;
		}
	}
	int p = page_index(cmd);
	if (p >= 0 && !strcmp(cfg.mode, "pages")) {
		st.blank = 0;
		st.menu = 0;
		st.page = p;
		st.activity = now;
		return 0;
	}
	const char *key[] = {"up", "down", "ok", "main", "reset"};
	for (int i = 0; i < 5; i++) {
		if (!strcmp(cmd, key[i])) {
			button(i, now, "control");
			return 0;
		}
		if (!strncmp(cmd, "hold:", 5) && !strcmp(cmd + 5, key[i])) {
			const char *a = i == 4 && !strcmp(cfg.holds[i], "factory-reset")
			                        ? "factory-reset"
			                : !strcmp(cfg.mode, "python") ? "app"
			                                              : cfg.holds[i];
			emit_event(i, "hold", a, "control");
			if (!strcmp(cfg.mode, "pages"))
				action(a, now);
			return 0;
		}
	}
	if (!strcmp(cfg.mode, "pages") && action_valid(cmd)) {
		action(cmd, now);
		return 0;
	}
	return -1;
}
static void json_string(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		if ((unsigned char)*s >= 32)
			fputc(*s, f);
	}
	fputc('"', f);
}
static void publish(void)
{
	FILE *f = fopen(DIRPATH "/state.new", "w");
	if (!f)
		return;
	fprintf(f, "{\"page\":");
	json_string(f, cfg.pages[st.page].id);
	fprintf(f, ",\"instance\":");
	json_string(f, instance);
	fprintf(f, ",\"menu\":%s,\"screen_on\":%s,\"events\":%u,\"last_button\":",
	        st.menu ? "true" : "false", !st.blank && cfg.screen ? "true" : "false", st.events);
	json_string(f, st.last);
	fprintf(f, ",\"last_source\":");
	json_string(f, st.source);
	fprintf(f, ",\"health\":");
	json_string(f, health);
	fprintf(f, ",\"ip\":");
	json_string(f, ip);
	fprintf(f, ",\"settings\":{\"title\":");
	json_string(f, cfg.title);
	fprintf(f, ",\"idle_timeout\":%d,\"brightness\":%d,\"main_led\":", cfg.idle,
	        cfg.brightness);
	json_string(f, cfg.main);
	fprintf(f, ",\"ok_led\":");
	json_string(f, cfg.ok);
	fprintf(f, ",\"up_action\":");
	json_string(f, cfg.actions[0]);
	fprintf(f, ",\"mode\":");
	json_string(f, cfg.mode);
	fprintf(f, "},\"main_input\":%s,\"framebuffer\":%s,\"app_frame_active\":%s,\"last_frame\":",
	        inputfd >= 0 ? "true" : "false", fb >= 0 ? "true" : "false",
	        app_has_frame && !strcmp(cfg.mode, "python") ? "true" : "false");
	json_string(f, appframe.id);
	fprintf(f, ",\"api_error\":");
	json_string(f, api_error);
	fprintf(f, ",\"lines\":[");
	for (int i = 0; i < 4; i++) {
		if (i)
			fputc(',', f);
		json_string(f, rendered[i]);
	}
	fprintf(f, "],\"variables\":{");
	for (int i = 0; i < nvars; i++) {
		if (i)
			fputc(',', f);
		json_string(f, vars[i].key);
		fputc(':', f);
		json_string(f, vars[i].value);
	}
	if (custom_active && !strcmp(cfg.mode, "python"))
		for (int i = 0; i < ncustom; i++) {
			if (nvars || i)
				fputc(',', f);
			json_string(f, custom[i].key);
			fputc(':', f);
			json_string(f, custom[i].value);
		}
	fprintf(f, "},\"pages\":[");
	for (int i = 0; i < cfg.npages; i++) {
		if (i)
			fputc(',', f);
		fprintf(f, "{\"id\":");
		json_string(f, cfg.pages[i].id);
		fprintf(f, ",\"label\":");
		char label[513];
		expand(label, sizeof(label), cfg.pages[i].label);
		json_string(f, label);
		fputc('}', f);
	}
	fprintf(f, "],\"event_log\":[");
	unsigned first = st.events > 32 ? st.events - 32 : 0;
	for (unsigned i = first; i < st.events; i++) {
		struct event *e = &events[i % 32];
		if (i != first)
			fputc(',', f);
		fprintf(f, "{\"id\":%u,\"button\":", e->id);
		json_string(f, e->button);
		fprintf(f, ",\"gesture\":");
		json_string(f, e->gesture);
		fprintf(f, ",\"action\":");
		json_string(f, e->action);
		fprintf(f, ",\"source\":");
		json_string(f, e->source);
		fputc('}', f);
	}
	fprintf(f, "],\"leds\":{");
	const char *names[] = {"main", "up", "ok", "down"};
	for (int i = 0; i < 4; i++) {
		if (i)
			fputc(',', f);
		json_string(f, names[i]);
		fputc(':', f);
		json_string(f, led_colors[i]);
	}
	fprintf(f, "},\"frame_hex\":\"");
	static const char hex[] = "0123456789abcdef";
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			unsigned v = pixels[y][x];
			fputc(hex[v >> 4], f);
			fputc(hex[v & 15], f);
		}
	fprintf(f, "\"}\n");
	fclose(f);
	rename(DIRPATH "/state.new", DIRPATH "/state.json");
}
