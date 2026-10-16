// SPDX-License-Identifier: BSD-2-Clause
/*
 * pcmd: plays audio from Linux on the DSP (the NET input). It reads
 * 16-bit stereo PCM from a pipe or file, resamples it to the DSP's 48 kHz
 * and writes it into the ring the DSP reads (struct dspaudio_ring).
 *
 * A pipe's writer (AirPlay) keeps its own time, the DSP another: its
 * 48 kHz runs 67 ppm slow against the system clock. pcmd keeps the ring
 * full and about as much again in the pipe, and holds that level by
 * nudging the resampling ratio, so a writer on real time never waits.
 * A backlog far above that is skipped; for 5 s after a start only its
 * silence is. While the DSP is on another input, what comes in is
 * dropped. A file, stdin or a writer faster than real time has no clock
 * to follow: writing waits while the ring is full, and the DSP sets the
 * pace.
 *
 *   pcmd [-r rate] [path]    default 44100 Hz, /var/run/pcm
 *
 * A named pipe is opened again whenever its writer goes away. SIGUSR1
 * drops what is queued in the pipe and the ring (a pause or skip).
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "dspaudio.h"

#define DSP_HZ		48000
#define TAPS		32		/* per phase */
#define PHASES_MIN	160		/* per input frame: fine enough to interpolate */
#define KAISER_BETA	8.0
#define CHUNK		1024		/* input frames per read */
#define FULL_WAIT_US	2000
#define LEVEL_TARGET	(DSPAUDIO_RING_LEN * 2)	/* the level's top: 170 ms */
#define IDLE_S		0.05		/* full and still this long: the DSP is elsewhere */
#define NUDGE_MAX	1e-3		/* the ratio's largest change, 0.1 % */
#define DRIFT_MAX	5e-4		/* the clocks' largest difference */
#define KP		4.3e-3		/* per ring of level: 20 s to settle */
#define KI		5.4e-5		/* per ring and second: critically damped */
#define FAST_RATE	1.5		/* times real time: the writer outruns the DSP */
#define PIPE_FULL	(56 << 10)	/* bytes: a writer waits on the pipe (AirPlay's
					 * packets fill it to 45 KiB at most) */
#define HOLD_S		5		/* after a start or a fast writer: skip only silence */
#define SILENT		8		/* largest sample counted as silence */
#define PHASES_MAX	1024		/* larger tables for odd rates are refused */

static volatile struct dspaudio_ring *ring;
static unsigned int rate_hz;
static volatile sig_atomic_t flush;
static int in_fd;
static unsigned int pending;		/* input frames read, not resampled yet */
static int idle;			/* the DSP does not take from the ring */
static int fast;			/* the writer outruns the DSP: wait on the ring */
static uint32_t idle_read;

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

/*
 * Polyphase windowed sinc from rate to DSP_HZ: L phases, M input steps.
 * An output between two phases is interpolated from both; phase up is
 * phase 0 one input frame later.
 */
static unsigned int up, down;
static int16_t *coef;			/* [up + 1][TAPS], Q15 */
static int16_t hist[2][2 * TAPS];	/* the last input frames, twice over */
static unsigned int hist_pos;		/* hist + pos: the last TAPS, oldest first */
static uint64_t phase;			/* output position within the input step,
					 * phases << 32 */
static uint64_t step;			/* phases per output frame << 32 */

/* the ratio nudged by how much is queued: a PI loop of about 40 s */
static int pace;			/* a pipe: follow its clock */
static double nudge, nudge_i, level_top;

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
	k = (PHASES_MIN + up - 1) / up;
	up *= k;
	down *= k;
	if (up > PHASES_MAX) {
		return -1;
	}
	step = (uint64_t)down << 32;
	coef = calloc((size_t)(up + 1) * TAPS, sizeof(*coef));
	if (!coef) {
		return -1;
	}

	for (p = 0; p <= up; p++) {
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

static void tick(void);
static double now(void);

static void ring_put(int32_t l, int32_t r)
{
	uint32_t w, next;
	double since = 0;

	if (idle) {
		return;
	}

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
		if (pace) {
			/* the writer must not wait: AirPlay's packets fill the pipe in 250 ms */
			if (!since) {
				since = now();
			} else if (now() - since > IDLE_S) {
				idle = 1;
				idle_read = ring->read;
				return;
			}
			tick();
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
	const int16_t *x0, *x1;
	unsigned int k;

	hist[0][hist_pos] = hist[0][hist_pos + TAPS] = in[0];
	hist[1][hist_pos] = hist[1][hist_pos + TAPS] = in[1];
	hist_pos = hist_pos + 1 == TAPS ? 0 : hist_pos + 1;
	x0 = hist[0] + hist_pos;
	x1 = hist[1] + hist_pos;

	while (phase >> 32 < up) {
		const int16_t *h = coef + (phase >> 32) * TAPS, *h1 = h + TAPS;
		int32_t f = (phase >> 17) & 0x7FFF;	/* Q15, between h and h1 */
		int64_t a0 = 0, a1 = 0;

		for (k = 0; k < TAPS; k++) {
			int32_t c = (h[k] << 8) + ((h1[k] - h[k]) * f >> 7);	/* Q23 */

			a0 += (int64_t)c * x0[k];
			a1 += (int64_t)c * x1[k];
		}

		/* Q15 x 16 bit -> 32 bit, MSB aligned like the McASP's samples */
		ring_put(sat(a0 >> 8), sat(a1 >> 8));
		phase += step;
	}

	phase -= (uint64_t)up << 32;
}

static unsigned int ring_fill(void)
{
	uint32_t w = ring->write, r = ring->read;

	return w < DSPAUDIO_RING_LEN && r < DSPAUDIO_RING_LEN ?
	       (w - r + DSPAUDIO_RING_LEN) % DSPAUDIO_RING_LEN : 0;
}

/*
 * What is queued for the DSP, in its frames: the ring, the pipe and what
 * pcmd holds. When pcmd gets to run does not change it.
 */
static unsigned int level(void)
{
	int q = 0;

	ioctl(in_fd, FIONREAD, &q);
	return ring_fill() + (unsigned int)((q / 4 + pending) * (uint64_t)DSP_HZ / rate_hz);
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static double win_t;			/* the window the level's top is taken in */
static unsigned int win_top;

static void window_reset(void)
{
	win_t = 0;
	win_top = 0;
}

/* silence up to about the target's mean, the writer's bursts go on top */
static void prefill(void)
{
	unsigned int n;

	for (n = level(); n < LEVEL_TARGET - DSPAUDIO_RING_LEN / 8 && !flush && !idle; n++) {
		ring_put(0, 0);
	}

	level_top = LEVEL_TARGET;
	window_reset();
}

/*
 * The writer is late now and then, by up to 150 ms, never early: the
 * level's top in each second is when it was on time. That top, smoothed
 * over a few seconds, moves the ratio; above the target fewer frames come
 * out per input frame. The integral keeps the clocks' difference, through
 * pauses too.
 */
static double last_tick;

static void tick(void)
{
	static uint32_t last_read = UINT32_MAX;
	double t = now(), dt = t - last_tick, x, i, sum;
	unsigned int lv;
	uint32_t r;

	if (dt < 0.01) {
		return;
	}
	last_tick = t;

	/* the DSP stood still, pcmd waited for the writer or it outruns the DSP */
	r = ring->read;
	if (r == last_read || dt > 0.5 || fast) {
		last_read = r;
		window_reset();
		return;
	}
	last_read = r;

	lv = level();
	win_top = lv > win_top ? lv : win_top;
	win_t += dt;
	if (win_t < 1.0) {
		return;
	}

	level_top += (win_top - level_top) * 0.25;
	x = (level_top - LEVEL_TARGET) / DSPAUDIO_RING_LEN;
	/* the integral does not grow further into a limit */
	i = nudge_i + KI * x * win_t;
	sum = i + KP * x;
	if (!(sum > NUDGE_MAX && x > 0) && !(sum < -NUDGE_MAX && x < 0)) {
		nudge_i = i > DRIFT_MAX ? DRIFT_MAX : i < -DRIFT_MAX ? -DRIFT_MAX : i;
	}
	nudge = nudge_i + KP * x;
	nudge = nudge > NUDGE_MAX ? NUDGE_MAX : nudge < -NUDGE_MAX ? -NUDGE_MAX : nudge;
	step = (uint64_t)((double)((uint64_t)down << 32) * (1.0 + nudge));
	window_reset();
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
	hist_pos = 0;
	phase = 0;
	pending = 0;
	flush = 0;
	idle = 0;
	if (pace) {
		prefill();
	}
}

static int play(int fd)
{
	int16_t buf[CHUNK][2];
	size_t have = 0, i, skip, most;
	double last_read = 0, t, rate_t0 = now(), writer = 0, hold = 0;
	unsigned int lv, skip_left = 0, ahead = 0, skipped = 0, peak = 0, a;
	int kept = 0;			/* sound met during a hold: keep the backlog */
	uint64_t got = 0, got0 = 0;	/* input frames read, skipped or dropped */
	int q;
	ssize_t n;

	for (;;) {
		if (flush) {
			drop(fd);
			have = 0;
			skip_left = 0;
			got0 = got;
			rate_t0 = now();
			hold = rate_t0 + HOLD_S;
			kept = 0;
		}

		n = read(fd, (char *)buf + have, sizeof(buf) - have);
		if (n < 0 && errno == EINTR) {
			continue;
		}

		if (n <= 0) {
			return n < 0 ? -1 : 0;
		}

		have += n;
		got += n / sizeof(buf[0]);
		pending = have / sizeof(buf[0]);
		skip = 0;
		if (pace) {
			t = now();
			/* the DSP takes from the ring again: start afresh */
			if (idle && ring->read != idle_read) {
				flush = 1;
				continue;
			}

			/* the writer kept up and still nothing was left: a gap was heard */
			if (!idle && ring_fill() == 0) {
				if (t - last_read < 0.5) {
					syslog(LOG_INFO, "ran dry, nudge %+.0f ppm", nudge * 1e6);
				}
				prefill();
			}
			/* a start: the writer may burst, as after a fast writer */
			if (t - last_read > 0.5) {
				hold = t + HOLD_S;
				kept = 0;
			}
			last_read = t;

			/*
			 * The writer's rate over the last second, what it put
			 * in the pipe included. Faster than real time, or
			 * waiting on a full pipe, it outruns the DSP (a file
			 * through the pipe): wait on the ring, like for a file.
			 */
			q = 0;
			ioctl(fd, FIONREAD, &q);
			if (t - rate_t0 >= 1) {
				writer = (got + q / 4 - got0) / (t - rate_t0);
				got0 = got + q / 4;
				rate_t0 = t;
			}
			if (!idle && fast != (writer > FAST_RATE * rate_hz ||
					      q >= PIPE_FULL - (fast ? 8 << 10 : 0))) {
				fast = !fast;
				syslog(LOG_INFO, fast ? "writer ahead of the DSP, waiting on the ring" :
					"writer on real time again");
				hold = t + HOLD_S;
				kept = 0;
				level_top = LEVEL_TARGET;
				window_reset();
				nudge = nudge_i;
				step = (uint64_t)((double)((uint64_t)down << 32) * (1.0 + nudge));
			}

			/*
			 * Far too much queued: the writer jumped ahead or left a
			 * backlog, skip it once the ring is fed. During a hold
			 * only silence goes: the end of a file or the start of a
			 * stream plays, what is left over goes after the hold.
			 */
			if (t >= hold) {
				kept = 0;
			}
			lv = level();
			if (lv <= LEVEL_TARGET + DSPAUDIO_RING_LEN / 2) {
				kept = 0;
			} else if (!idle && !fast && !skip_left && !kept &&
				   ring_fill() >= DSPAUDIO_RING_LEN / 2) {
				ahead = lv - LEVEL_TARGET;
				skip_left = (uint64_t)ahead * rate_hz / DSP_HZ;
				skipped = peak = 0;
			}

			if (skip_left) {
				most = skip_left < pending ? skip_left : pending;
				for (; skip < most; skip++) {
					a = abs(buf[skip][0]) > abs(buf[skip][1]) ? abs(buf[skip][0]) :
										    abs(buf[skip][1]);
					if (a > SILENT && t < hold) {
						break;
					}
					peak = a > peak ? a : peak;
				}
				kept = skip < most;
				/* a zero crossing is not silence */
				if (kept && skip < 64) {
					skip = 0;
				}
				skip_left = kept ? 0 : skip_left - skip;
				pending -= skip;
				skipped += skip;
				if (!skip_left && skipped) {
					syslog(LOG_INFO, "%u frames ahead, skipped %u, peak %u", ahead,
					       skipped, peak);
				}
				if (!skip_left) {
					level_top = LEVEL_TARGET;
					window_reset();
				}
			}
			tick();
		}

		for (i = skip; i < have / sizeof(buf[0]) && !idle; i++) {
			resample(buf[i]);
			pending--;
		}
		/* nobody listens: drop the input, the pipe must not back up */
		i = have / sizeof(buf[0]);
		pending = 0;

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

	/* ahead of everything else in Linux: 85 ms in the ring is not much */
	sched_setscheduler(0, SCHED_FIFO, &(struct sched_param){ .sched_priority = 10 });

	mem = open("/dev/mem", O_RDWR | O_SYNC);
	p = mem < 0 ? MAP_FAILED : mmap(NULL, sizeof(*ring), PROT_READ | PROT_WRITE, MAP_SHARED,
					 mem, DSPAUDIO_RING);
	if (p == MAP_FAILED) {
		perror("/dev/mem");
		return 1;
	}

	ring = p;
	rate_hz = rate;
	in_fd = -1;
	/* whatever is left in the ring is old */
	ring->write = ring->read;

	if (!strcmp(path, "-")) {
		in_fd = STDIN_FILENO;
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

		in_fd = fd;
		pace = S_ISFIFO(st.st_mode);
		idle = 0;
		fast = 0;
		if (pace) {
			prefill();
		}

		play(fd);
		close(fd);
	} while (S_ISFIFO(st.st_mode));

	return 0;
}
