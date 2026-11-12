#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define LED_DIR     "/sys/class/leds/asus:rgb:kbd_color"
#define INPUT_NAME  "Asus WMI hotkeys"
#define STATE_DIR   "/var/lib/tuf_kbd"
#define STATE_FILE  STATE_DIR "/state"
#define WAIT_SECS   30

struct rgb {
	unsigned int r, g, b;
};

struct preset {
	const char *name;
	struct rgb c;
};

static const struct preset presets[] = {
	{ "white",  { 255, 255, 255 } },
	{ "red",    { 255,   0,   0 } },
	{ "orange", { 255, 100,   0 } },
	{ "yellow", { 255, 200,   0 } },
	{ "green",  {   0, 255,   0 } },
	{ "cyan",   {   0, 255, 255 } },
	{ "blue",   {   0,   0, 255 } },
	{ "purple", { 140,   0, 255 } },
	{ "pink",   { 255,   0, 140 } },
};

#define NPRESETS (sizeof(presets) / sizeof(presets[0]))

static int find_dev(char *out, size_t len)
{
	if (access(LED_DIR "/multi_intensity", F_OK) || strlen(LED_DIR) >= len)
		return -1;

	strcpy(out, LED_DIR);
	return 0;
}

static int need_dev(char *out, size_t len)
{
	if (find_dev(out, len)) {
		fprintf(stderr, "tuf_kbdctl: keyboard not found, is the tuf_kbd module loaded?\n");
		return -1;
	}
	return 0;
}

static int read_attr(const char *dir, const char *name, char *buf, size_t len)
{
	char path[512];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;

	buf[n] = '\0';
	return 0;
}

static int write_attr(const char *dir, const char *name, const char *val)
{
	char path[512];
	size_t len = strlen(val);
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "tuf_kbdctl: %s: %s\n", path, strerror(errno));
		if (errno == EACCES)
			fprintf(stderr, "tuf_kbdctl: install the udev rule and join the video group, or use sudo\n");
		return -1;
	}

	n = write(fd, val, len);
	if (n < 0)
		fprintf(stderr, "tuf_kbdctl: %s: %s\n", path, strerror(errno));
	close(fd);

	return n == (ssize_t)len ? 0 : -1;
}

static int get_color(const char *dir, struct rgb *c)
{
	char buf[64];

	if (read_attr(dir, "multi_intensity", buf, sizeof(buf)))
		return -1;

	return sscanf(buf, "%u %u %u", &c->r, &c->g, &c->b) == 3 ? 0 : -1;
}

static int set_color(const char *dir, const struct rgb *c)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%u %u %u\n", c->r, c->g, c->b);
	return write_attr(dir, "multi_intensity", buf);
}

static int get_brightness(const char *dir, unsigned int *v)
{
	char buf[16];

	if (read_attr(dir, "brightness", buf, sizeof(buf)))
		return -1;

	return sscanf(buf, "%u", v) == 1 ? 0 : -1;
}

static int set_brightness(const char *dir, unsigned int v)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%u\n", v);
	return write_attr(dir, "brightness", buf);
}

static int parse_u8(const char *s, unsigned int *out)
{
	char *end;
	unsigned long v;

	if (!isdigit((unsigned char)*s))
		return -1;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || *end || v > 255)
		return -1;

	*out = v;
	return 0;
}

static int parse_hex(const char *s, struct rgb *c)
{
	unsigned long v;

	if (*s == '#')
		s++;

	if (strlen(s) != 6)
		return -1;

	for (int i = 0; i < 6; i++)
		if (!isxdigit((unsigned char)s[i]))
			return -1;

	v = strtoul(s, NULL, 16);
	c->r = (v >> 16) & 0xff;
	c->g = (v >> 8) & 0xff;
	c->b = v & 0xff;

	return 0;
}

static int parse_color(int argc, char **argv, struct rgb *c)
{
	if (argc == 3)
		return parse_u8(argv[0], &c->r) || parse_u8(argv[1], &c->g) ||
		       parse_u8(argv[2], &c->b) ? -1 : 0;

	if (argc != 1)
		return -1;

	for (size_t i = 0; i < NPRESETS; i++) {
		if (!strcasecmp(argv[0], presets[i].name)) {
			*c = presets[i].c;
			return 0;
		}
	}

	return parse_hex(argv[0], c);
}

static int preset_index(const struct rgb *c)
{
	for (size_t i = 0; i < NPRESETS; i++)
		if (presets[i].c.r == c->r && presets[i].c.g == c->g && presets[i].c.b == c->b)
			return i;

	return -1;
}

static int step_preset(const char *dir, int dir_step)
{
	struct rgb c = { 255, 255, 255 };
	int i;

	get_color(dir, &c);
	i = preset_index(&c);
	if (i < 0)
		i = dir_step > 0 ? -1 : 0;

	i = (i + dir_step + (int)NPRESETS) % (int)NPRESETS;

	return set_color(dir, &presets[i].c);
}

static int save_state(const char *dir)
{
	struct rgb c;
	unsigned int br;
	FILE *f;

	if (get_color(dir, &c) || get_brightness(dir, &br))
		return -1;

	mkdir(STATE_DIR, 0755);
	f = fopen(STATE_FILE ".tmp", "w");
	if (!f)
		return -1;

	fprintf(f, "%u %u %u %u\n", c.r, c.g, c.b, br);
	if (fclose(f))
		return -1;

	return rename(STATE_FILE ".tmp", STATE_FILE);
}

static int load_state(const char *dir)
{
	struct rgb c;
	unsigned int br;
	FILE *f;
	int n;

	f = fopen(STATE_FILE, "r");
	if (!f)
		return -1;

	n = fscanf(f, "%u %u %u %u", &c.r, &c.g, &c.b, &br);
	fclose(f);

	if (n != 4 || c.r > 255 || c.g > 255 || c.b > 255 || br > 255)
		return -1;

	if (set_color(dir, &c))
		return -1;

	return set_brightness(dir, br);
}

static int open_input(void)
{
	char path[32];
	char name[256];

	for (int i = 0; i < 64; i++) {
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;

		memset(name, 0, sizeof(name));
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
		    !strcmp(name, INPUT_NAME))
			return fd;

		close(fd);
	}

	return -1;
}

static int cmd_daemon(void)
{
	struct input_event ev[16];
	struct pollfd pfd[2];
	char dir[256];
	sigset_t mask;
	int fd = -1, sfd, ret = 0;

	for (int i = 0; i < WAIT_SECS; i++) {
		if (!find_dev(dir, sizeof(dir)))
			break;
		sleep(1);
	}

	if (find_dev(dir, sizeof(dir))) {
		fprintf(stderr, "tuf_kbdctl: keyboard not found\n");
		return 1;
	}

	if (load_state(dir))
		fprintf(stderr, "tuf_kbdctl: no saved state, keeping current color\n");

	for (int i = 0; i < WAIT_SECS && fd < 0; i++) {
		fd = open_input();
		if (fd < 0)
			sleep(1);
	}

	if (fd < 0) {
		fprintf(stderr, "tuf_kbdctl: \"%s\" input device not found\n", INPUT_NAME);
		return 1;
	}

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	sfd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (sfd < 0) {
		perror("tuf_kbdctl: signalfd");
		close(fd);
		return 1;
	}

	pfd[0] = (struct pollfd){ .fd = fd, .events = POLLIN };
	pfd[1] = (struct pollfd){ .fd = sfd, .events = POLLIN };

	fprintf(stderr, "tuf_kbdctl: running, Fn+Aura cycles colors\n");

	for (;;) {
		ssize_t n;

		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("tuf_kbdctl: poll");
			ret = 1;
			break;
		}

		if (pfd[1].revents)
			break;

		if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "tuf_kbdctl: input device gone\n");
			ret = 1;
			break;
		}

		n = read(fd, ev, sizeof(ev));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			perror("tuf_kbdctl: read");
			ret = 1;
			break;
		}

		for (size_t i = 0; i < n / sizeof(ev[0]); i++) {
			if (ev[i].type != EV_KEY || ev[i].code != KEY_PROG4 || ev[i].value != 1)
				continue;

			if (find_dev(dir, sizeof(dir)))
				continue;

			if (!step_preset(dir, 1))
				save_state(dir);
		}
	}

	if (!find_dev(dir, sizeof(dir)))
		save_state(dir);

	close(sfd);
	close(fd);

	return ret;
}

static int cmd_status(const char *dir)
{
	struct rgb c;
	unsigned int br;
	int i;

	if (get_color(dir, &c) || get_brightness(dir, &br)) {
		fprintf(stderr, "tuf_kbdctl: failed to read state\n");
		return 1;
	}

	i = preset_index(&c);
	printf("color       #%02x%02x%02x  (%u %u %u)%s%s\n", c.r, c.g, c.b,
	       c.r, c.g, c.b, i >= 0 ? "  " : "", i >= 0 ? presets[i].name : "");
	printf("brightness  %u\n", br);

	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: tuf_kbdctl <command>\n"
		"\n"
		"  color <hex | name | R G B>   set color (ff8800, #ff8800, red, 255 136 0)\n"
		"  brightness <0-255>           set brightness\n"
		"  next | prev                  cycle presets\n"
		"  status                       show current color and brightness\n"
		"  list                         show presets\n"
		"  save                         save current state (needs root)\n"
		"  daemon                       restore state and listen for Fn+Aura\n");
}

int main(int argc, char **argv)
{
	char dir[256];
	const char *cmd;

	if (argc < 2) {
		usage();
		return 1;
	}

	cmd = argv[1];

	if (!strcmp(cmd, "daemon"))
		return cmd_daemon();

	if (!strcmp(cmd, "list")) {
		for (size_t i = 0; i < NPRESETS; i++)
			printf("%-8s #%02x%02x%02x\n", presets[i].name,
			       presets[i].c.r, presets[i].c.g, presets[i].c.b);
		return 0;
	}

	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
		usage();
		return 0;
	}

	if (need_dev(dir, sizeof(dir)))
		return 1;

	if (!strcmp(cmd, "status"))
		return cmd_status(dir);

	if (!strcmp(cmd, "next"))
		return step_preset(dir, 1) ? 1 : 0;

	if (!strcmp(cmd, "prev"))
		return step_preset(dir, -1) ? 1 : 0;

	if (!strcmp(cmd, "save")) {
		if (save_state(dir)) {
			fprintf(stderr, "tuf_kbdctl: failed to write %s: %s\n", STATE_FILE, strerror(errno));
			return 1;
		}
		return 0;
	}

	if (!strcmp(cmd, "color")) {
		struct rgb c;

		if (parse_color(argc - 2, argv + 2, &c)) {
			fprintf(stderr, "tuf_kbdctl: bad color, try: ff8800, red, 255 136 0\n");
			return 1;
		}
		return set_color(dir, &c) ? 1 : 0;
	}

	if (!strcmp(cmd, "brightness")) {
		unsigned int v;

		if (argc != 3 || parse_u8(argv[2], &v)) {
			fprintf(stderr, "tuf_kbdctl: brightness must be 0-255\n");
			return 1;
		}
		return set_brightness(dir, v) ? 1 : 0;
	}

	usage();
	return 1;
}
