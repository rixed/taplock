#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <X11/Xlib.h>

static void syntax(void)
{
	printf(
		"taplock [-r] file\n"
		"\n"
		" -r : record new rythm into file\n"
		" otherwise : lock screen until tapped rythm match previously recorded rythm\n");
}

/*
 * Data Definitions
 */

#define sizeof_array(x) (sizeof(x)/sizeof(*(x)))

/*
 * Process command Line
 */

// TODO: add optional images:
//       - one when waiting first tap,
//       - two (alterning) when tapping,
//       - one when wrong.

static bool recording = false;	// depending on command line
static char const *filename;	// filename to read from/write to the rythm

static int read_args(int nb_args, char const **args)
{
	if (nb_args == 2) {
		filename = args[1];
		return 0;
	}

	if (nb_args == 3 && args[1][0] == '-' && args[1][1] == 'r') {
		recording = true;
		filename = args[2];
		return 0;
	}

	syntax();
	return -1;
}

/*
 * Init Window
 */

static Display *dis;
static int screen;
static Window win;
static GC gc;
static unsigned win_width=300, win_height=300;

extern void stay_on_top(Display *, Window);

// Open a full screen window, receiving mouse event. Return file descriptor.
static int init_window(void)
{
	dis = XOpenDisplay(NULL);
	screen = DefaultScreen(dis);

	XSetWindowAttributes setwinattr = { .override_redirect = 1 };
	win = XCreateWindow(
		dis, XRootWindow(dis, screen),
		0, 0, win_width, win_height,
		0,
		DefaultDepth(dis, screen),
		CopyFromParent,
		DefaultVisual(dis, screen),
		CWOverrideRedirect,
		&setwinattr);
	XSelectInput(dis, win, ExposureMask|ButtonPressMask);
	XStoreName(dis, win, "TapLock");
	gc = XCreateGC(dis, win, 0,0);
	stay_on_top(dis, win);

	XClearWindow(dis, win);
	XMapRaised(dis, win);
	XFlush(dis);

	return ConnectionNumber(dis);
}

static void close_window(void)
{
	XUngrabPointer(dis, CurrentTime);
	XFreeGC(dis, gc);
	XDestroyWindow(dis, win);
	XCloseDisplay(dis);	
}

/*
 * Read Rythm
 */

static unsigned nb_taps;	// size of the rythm so far. Never greater then sizeof_array(rythm)
static struct timeval rythm[32];

static suseconds_t timeval_diff(struct timeval from, struct timeval to)
{
	int_least64_t diff = (to.tv_sec - from.tv_sec) * 1000000LL;
	diff += to.tv_usec - from.tv_usec;
	return diff;
}

static void rythm_reset(void)
{
	nb_taps = 0;
}

static void redraw(void)
{
	// TODO
}

static void add_tap(void)
{
	if (nb_taps >= sizeof_array(rythm)) return;

	if (0 == gettimeofday(rythm+nb_taps, NULL)) {
		nb_taps++;
	} else {
		perror("gettimeofday");
	}
}

static int rythm_read(int fd)
{
	rythm_reset();

	// Event loop
	while (1) {
		while (XPending(dis)) {
			XEvent event;
			XNextEvent(dis, &event);

			if (event.type == Expose && event.xexpose.count == 0) {
				redraw();
			} else if (event.type == ButtonPress) {
				add_tap();
			}
		}

		// Wait for X11 event or timeout (1s)
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		int err = select(fd+1, &fds, NULL, NULL, &(struct timeval){ .tv_usec=0, .tv_sec=1 });

		if (err < 0) {
			// FIXME: check errno
			fprintf(stderr, "Cannot read X events : %s\n", strerror(errno));
			return -1;
		} else if (err == 0) {	// timeout
			if (nb_taps > 0) break;	// finished
			// else just wait for the user to tap something
		}
	}

	return 0;
}

/*
 * Save/Load Rythm File
 */

static suseconds_t saved_delays[sizeof_array(rythm)-1];	// we save the diff between successive taps
static unsigned nb_saved_diffs;

static int rythm_save(void)
{
	int fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "Cannot open '%s' : %s\n", filename, strerror(errno));
		return -1;
	}

	ssize_t err = 0;
	assert(nb_taps >= 2);
	for (unsigned t = 1; t < nb_taps; t++) {
		suseconds_t delay = timeval_diff(rythm[t-1], rythm[t]);
		err = write(fd, &delay, sizeof(delay));
		if (err < 0) {
			fprintf(stderr, "Cannot write to '%s' : %s\n", filename, strerror(errno));
			break;
		} else if (err != sizeof(delay)) {
			fprintf(stderr, "Shortwrite ??\n");	// FIXME?
			break;
		}
	}

	(void)close(fd);
	return err;
}

static int rythm_load(void)
{
	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open '%s' : %s\n", filename, strerror(errno));
		return -1;
	}

	ssize_t err = read(fd, &saved_delays, sizeof(saved_delays));
	(void)close(fd);

	if (err < 0) {
		fprintf(stderr, "Cannot read from '%s' : %s\n", filename, strerror(errno));
		return -1;
	}

	if (err % sizeof(saved_delays[0])) {
		fprintf(stderr, "Bad file length\n");
		return -1;
	}

	nb_saved_diffs = err / sizeof(saved_delays[0]);

	return 0;
}

/*
 * Matching Tapped Rythm Against Saved One
 */

static bool rythm_match(void)
{
	if (nb_taps != nb_saved_diffs+1) return false;

	if (nb_taps < 1) return false;
	for (unsigned t = 1; t < nb_taps; t++) {
		suseconds_t delay = timeval_diff(rythm[t-1], rythm[t]);
		suseconds_t dist = delay > saved_delays[t-1] ?
			delay - saved_delays[t-1] : saved_delays[t-1] - delay;
		if (dist > 200000LL) return false;
	}
	return true;
}

/*
 * Main
 */

static int rythm_record(int fd)
{
	int err = rythm_read(fd);
	if (err) return err;

	if (nb_taps < 3) {
		fprintf(stderr, "The rythm must be at least 3 taps long.\n");
		return -1;
	}

	err = rythm_save();
	return err;
}

static int try_unlock(int fd)
{
	int err = rythm_load();
	if (err) return err;

	do {
		int err = rythm_read(fd);
		if (err) return err;
		
		if (rythm_match()) return 0;
		printf("Bad rythm.\n");
		// Else display something
	} while (1);
}

int main(int nb_args, char const **args)
{
	if (-1 == read_args(nb_args, args)) return EXIT_FAILURE;

	int xfd;	// The file descriptor for X11 events
	if (-1 == (xfd = init_window())) return EXIT_FAILURE;

	int err = 0;
	if (recording) {
		err = rythm_record(xfd);
	} else {
		err = try_unlock(xfd);
	}

	close_window();

	return err == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

