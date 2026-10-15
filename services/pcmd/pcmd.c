// SPDX-License-Identifier: BSD-2-Clause
/*
 * pcmd: plays audio from Linux on the DSP (the NET input). It reads
 * 16-bit stereo PCM from a pipe or file, resamples it to the DSP's 48 kHz
 * and writes it into the ring the DSP reads (struct dspaudio_ring). Writing
 * waits while the ring is full, so the DSP's clock sets the pace.
 *
 *   pcmd [-r rate] [path]    default 44100 Hz, /var/run/pcm
 *
 * A named pipe is opened again whenever its writer goes away. SIGUSR1
 * drops what is queued in the pipe and the ring (a pause or skip).
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "dspaudio.h"

#define DSP_HZ		48000
#define TAPS		32		/* per phase */
#define KAISER_BETA	8.0
#define CHUNK		1024		/* input frames per read */
#define FULL_WAIT_US	2000

static volatile struct dspaudio_ring *ring;
static volatile sig_atomic_t flush;

static double bessel_i0(double x)
{
	double s = 1.0, t = 1.0;
	int i;

	for (i = 1; i < 30; i++) {
		t *= (x / (2.0 * i)) * (x / (2.0 * i));
		s += t;
	}
	return s;
}

/* polyphase windowed sinc from rate to DSP_HZ: L phases, M input steps */
static unsigned int up, down;
static int16_t *coef;			/* [up][TAPS], Q15 */
static int16_t hist[2][TAPS];		/* the last input frames, oldest first */
static unsigned int phase;		/* output position within the input step */

static unsigned int gcd(unsigned int a, unsigned int b)
{
	while (b) {
		unsigned int t = a % b;

		a = b;
		b = t;
	}

	return a;
}

static int resampler_init(unsigned int rate)
{
	unsigned int g = gcd(rate, DSP_HZ), p, k;
	/* below the lower Nyquist frequency of the two, with some margin */
	double fc = 0.47 * (rate < DSP_HZ ? 1.0 : (double)DSP_HZ / rate);

	up = DSP_HZ / g;
	down = rate / g;
	coef = calloc((size_t)up * TAPS, sizeof(*coef));
	if (!coef) {
		return -1;
	}

	for (p = 0; p < up; p++) {
		double sum = 0, h[TAPS];

		for (k = 0; k < TAPS; k++) {
			/* distance of input tap k from this output, in input samples */
			double t = (double)k - (TAPS / 2 - 1) - (double)p / up;
			/* Kaiser window over the taps, -1..1 */
			double a = 2.0 * (t + TAPS / 2) / TAPS - 1.0, x = 2.0 * fc * t;
			double w = bessel_i0(KAISER_BETA * sqrt(fmax(0.0, 1.0 - a * a))) /
				   bessel_i0(KAISER_BETA);

			h[k] = (x == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x)) * w;
			sum += h[k];
		}

		for (k = 0; k < TAPS; k++) {
			coef[p * TAPS + k] = (int16_t)lround(h[k] / sum * 32767.0);
		}
	}

	return 0;
}

static void ring_put(int32_t l, int32_t r)
{
	uint32_t w, next;

	/* out of range until the DSP has set the ring up: wait as if full */
	for (;;) {
		w = ring->write;
		next = w + 1 == DSPAUDIO_RING_LEN ? 0 : w + 1;
		if (w < DSPAUDIO_RING_LEN && ring->read < DSPAUDIO_RING_LEN &&
		    next != ring->read) {
			break;
		}

		if (flush) {
			return;
		}
		usleep(FULL_WAIT_US);
	}

	ring->frame[w][0] = l;
	ring->frame[w][1] = r;
	__sync_synchronize();
	ring->write = next;
}

/* Q30 to 32 bit: overshoot past full scale (the sinc's ripple) clips */
static int32_t sat(int64_t acc)
{
	if (acc > INT32_MAX / 2) {
		return INT32_MAX;
	}
	if (acc < INT32_MIN / 2) {
		return INT32_MIN;
	}
	return (int32_t)acc * 2;
}

/* one input frame in, the output frames that fall before the next one out */
static void resample(const int16_t *in)
{
	unsigned int c, k;

	for (c = 0; c < 2; c++) {
		memmove(hist[c], hist[c] + 1, (TAPS - 1) * sizeof(hist[c][0]));
		hist[c][TAPS - 1] = in[c];
	}

	while (phase < up) {
		const int16_t *h = coef + phase * TAPS;
		int64_t acc[2] = { 0, 0 };

		for (k = 0; k < TAPS; k++) {
			acc[0] += (int32_t)hist[0][k] * h[k];
			acc[1] += (int32_t)hist[1][k] * h[k];
		}

		/* Q15 x 16 bit -> 32 bit, MSB aligned like the McASP's samples */
		ring_put(sat(acc[0]), sat(acc[1]));
		phase += down;
	}

	phase -= up;
}

static void on_flush(int sig)
{
	flush = 1;
}

/* empty the pipe and the ring, start the resampler from silence */
static void drop(int fd)
{
	char junk[4096];
	int fl = fcntl(fd, F_GETFL);
	uint32_t rd;

	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	while (read(fd, junk, sizeof(junk)) > 0) {
	}
	fcntl(fd, F_SETFL, fl);

	/* the DSP may move read meanwhile: until it holds still */
	do {
		rd = ring->read;
		ring->write = rd;
	} while (ring->read != rd);

	memset(hist, 0, sizeof(hist));
	phase = 0;
	flush = 0;
}

static int play(int fd)
{
	int16_t buf[CHUNK][2];
	size_t have = 0, i;
	ssize_t n;

	for (;;) {
		if (flush) {
			drop(fd);
			have = 0;
		}

		n = read(fd, (char *)buf + have, sizeof(buf) - have);
		if (n < 0 && errno == EINTR) {
			continue;
		}

		if (n <= 0) {
			return n < 0 ? -1 : 0;
		}

		have += n;
		for (i = 0; i < have / sizeof(buf[0]); i++) {
			resample(buf[i]);
		}

		/* keep a partial frame for the next read */
		memmove(buf, (char *)buf + i * sizeof(buf[0]), have - i * sizeof(buf[0]));
		have -= i * sizeof(buf[0]);
	}
}

int main(int argc, char **argv)
{
	const char *path = "/var/run/pcm";
	unsigned int rate = 44100;
	struct stat st;
	int opt, mem, fd;
	void *p;

	while ((opt = getopt(argc, argv, "r:")) != -1) {
		if (opt == 'r') {
			rate = strtoul(optarg, NULL, 10);
		} else {
			fprintf(stderr, "usage: %s [-r rate] [path]\n", argv[0]);
			return 1;
		}
	}

	if (optind < argc) {
		path = argv[optind];
	}

	openlog("pcmd", LOG_PID, LOG_DAEMON);
	/* no SA_RESTART: a blocked read returns to look at the flag */
	sigaction(SIGUSR1, &(struct sigaction){ .sa_handler = on_flush }, NULL);
	if (rate < 8000 || rate > 192000 || resampler_init(rate)) {
		fprintf(stderr, "%s: bad rate %u\n", argv[0], rate);
		return 1;
	}

	mem = open("/dev/mem", O_RDWR | O_SYNC);
	p = mem < 0 ? MAP_FAILED : mmap(NULL, sizeof(*ring), PROT_READ | PROT_WRITE, MAP_SHARED,
					 mem, DSPAUDIO_RING);
	if (p == MAP_FAILED) {
		perror("/dev/mem");
		return 1;
	}

	ring = p;
	/* whatever is left in the ring is old */
	ring->write = ring->read;

	if (!strcmp(path, "-")) {
		return play(STDIN_FILENO) ? 1 : 0;
	}

	if (stat(path, &st) && (mkfifo(path, 0666) || stat(path, &st))) {
		perror(path);
		return 1;
	}

	do {
		/* waits for a writer; a flush meanwhile has nothing to drop */
		do {
			flush = 0;
			fd = open(path, O_RDONLY);
		} while (fd < 0 && errno == EINTR);

		if (fd < 0) {
			syslog(LOG_ERR, "%s: %s", path, strerror(errno));
			return 1;
		}

		play(fd);
		close(fd);
	} while (S_ISFIFO(st.st_mode));

	return 0;
}
