/* SPDX-License-Identifier: Apache-2.0
 * C adaptation of the typometer-c measurement backend for te-stsuite.
 * Recognition derives from Typometer, Copyright 2017 Pavel Fatin.
 * See NOTICE.typometer and LICENSES/Apache-2.0.txt.
 */
#define _GNU_SOURCE
#include "te-stsuite.h"

#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Calibration coordinates are local to the visible terminal window. */
typedef struct { int x, y, width, height; } Rectangle;
typedef struct {
    int x, y, length;
    double step;
    unsigned long background;
    bool block_cursor;
} Metrics;

typedef struct {
    Display *display;
    Window root, target;
    int left, top, width, height;
    KeyCode dot, backspace, shift;
    bool shifted;
    int tty;
    atomic_bool stop_echo, echo_failed;
    uint64_t deadline, next_focus_check;
    char *error;
    size_t error_size;
} Typometer;

/* Xlib reports protocol errors through a process-wide callback. */
static atomic_bool x11_failed;
static atomic_int interrupted;

static int x11_error(Display *display, XErrorEvent *event) {
    (void)display;
    (void)event;
    atomic_store(&x11_failed, true);
    return 0;
}

static void interrupt_run(int signal_number) {
    interrupted = signal_number;
}

static bool fail(Typometer *run, const char *message) {
    snprintf(run->error, run->error_size, "%s", message);
    return false;
}

/* Sleeps remain short so cancellation and focus changes are noticed promptly. */
static void sleep_ns(uint64_t nanoseconds) {
    struct timespec delay = {.tv_sec = (time_t)(nanoseconds / 1000000000ULL),
                             .tv_nsec = (long)(nanoseconds % 1000000000ULL)};
    nanosleep(&delay, NULL);
}

static bool write_bytes(int fd, const char *bytes, size_t length) {
    while (length) {
        ssize_t count = write(fd, bytes, length);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd output = {.fd = fd, .events = POLLOUT};
            if (poll(&output, 1, 20) >= 0 && !interrupted) continue;
        }
        if (count <= 0) return false;
        bytes += count;
        length -= (size_t)count;
    }
    return true;
}

/* A tiny raw-mode editor keeps input dispatch and PTY echo in the timed path. */
static void *echo_input(void *user) {
    Typometer *run = user;
    unsigned cells = 0;
    while (!atomic_load(&run->stop_echo) && !interrupted) {
        struct pollfd input = {.fd = run->tty, .events = POLLIN};
        int ready = poll(&input, 1, 10);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (input.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (!ready) continue;
        char bytes[128];
        ssize_t count = read(run->tty, bytes, sizeof(bytes));
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) break;
        for (ssize_t i = 0; i < count; ++i) {
            if (bytes[i] == '.') {
                if (!write_bytes(run->tty, ".", 1)) goto failed;
                ++cells;
            } else if ((bytes[i] == '\b' || bytes[i] == 127) && cells > 0) {
                if (!write_bytes(run->tty, "\b \b", 3)) goto failed;
                --cells;
            } else {
                /* Unexpected input invalidates the controlled typing workload. */
                goto failed;
            }
        }
    }
    if (atomic_load(&run->stop_echo) || interrupted) return NULL;
failed:
    atomic_store(&run->echo_failed, true);
    return NULL;
}

/* Only windows owned by this worker's terminal ancestry may receive input. */
static bool ancestor_pid(unsigned long candidate) {
    struct stat executable;
    if (stat("/proc/self/exe", &executable) != 0) return false;
    pid_t pid = getppid();
    for (unsigned depth = 0; pid > 1 && depth < 64; ++depth) {
        char path[64], line[4096];
        struct stat parent_executable;
        snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
        if (stat(path, &parent_executable) != 0) return false;
        /* Stop at the controller, before reaching the user's launching terminal. */
        if (parent_executable.st_dev == executable.st_dev &&
            parent_executable.st_ino == executable.st_ino) return false;
        if ((unsigned long)pid == candidate) return true;
        snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
        FILE *file = fopen(path, "r");
        if (!file) return false;
        bool read_ok = fgets(line, sizeof(line), file) != NULL;
        fclose(file);
        char *tail = read_ok ? strrchr(line, ')') : NULL;
        long parent = 0;
        char state;
        if (!tail || sscanf(tail + 1, " %c %ld", &state, &parent) != 2 ||
            parent <= 0 || parent == pid) return false;
        pid = (pid_t)parent;
    }
    return false;
}

/* Window properties identify the terminal process, including embedded clients. */
static bool owns_window(Display *display, Window window) {
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;
    Atom property = XInternAtom(display, "_NET_WM_PID", True);
    if (property == None) return false;
    int status = XGetWindowProperty(display, window, property, 0, 1, False,
                                    XA_CARDINAL, &type, &format, &count,
                                    &remaining, &data);
    bool owned = status == Success && type == XA_CARDINAL && format == 32 &&
                 count == 1 && remaining == 0 && data &&
                 ancestor_pid(*(unsigned long *)data);
    if (data) XFree(data);
    return owned;
}

static Window parent_window(Display *display, Window window) {
    Window root, parent, *children = NULL;
    unsigned count;
    bool ok = XQueryTree(display, window, &root, &parent, &children, &count);
    if (children) XFree(children);
    return ok ? parent : None;
}

/* Focus may belong to a child widget beneath the advertised client window. */
static bool within_target(Typometer *run, Display *display, Window window) {
    for (unsigned depth = 0; window > PointerRoot && depth < 64; ++depth) {
        if (window == run->target) return true;
        if (window == run->root) break;
        window = parent_window(display, window);
    }
    return false;
}

/* Search mapped client windows when the new terminal did not acquire focus. */
static Window find_target(Display *display, Window window, unsigned depth) {
    XWindowAttributes attributes;
    if (depth > 16 || !XGetWindowAttributes(display, window, &attributes) ||
        attributes.map_state != IsViewable) return None;
    if (owns_window(display, window)) return window;
    Window root, parent, *children = NULL, found = None;
    unsigned count = 0;
    if (XQueryTree(display, window, &root, &parent, &children, &count)) {
        for (unsigned i = 0; i < count && found == None; ++i)
            found = find_target(display, children[i], depth + 1);
    }
    if (children) XFree(children);
    return found;
}

static bool focused(Typometer *run, Display *display) {
    Window focus;
    int revert;
    XGetInputFocus(display, &focus, &revert);
    return within_target(run, display, focus);
}

/* Poll focus at most every ten milliseconds, forcing a check before injection. */
static bool check_run(Typometer *run, bool force_focus) {
    if (interrupted) return fail(run, "latency measurement interrupted");
    if (atomic_load(&x11_failed)) return fail(run, "X11 request failed; keep the terminal visible");
    if (atomic_load(&run->echo_failed)) return fail(run, "typing target failed or received unexpected input");
    uint64_t now = monotonic_now_ns();
    if (force_focus || now >= run->next_focus_check) {
        if (!focused(run, run->display)) return fail(run, "terminal focus changed during latency measurement");
        run->next_focus_check = now + 10000000ULL;
    }
    return true;
}

static bool wait_ms(Typometer *run, unsigned milliseconds) {
    uint64_t until = monotonic_now_ns() + (uint64_t)milliseconds * 1000000ULL;
    if (run->deadline && until > run->deadline) until = run->deadline;
    for (;;) {
        if (!check_run(run, false)) return false;
        uint64_t now = monotonic_now_ns();
        if (now >= until) return true;
        sleep_ns(until - now < 5000000ULL ? until - now : 5000000ULL);
    }
}

/* Open the native TrueColor display used for rendered-pixel measurements. */
static bool open_display(Typometer *run) {
    run->display = XOpenDisplay(NULL);
    if (!run->display) return fail(run, "pixel measurement requires a native X11 display (check DISPLAY)");
    Display *display = run->display;
    int screen = DefaultScreen(display);
    if (DefaultVisual(display, screen)->class != TrueColor)
        return fail(run, "pixel measurement requires a TrueColor X11 visual");
    run->root = RootWindow(display, screen);
    return true;
}

/* Locate this worker's terminal window and record its visible geometry. */
static bool locate_terminal_window(Typometer *run) {
    Display *display = run->display;
    int screen = DefaultScreen(display);
    uint64_t until = monotonic_now_ns() + 3000000000ULL;
    do {
        run->target = find_target(display, run->root, 0);
        if (run->target) break;
        if (interrupted) return fail(run, "pixel measurement interrupted");
        sleep_ns(20000000ULL);
    } while (monotonic_now_ns() < until);
    if (!run->target) return fail(run, "cannot identify this terminal's X11 window using _NET_WM_PID");
    if (!focused(run, display)) {
        XRaiseWindow(display, run->target);
        XSetInputFocus(display, run->target, RevertToParent, CurrentTime);
        XSync(display, False);
    }
    if (!check_run(run, true)) return false;
    XWindowAttributes attributes;
    Window child;
    if (!XGetWindowAttributes(display, run->target, &attributes) ||
        !XTranslateCoordinates(display, run->target, run->root, 0, 0,
                                &run->left, &run->top, &child))
        return fail(run, "cannot determine terminal window geometry");
    run->width = attributes.width;
    run->height = attributes.height;
    if (run->left < 0 || run->top < 0 || run->width < 1 || run->height < 1 ||
        run->left + run->width > DisplayWidth(display, screen) ||
        run->top + run->height > DisplayHeight(display, screen))
        return fail(run, "keep the entire terminal window on screen");
    return true;
}

/* Resolve period through the active XKB group, including Shift-only layouts. */
static bool open_screen(Typometer *run) {
    if (!open_display(run)) return false;
    Display *display = run->display;
    int event, error, major, minor;
    if (!XTestQueryExtension(display, &event, &error, &major, &minor))
        return fail(run, "X11 server does not support XTEST");
    if (!locate_terminal_window(run)) return false;
    XkbStateRec state;
    if (XkbGetState(display, XkbUseCoreKbd, &state) != Success)
        return fail(run, "cannot query XKB keyboard state");
    if (state.mods & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask | Mod5Mask))
        return fail(run, "release keyboard modifiers before measuring latency");
    int low, high;
    XDisplayKeycodes(display, &low, &high);
    for (int level = 0; level < 2 && !run->dot; ++level) {
        for (int code = low; code <= high; ++code) {
            KeySym symbol;
            unsigned consumed;
            unsigned modifiers = state.locked_mods | (level ? ShiftMask : 0);
            if (XkbLookupKeySym(display, (KeyCode)code,
                                XkbBuildCoreState(modifiers, state.group),
                                &consumed, &symbol) && symbol == XK_period) {
                run->dot = (KeyCode)code;
                run->shifted = level != 0;
                break;
            }
        }
    }
    run->backspace = XKeysymToKeycode(display, XK_BackSpace);
    run->shift = XKeysymToKeycode(display, XK_Shift_L);
    if (!run->dot || !run->backspace || (run->shifted && !run->shift))
        return fail(run, "cannot map period or Backspace in the active keyboard layout");
    return true;
}

/* Always release both the character key and any synthetic Shift modifier. */
static bool inject(Display *display, KeyCode key, KeyCode shift) {
    bool ok = true;
    if (shift) ok = XTestFakeKeyEvent(display, shift, True, CurrentTime);
    bool pressed = XTestFakeKeyEvent(display, key, True, CurrentTime);
    bool released = XTestFakeKeyEvent(display, key, False, CurrentTime);
    if (shift) ok = XTestFakeKeyEvent(display, shift, False, CurrentTime) && ok;
    XFlush(display);
    return ok && pressed && released;
}

/* A block caret moves into the next cell when the character becomes visible. */
static int sample_x(const Metrics *metrics, unsigned index) {
    return (int)lround(metrics->x + metrics->step *
                       (index + (metrics->block_cursor ? 1.0 : 0.0)));
}

static bool pixel(Typometer *run, int x, int y, unsigned long *value) {
    if (x < 0 || y < 0 || x >= run->width || y >= run->height)
        return fail(run, "detected sampling point is outside the terminal");
    XImage *image = XGetImage(run->display, run->root, run->left + x,
                              run->top + y, 1, 1, AllPlanes, ZPixmap);
    if (!image) return fail(run, "cannot read the terminal screen pixel");
    *value = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    return !atomic_load(&x11_failed);
}

/* Match the source backend with a three-second timeout from injection. */
static bool changed(Typometer *run, int x, int y, unsigned long previous,
                     uint64_t start) {
    do {
        if (!check_run(run, false)) return false;
        unsigned long value;
        if (!pixel(run, x, y, &value)) return false;
        if (value != previous) return true;
        if (monotonic_now_ns() - start > 3000000000ULL)
            return fail(run, "no screen change within three seconds");
    } while (true);
}

/* Preserve the source algorithm's grayscale range and RGB channel weights. */
static unsigned channel(unsigned long value, unsigned long mask) {
    if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; value >>= 1; }
    return (unsigned)((value & mask) * 255 / mask);
}

static int *grayscale(XImage *image) {
    size_t count = (size_t)image->width * image->height;
    if (count > SIZE_MAX / sizeof(int)) return NULL;
    int *gray = malloc(count * sizeof(*gray));
    if (!gray) return NULL;
    for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
            unsigned long value = XGetPixel(image, x, y);
            gray[(size_t)y * image->width + x] =
                (int)(0.1063 * channel(value, image->red_mask) +
                      0.3576 * channel(value, image->green_mask) +
                      0.0361 * channel(value, image->blue_mask));
        }
    }
    return gray;
}

/* Ordering gives both a set difference and row-wise sequence detection. */
static int compare_rectangle(const void *a, const void *b) {
    const Rectangle *left = a, *right = b;
    if (left->y != right->y) return (left->y > right->y) - (left->y < right->y);
    if (left->x != right->x) return (left->x > right->x) - (left->x < right->x);
    if (left->width != right->width) return left->width - right->width;
    return left->height - right->height;
}

/* Local extrema and connected components isolate compact antialiased dots. */
static bool areas(const int *image, int width, int height, bool inverted,
                   Buffer *rectangles) {
    size_t count = (size_t)width * height;
    if (count > SIZE_MAX / sizeof(size_t)) return false;
    unsigned char *binary = malloc(count);
    size_t *queue = malloc(count * sizeof(*queue));
    if (!binary || !queue) { free(binary); free(queue); return false; }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int background = 0;
            if (x >= 2 && y >= 2 && x < width - 2 && y < height - 2) {
                background = inverted ? 127 : 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        int v = image[(size_t)(y + dy) * width + x + dx];
                        if (inverted ? v < background : v > background) background = v;
                    }
                }
            }
            size_t p = (size_t)y * width + x;
            binary[p] = (inverted ? image[p] - background : background - image[p]) > 20;
        }
    }
    bool ok = true;
    for (size_t p = 0; p < count && ok; ++p) {
        if (!binary[p]) continue;
        size_t length = 1;
        queue[0] = p;
        binary[p] = 0;
        int left = width, top = height, right = 0, bottom = 0;
        for (size_t head = 0; head < length; ++head) {
            int x = (int)(queue[head] % width), y = (int)(queue[head] / width);
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    size_t next = (size_t)ny * width + nx;
                    if (binary[next]) { binary[next] = 0; queue[length++] = next; }
                }
            }
        }
        Rectangle r = {left, top, right - left + 1, bottom - top + 1};
        size_t area = (size_t)r.width * r.height;
        size_t fill = length + (length > 1 ? 1 : 0);
        if (area <= 100 && r.width <= 2 * r.height && r.height <= 2 * r.width &&
            (double)fill > 0.7 * area)
            ok = buffer_append(rectangles, &r, sizeof(r));
    }
    free(binary);
    free(queue);
    if (rectangles->length) qsort(rectangles->data, rectangles->length / sizeof(Rectangle),
                                 sizeof(Rectangle), compare_rectangle);
    return ok;
}

static bool consistent(const double *values, unsigned count) {
    double mean = 0;
    for (unsigned i = 0; i < count; ++i) mean += values[i] / count;
    for (unsigned i = 0; i < count; ++i) if (fabs(values[i] - mean) >= 2.01) return false;
    return true;
}

/* Subtract the baseline components and recognize exactly five equally spaced dots. */
static bool sequence(Buffer *previous, Buffer *current, XImage *after, Metrics *metrics) {
    Rectangle *old = (Rectangle *)previous->data, *added = (Rectangle *)current->data;
    size_t count = 0, old_count = previous->length / sizeof(*old);
    for (size_t i = 0; i < current->length / sizeof(*added); ++i) {
        if (!old_count || !bsearch(&added[i], old, old_count, sizeof(*old), compare_rectangle))
            added[count++] = added[i];
    }
    for (size_t begin = 0; begin < count;) {
        size_t end = begin + 1;
        while (end < count && added[end].y == added[begin].y) ++end;
        if (end - begin == 5) {
            double widths[5], heights[5], distances[4];
            for (unsigned i = 0; i < 5; ++i) {
                widths[i] = added[begin + i].width;
                heights[i] = added[begin + i].height;
                if (i) distances[i - 1] = added[begin + i].x - added[begin + i - 1].x;
            }
            Rectangle first = added[begin], last = added[end - 1];
            double x1 = first.x + 0.5 * first.width - 1;
            double x2 = last.x + 0.5 * last.width - 1;
            double step = (x2 - x1) / 4;
            if (lround(step) >= 4 && consistent(widths, 5) &&
                consistent(heights, 5) && consistent(distances, 4)) {
                int x = (int)lround(x1), bg_x = (int)lround(last.x + 2 * step);
                int cursor_x = (int)lround(last.x + step);
                int tail = x + (int)lround(step * 8);
                if (x >= 0 && bg_x < after->width && tail < after->width) {
                    unsigned long bg = XGetPixel(after, bg_x, first.y);
                    bool block = XGetPixel(after, cursor_x, first.y) != bg;
                    int start = tail;
                    while (tail < after->width && XGetPixel(after, tail, first.y) == bg) ++tail;
                    int length = (int)floor((tail - start) / step) - (block ? 1 : 0) + 7;
                    *metrics = (Metrics){x, first.y, length, step, bg, block};
                    return true;
                }
            }
        }
        begin = end;
    }
    return false;
}

static bool detect_metrics(XImage *before, XImage *after, Metrics *metrics) {
    int *a = grayscale(before), *b = grayscale(after);
    if (!a || !b) { free(a); free(b); return false; }
    int luminance = 0;
    for (int i = 0; i < 10; ++i)
        luminance += a[(size_t)(i * (before->height / 10)) * before->width +
                       i * (before->width / 10)];
    bool inverted = luminance / 10 < 63, found = false;
    for (unsigned pass = 0; pass < 2 && !found; ++pass) {
        Buffer previous, current;
        buffer_init(&previous);
        buffer_init(&current);
        bool polarity = pass ? !inverted : inverted;
        bool ok = areas(a, before->width, before->height, polarity, &previous) &&
                  areas(b, after->width, after->height, polarity, &current);
        if (ok) found = sequence(&previous, &current, after, metrics);
        buffer_free(&previous);
        buffer_free(&current);
        if (!ok) break;
    }
    free(a);
    free(b);
    return found;
}

static bool erase_batch(Typometer *run, const Metrics *metrics, unsigned count) {
    if (!check_run(run, true)) return false;
    unsigned long previous;
    if (!pixel(run, sample_x(metrics, 0), metrics->y, &previous)) return false;
    for (unsigned i = 0; i < count; ++i) {
        if (!check_run(run, true)) return false;
        if (!inject(run->display, run->backspace, 0)) return fail(run, "cannot inject Backspace");
    }
    if (!changed(run, sample_x(metrics, 0), metrics->y, previous, monotonic_now_ns())) return false;
    /* Cleanup must settle even when the measurement deadline has been reached. */
    uint64_t deadline = run->deadline;
    run->deadline = 0;
    bool ok = wait_ms(run, 200);
    run->deadline = deadline;
    return ok;
}

static bool calibrate(Typometer *run, Metrics *metrics) {
    const char *setup = "\033c\033[2J\033[H";
    if (!write_bytes(run->tty, setup, strlen(setup)) || !wait_ms(run, 200)) return false;
    XImage *before = XGetImage(run->display, run->root, run->left, run->top,
                               (unsigned)run->width, (unsigned)run->height, AllPlanes, ZPixmap);
    if (!before) return fail(run, "cannot capture the calibration baseline");
    bool ok = true;
    for (unsigned i = 0; i < 5 && ok; ++i)
        ok = check_run(run, true) && inject(run->display, run->dot, run->shifted ? run->shift : 0);
    if (ok) ok = wait_ms(run, 300);
    XImage *after = ok ? XGetImage(run->display, run->root, run->left, run->top,
                                   (unsigned)run->width, (unsigned)run->height, AllPlanes, ZPixmap) : NULL;
    if (ok) ok = after && detect_metrics(before, after, metrics);
    XDestroyImage(before);
    if (after) XDestroyImage(after);
    if (!ok) {
        if (!run->error[0]) fail(run, "cannot recognize five dots; use a visible monospaced terminal with a solid background");
        return false;
    }
    if (metrics->length < 15) return fail(run, "typometer needs at least 15 available character cells");
    return erase_batch(run, metrics, 5);
}

static unsigned delay_after(const RunOptions *options, unsigned index) {
    return options->typometer_pause_every && index % options->typometer_pause_every == 0
               ? options->typometer_pause_ms : options->typometer_delay_ms;
}

/* Separate connections allow the writer to queue input while the reader polls. */
typedef struct {
    Typometer *run;
    Display *display;
    const RunOptions *options;
    atomic_uint_fast64_t *moments;
    unsigned count, offset;
    atomic_bool stop, done, failed;
} AsyncWriter;

static void *type_async(void *user) {
    AsyncWriter *writer = user;
    Typometer *run = writer->run;
    for (unsigned i = 0; i < writer->count; ++i) {
        if (atomic_load(&writer->stop) || interrupted || monotonic_now_ns() >= run->deadline) break;
        if (!focused(run, writer->display)) goto failed;
        uint64_t start = monotonic_now_ns();
        atomic_store_explicit(&writer->moments[i], start, memory_order_release);
        if (!inject(writer->display, run->dot, run->shifted ? run->shift : 0)) goto failed;
        uint64_t until = monotonic_now_ns() +
                         (uint64_t)delay_after(writer->options, writer->offset + i + 1) * 1000000ULL;
        if (until > run->deadline) until = run->deadline;
        while (monotonic_now_ns() < until) {
            if (atomic_load(&writer->stop) || interrupted) goto done;
            if (!focused(run, writer->display)) goto failed;
            sleep_ns(1000000ULL);
        }
    }
    goto done;
failed:
    atomic_store(&writer->failed, true);
done:
    atomic_store_explicit(&writer->done, true, memory_order_release);
    return NULL;
}

static bool async_batch(Typometer *run, const Metrics *metrics, const RunOptions *options,
                         unsigned count, BenchSamples *samples, unsigned *typed) {
    AsyncWriter writer = {.run = run, .options = options, .count = count,
                           .offset = samples->count};
    atomic_init(&writer.stop, false);
    atomic_init(&writer.done, false);
    atomic_init(&writer.failed, false);
    writer.display = XOpenDisplay(NULL);
    writer.moments = calloc(count, sizeof(*writer.moments));
    if (!writer.display || !writer.moments) {
        if (writer.display) XCloseDisplay(writer.display);
        free(writer.moments);
        return fail(run, "cannot allocate asynchronous typing resources");
    }
    for (unsigned i = 0; i < count; ++i) atomic_init(&writer.moments[i], 0);
    pthread_t thread;
    int status = pthread_create(&thread, NULL, type_async, &writer);
    if (status) {
        XCloseDisplay(writer.display);
        free(writer.moments);
        return fail(run, "cannot start asynchronous typing thread");
    }
    bool ok = true;
    *typed = 0;
    for (unsigned i = 0; i < count && ok; ++i) {
        uint64_t start;
        for (;;) {
            bool done = atomic_load_explicit(&writer.done, memory_order_acquire);
            start = atomic_load_explicit(&writer.moments[i], memory_order_acquire);
            if (start || done) break;
            if (!check_run(run, false)) { ok = false; break; }
            sleep_ns(50000ULL);
        }
        if (!ok || !start) break;
        ok = changed(run, sample_x(metrics, i), metrics->y, metrics->background, start);
        if (ok) {
            samples->values[samples->count++] = monotonic_now_ns() - start;
            ++*typed;
        }
    }
    if (!ok) atomic_store(&writer.stop, true);
    pthread_join(thread, NULL);
    if (atomic_load(&writer.failed)) ok = fail(run, "asynchronous typing failed or terminal focus changed");
    XCloseDisplay(writer.display);
    free(writer.moments);
    return ok;
}

/* Own all terminal, thread and X11 state so every normal exit restores the TTY. */
bool bench_typometer_execute(const BenchContext *context, const RunOptions *options,
                              bool warmup, BenchBegin begin, void *user,
                              BenchSamples *samples, char *error, size_t error_size) {
    (void)context;
    Typometer run = {.tty = -1, .error = error, .error_size = error_size};
    atomic_init(&run.stop_echo, false);
    atomic_init(&run.echo_failed, false);
    atomic_store(&x11_failed, false);
    interrupted = 0;
    if (getenv("WAYLAND_DISPLAY") ||
        (getenv("XDG_SESSION_TYPE") && !strcmp(getenv("XDG_SESSION_TYPE"), "wayland")))
        return fail(&run, "typometer supports native X11 sessions only; Wayland and XWayland are unsupported");
    if (!XInitThreads()) return fail(&run, "cannot initialize Xlib threading");
    int (*old_xerror)(Display *, XErrorEvent *) = XSetErrorHandler(x11_error);
    const int signals[] = {SIGINT, SIGTERM, SIGHUP};
    struct sigaction previous[3], action = {.sa_handler = interrupt_run};
    sigemptyset(&action.sa_mask);
    unsigned installed = 0;
    bool ok = true, tty_changed = false, echo_started = false;
    struct termios saved;
    pthread_t echo;
    for (; installed < 3; ++installed) {
        if (sigaction(signals[installed], &action, &previous[installed]) != 0) {
            ok = fail(&run, "cannot install latency cancellation handlers");
            break;
        }
    }
    if (ok) ok = open_screen(&run);
    if (ok) {
        run.tty = open("/dev/tty", O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (run.tty < 0 || tcgetattr(run.tty, &saved) != 0)
            ok = fail(&run, "cannot open the terminal typing target");
    }
    if (ok) {
        struct termios raw = saved;
        cfmakeraw(&raw);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tty_changed = tcsetattr(run.tty, TCSAFLUSH, &raw) == 0;
        if (!tty_changed) ok = fail(&run, "cannot configure terminal raw input");
    }
    if (ok) {
        echo_started = pthread_create(&echo, NULL, echo_input, &run) == 0;
        if (!echo_started) ok = fail(&run, "cannot start terminal typing target");
    }
    Metrics metrics = {0};
    if (ok) ok = calibrate(&run, &metrics);
    unsigned limit = options->typometer_count;
    if (options->max_samples && limit > options->max_samples) limit = options->max_samples;
    if (ok) {
        /* calloc checks size multiplication overflow on narrower platforms. */
        samples->values = calloc(limit, sizeof(*samples->values));
        if (!samples->values) ok = fail(&run, "cannot allocate latency samples");
    }
    if (ok && !warmup && !begin(user)) ok = fail(&run, "cannot start latency resource accounting");
    run.deadline = monotonic_now_ns() + (uint64_t)options->max_seconds * 1000000000ULL;
    while (ok && samples->count < limit && monotonic_now_ns() < run.deadline) {
        unsigned count = limit - samples->count;
        if (count > (unsigned)metrics.length) count = (unsigned)metrics.length;
        for (unsigned i = 0; i < count && ok; ++i) {
            unsigned long value;
            ok = check_run(&run, false) && pixel(&run, sample_x(&metrics, i), metrics.y, &value);
            if (ok && value != metrics.background) ok = fail(&run, "sampling line is not empty");
        }
        unsigned typed = 0;
        if (ok && options->typometer_asynchronous) {
            ok = async_batch(&run, &metrics, options, count, samples, &typed);
        } else {
            for (unsigned i = 0; i < count && ok && monotonic_now_ns() < run.deadline; ++i) {
                ok = check_run(&run, true);
                if (!ok) break;
                uint64_t start = monotonic_now_ns();
                ok = inject(run.display, run.dot, run.shifted ? run.shift : 0) &&
                     changed(&run, sample_x(&metrics, i), metrics.y, metrics.background, start);
                if (ok) {
                    samples->values[samples->count++] = monotonic_now_ns() - start;
                    ++typed;
                    ok = wait_ms(&run, delay_after(options, samples->count));
                }
            }
        }
        if (ok && typed) ok = erase_batch(&run, &metrics, typed);
    }
    if (ok && !samples->count) ok = fail(&run, "no latency samples completed before the time limit");
    atomic_store(&run.stop_echo, true);
    if (echo_started) pthread_join(echo, NULL);
    if (tty_changed) {
        /* Reset only our PTY; never inject cleanup keys after losing focus. */
        write_bytes(run.tty, "\033c", 2);
        if (tcsetattr(run.tty, TCSAFLUSH, &saved) != 0) ok = fail(&run, "cannot restore terminal input settings");
    }
    if (run.tty >= 0) close(run.tty);
    if (run.display) XCloseDisplay(run.display);
    XSetErrorHandler(old_xerror);
    while (installed) { --installed; sigaction(signals[installed], &previous[installed], NULL); }
    if (!ok && !error[0]) fail(&run, "latency measurement failed");
    return ok;
}

/* Accept palette-converted marker colors without requiring RGB equality. */
static bool marker_pixel(XImage *image, int x, int y, bool blue_marker) {
    unsigned long value = XGetPixel(image, x, y);
    unsigned red = channel(value, image->red_mask);
    unsigned green = channel(value, image->green_mask);
    unsigned blue = channel(value, image->blue_mask);
    if (blue_marker) {
        unsigned swap = red;
        red = blue;
        blue = swap;
    }
    unsigned other = green > blue ? green : blue;
    return red >= 64 && red > green + 16 && red > blue + 16 &&
           red * 100 >= other * 115;
}

/* A blue ring distinguishes the red space from window chrome and backgrounds. */
static bool guarded_cell(XImage *image, Rectangle cell) {
    if (cell.x < cell.width || cell.y < cell.height ||
        cell.x + 2 * cell.width > image->width ||
        cell.y + 2 * cell.height > image->height) return false;
    for (int y = cell.y - cell.height; y < cell.y + 2 * cell.height; ++y) {
        for (int x = cell.x - cell.width; x < cell.x + 2 * cell.width; ++x) {
            bool inside = x >= cell.x && x < cell.x + cell.width &&
                          y >= cell.y && y < cell.y + cell.height;
            if (!marker_pixel(image, x, y, !inside)) return false;
        }
    }
    return true;
}

/* Examine red runs only at their top edge, then verify the entire 3x3 marker. */
static bool locate_red_cell(XImage *image, Rectangle *cell) {
    bool found = false;
    for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
            if (!marker_pixel(image, x, y, false)) continue;
            int left = x;
            while (x + 1 < image->width && marker_pixel(image, x + 1, y, false)) ++x;
            if (y > 0 && marker_pixel(image, left, y - 1, false)) continue;
            int bottom = y;
            while (bottom + 1 < image->height &&
                   marker_pixel(image, left, bottom + 1, false)) ++bottom;
            Rectangle candidate = {left, y, x - left + 1, bottom - y + 1};
            if (!guarded_cell(image, candidate)) continue;
            if (found) return false;
            *cell = candidate;
            found = true;
        }
    }
    return found;
}

/* Draw one colored space, capture its rendered bounds, and restore the screen. */
bool bench_cell_dimensions_execute(const BenchContext *context,
                                    const RunOptions *options, bool warmup,
                                    BenchBegin begin, void *user,
                                    BenchSamples *samples, char *error,
                                    size_t error_size) {
    (void)options;
    if (warmup) return true;
    Typometer run = {.tty = -1, .error = error, .error_size = error_size};
    if (context->rows < 3 || context->columns < 3)
        return fail(&run, "cell_dimensions requires at least three rows and columns");
    atomic_init(&run.stop_echo, false);
    atomic_init(&run.echo_failed, false);
    atomic_store(&x11_failed, false);
    interrupted = 0;
    if (getenv("WAYLAND_DISPLAY") ||
        (getenv("XDG_SESSION_TYPE") && !strcmp(getenv("XDG_SESSION_TYPE"), "wayland")))
        return fail(&run, "cell_dimensions requires a native X11 session");
    if (!XInitThreads()) return fail(&run, "cannot initialize Xlib threading");
    int (*old_xerror)(Display *, XErrorEvent *) = XSetErrorHandler(x11_error);
    bool ok = open_display(&run) && locate_terminal_window(&run);
    bool wrote_test = false;
    if (ok && !begin(user))
        ok = fail(&run, "cannot start cell dimension measurement");
    /* Explicit row positioning avoids depending on newline or wrapping modes. */
    static const char test[] = "\033c\033[2J\033[H\033[?25l"
                               "\033[48;2;0;0;255m   "
                               "\033[2;1H \033[48;2;255;0;0m "
                               "\033[48;2;0;0;255m \033[3;1H   \033[0m";
    if (ok) {
        wrote_test = true;
        ok = write_bytes(STDOUT_FILENO, test, sizeof(test) - 1);
        if (!ok) fail(&run, "cannot write the red backgrounded space");
    }
    Rectangle cell = {0};
    if (ok) {
        /* Mapping the window does not mean its first frame has been painted. */
        uint64_t until = monotonic_now_ns() + 3000000000ULL;
        bool found = false;
        do {
            sleep_ns(50000000ULL);
            if (!check_run(&run, false)) { ok = false; break; }
            XImage *image = XGetImage(run.display, run.root, run.left, run.top,
                                      (unsigned)run.width, (unsigned)run.height,
                                      AllPlanes, ZPixmap);
            if (image == NULL) {
                ok = fail(&run, "cannot capture the terminal window pixels");
                break;
            }
            found = locate_red_cell(image, &cell);
            XDestroyImage(image);
        } while (!found && monotonic_now_ns() < until);
        if (ok && !found)
            ok = fail(&run, "no red cell surrounded by blue cells appeared within three seconds");
    }
    if (wrote_test && !write_bytes(STDOUT_FILENO, "\033c", 2))
        ok = fail(&run, "cannot restore the terminal after cell measurement");
    if (ok) {
        samples->has_cell_dimensions = true;
        samples->cell_width_px = (unsigned)cell.width;
        samples->cell_height_px = (unsigned)cell.height;
    }
    if (run.display) XCloseDisplay(run.display);
    XSetErrorHandler(old_xerror);
    if (!ok && !error[0]) fail(&run, "cell dimension measurement failed");
    return ok;
}
