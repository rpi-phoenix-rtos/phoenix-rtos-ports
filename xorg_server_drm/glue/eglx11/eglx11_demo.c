/*
 * Phoenix-RTOS
 *
 * eglx11-demo - a GLES2 client in an X window through Mesa's EGL X11 platform
 * (DRI3 buffers + Present), the first GL-in-a-window client of Xorg-drm
 * (docs/gpu-new-lane/M4-xorg-modesetting.md, "M4 part 2").
 *
 * Draws a rotating, colour-cycling hexagon fan over a moving background, swaps
 * with eglSwapBuffers (= loader_dri3: a DRI3 pixmap per back buffer, PresentPixmap,
 * idle fences in shmsrv memory), and prints the frame rate. The first frame's
 * centre pixel is read back from the back buffer before the first swap.
 *
 * Usage: eglx11-demo [-c frames] [-s seconds] [-i swap_interval] [-g WxH+X+Y] [-t title] [-f]
 *   -g     window geometry (default 640x480+640+300: the size of the old lane's 14.2 fps
 *          GL-in-X measurement, research doc §2); set as USPosition|USSize WM hints, so a
 *          window manager honours it
 *   -t     window title (default "eglx11-demo")
 *   -c/-s  stop after that many frames / seconds (default: run until SIGTERM/SIGINT)
 *   -i     eglSwapInterval (default 1 = vsync through Present; 0 = as fast as possible)
 *   -f     start even when shmsrv (/shm) is not running (xshmfence then falls back to a
 *          /tmp file, which is not coherent across processes on Phoenix-RTOS)
 * The same knobs come from the environment (psh cannot quote a command line with
 * spaces into bash's CLIENT=): XDEMO_FRAMES, XDEMO_SECS, XDEMO_INTERVAL, XDEMO_GEOM,
 * XDEMO_TITLE.
 * Every line starts with "XDEMO " (grading).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#ifdef __phoenix__
#include <sys/msg.h>
#endif

#define NSEG 6

static volatile sig_atomic_t stop_flag;

/* First SIGTERM/SIGINT: finish the frame, print the summary, exit. If a swap is
 * stuck (nothing more is presented), SIGALRM's default action ends the process
 * 5 s later, so a caller's `kill; wait` never hangs. */
static void on_signal(int sig)
{
	(void)sig;
	if (stop_flag)
		_exit(4);
	stop_flag = 1;
	alarm(5);
}

static uint64_t now_us(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static const char *vs_src =
	"attribute vec2 pos;\n"
	"attribute vec3 col;\n"
	"uniform float angle;\n"
	"uniform float aspect;\n"
	"varying vec3 v_col;\n"
	"void main() {\n"
	"  float c = cos(angle), s = sin(angle);\n"
	"  vec2 p = vec2(c * pos.x - s * pos.y, s * pos.x + c * pos.y);\n"
	"  gl_Position = vec4(p.x / aspect, p.y, 0.0, 1.0);\n"
	"  v_col = col;\n"
	"}\n";

static const char *fs_src =
	"precision mediump float;\n"
	"varying vec3 v_col;\n"
	"uniform float phase;\n"
	"void main() {\n"
	"  gl_FragColor = vec4(v_col * (0.6 + 0.4 * sin(phase)), 1.0);\n"
	"}\n";

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	char log[512];

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		printf("XDEMO shader type=0x%x compile=FAIL log=%s\n", type, log);
		return 0;
	}
	return s;
}

static int env_int(const char *name, int dflt)
{
	const char *v = getenv(name);

	return (v != NULL && *v != '\0') ? atoi(v) : dflt;
}

int main(int argc, char **argv)
{
	int frames_max = env_int("XDEMO_FRAMES", 0);
	int secs_max = env_int("XDEMO_SECS", 0);
	int interval = env_int("XDEMO_INTERVAL", 1);
	const char *geom = getenv("XDEMO_GEOM") != NULL ? getenv("XDEMO_GEOM") : "640x480+640+300";
	const char *title = getenv("XDEMO_TITLE") != NULL ? getenv("XDEMO_TITLE") : "eglx11-demo";
	int force = 0, opt;
	unsigned int w = 640, h = 480;
	int x = 640, y = 300;
	Display *xd;
	Window win;
	XSetWindowAttributes swa;
	XVisualInfo vtmpl, *vi;
	int nvi = 0;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLint major = 0, minor = 0, ncfg = 0, vid = 0;
	EGLContext ctx;
	EGLSurface surf;
	GLuint prog, vs, fs, vbo;
	GLint loc_angle, loc_aspect, loc_phase, loc_pos, loc_col;
	GLfloat verts[(NSEG + 2) * 5];
	uint64_t t_start, t_last, swap_us = 0, swap_max = 0, frame_max = 0, t_prev;
	unsigned long frames = 0, frames_last = 0;
	int i;

	while ((opt = getopt(argc, argv, "c:s:i:g:t:f")) != -1) {
		switch (opt) {
			case 'c': frames_max = atoi(optarg); break;
			case 's': secs_max = atoi(optarg); break;
			case 'i': interval = atoi(optarg); break;
			case 'g': geom = optarg; break;
			case 't': title = optarg; break;
			case 'f': force = 1; break;
			default:
				fprintf(stderr, "usage: %s [-c frames] [-s secs] [-i interval] [-g WxH+X+Y] [-t title] [-f]\n",
					argv[0]);
				return 2;
		}
	}
	(void)XParseGeometry(geom, &x, &y, &w, &h);
	printf("XDEMO start pid=%d geom=%ux%u+%d+%d interval=%d frames=%d secs=%d\n", (int)getpid(), w, h, x, y,
		interval, frames_max, secs_max);

#ifdef __phoenix__
	{
		/* Refuse to run with the unsafe /tmp fence fallback unless asked (-f). */
		oid_t oid;
		int up = lookup("/shm", NULL, &oid) >= 0;

		printf("XDEMO shmsrv=%s\n", up ? "up" : "missing");
		if (!up && !force) {
			printf("XDEMO done rc=3 reason=no-shmsrv (start /bin/shmsrv first, or -f)\n");
			return 3;
		}
	}
#else
	(void)force;
#endif
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	xd = XOpenDisplay(NULL);
	if (xd == NULL) {
		printf("XDEMO done rc=1 reason=XOpenDisplay DISPLAY=%s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
		return 1;
	}
	printf("XDEMO x11 display=%s screen=%d depth=%d vendor=\"%s\" release=%d\n", DisplayString(xd),
		DefaultScreen(xd), DefaultDepth(xd, DefaultScreen(xd)), ServerVendor(xd), VendorRelease(xd));

	/* Mesa's EGL log names the platform path it took (DRI3 vs a fallback) -- this
	 * process only (the X server's Mesa stays quiet); XDEMO_EGL_DEBUG=0 silences it. */
	if (env_int("XDEMO_EGL_DEBUG", 1) != 0 && getenv("EGL_LOG_LEVEL") == NULL)
		setenv("EGL_LOG_LEVEL", "debug", 1);
	dpy = eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, xd, NULL);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
		printf("XDEMO done rc=1 reason=eglInitialize err=0x%x\n", eglGetError());
		return 1;
	}
	{
		const char *drv = NULL;
		PFNEGLGETDISPLAYDRIVERNAMEPROC get_drv =
			(PFNEGLGETDISPLAYDRIVERNAMEPROC)eglGetProcAddress("eglGetDisplayDriverName");

		if (get_drv != NULL)
			drv = get_drv(dpy);
		printf("XDEMO egl version=%d.%d vendor=\"%s\" driver=%s apis=\"%s\"\n", major, minor,
			eglQueryString(dpy, EGL_VENDOR), drv ? drv : "?", eglQueryString(dpy, EGL_CLIENT_APIS));
	}

	{
		static const EGLint attrs[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE
		};

		if (!eglChooseConfig(dpy, attrs, &cfg, 1, &ncfg) || ncfg < 1) {
			printf("XDEMO done rc=1 reason=eglChooseConfig err=0x%x n=%d\n", eglGetError(), ncfg);
			return 1;
		}
	}
	eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid);
	vtmpl.visualid = (VisualID)vid;
	vi = XGetVisualInfo(xd, VisualIDMask, &vtmpl, &nvi);
	if (vi == NULL) {
		printf("XDEMO done rc=1 reason=no-visual id=0x%x\n", vid);
		return 1;
	}
	memset(&swa, 0, sizeof(swa));
	swa.colormap = XCreateColormap(xd, RootWindow(xd, vi->screen), vi->visual, AllocNone);
	swa.background_pixel = 0;
	swa.border_pixel = 0;
	swa.event_mask = StructureNotifyMask | ExposureMask;
	win = XCreateWindow(xd, RootWindow(xd, vi->screen), x, y, w, h, 0, vi->depth, InputOutput, vi->visual,
		CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &swa);
	XStoreName(xd, win, title);
	{
		/* The geometry is a user request (USPosition|USSize): a window manager places the
		 * window there instead of auto-placing it (Window Maker ignores the bare
		 * XCreateWindow position and PPosition). No effect without a window manager. */
		XSizeHints *hints = XAllocSizeHints();

		if (hints != NULL) {
			hints->flags = USPosition | USSize | PPosition | PSize;
			hints->x = x;
			hints->y = y;
			hints->width = (int)w;
			hints->height = (int)h;
			XSetWMNormalHints(xd, win, hints);
			XFree(hints);
		}
	}
	XMapWindow(xd, win);
	XFlush(xd);
	printf("XDEMO window id=0x%lx visual=0x%x depth=%d title=\"%s\" hints=USPosition|USSize\n", (unsigned long)win, vid,
		vi->depth, title);

	{
		static const EGLint cattrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

		eglBindAPI(EGL_OPENGL_ES_API);
		ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattrs);
		surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)win, NULL);
		if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
			printf("XDEMO done rc=1 reason=context/surface err=0x%x ctx=%p surf=%p\n", eglGetError(), (void *)ctx,
				(void *)surf);
			return 1;
		}
	}
	if (!eglSwapInterval(dpy, interval))
		printf("XDEMO swapinterval=%d rc=FAIL err=0x%x\n", interval, eglGetError());
	printf("XDEMO gl renderer=\"%s\" version=\"%s\"\n", (const char *)glGetString(GL_RENDERER),
		(const char *)glGetString(GL_VERSION));

	vs = compile(GL_VERTEX_SHADER, vs_src);
	fs = compile(GL_FRAGMENT_SHADER, fs_src);
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glUseProgram(prog);
	loc_angle = glGetUniformLocation(prog, "angle");
	loc_aspect = glGetUniformLocation(prog, "aspect");
	loc_phase = glGetUniformLocation(prog, "phase");
	loc_pos = glGetAttribLocation(prog, "pos");
	loc_col = glGetAttribLocation(prog, "col");

	/* centre (white) + NSEG+1 rim vertices in rainbow colours */
	verts[0] = 0.0f; verts[1] = 0.0f; verts[2] = 1.0f; verts[3] = 1.0f; verts[4] = 1.0f;
	for (i = 0; i <= NSEG; i++) {
		float a = (float)i * 2.0f * (float)M_PI / NSEG;
		GLfloat *v = &verts[(i + 1) * 5];

		v[0] = 0.8f * cosf(a);
		v[1] = 0.8f * sinf(a);
		v[2] = 0.5f + 0.5f * cosf(a);
		v[3] = 0.5f + 0.5f * cosf(a - 2.094f);
		v[4] = 0.5f + 0.5f * cosf(a + 2.094f);
	}
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glVertexAttribPointer((GLuint)loc_pos, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (void *)0);
	glVertexAttribPointer((GLuint)loc_col, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (void *)(2 * sizeof(GLfloat)));
	glEnableVertexAttribArray((GLuint)loc_pos);
	glEnableVertexAttribArray((GLuint)loc_col);
	glViewport(0, 0, (GLsizei)w, (GLsizei)h);
	glUniform1f(loc_aspect, (float)w / (float)h);

	t_start = t_last = t_prev = now_us();
	while (!stop_flag) {
		float t = (float)(now_us() - t_start) / 1e6f;
		uint64_t t0, t1;

		while (XPending(xd) > 0) {
			XEvent ev;

			XNextEvent(xd, &ev);
			if (ev.type == ConfigureNotify && ((unsigned)ev.xconfigure.width != w || (unsigned)ev.xconfigure.height != h)) {
				w = (unsigned)ev.xconfigure.width;
				h = (unsigned)ev.xconfigure.height;
				glViewport(0, 0, (GLsizei)w, (GLsizei)h);
				glUniform1f(loc_aspect, (float)w / (float)h);
			}
		}
		glClearColor(0.1f + 0.1f * sinf(t * 0.7f), 0.15f, 0.25f + 0.15f * sinf(t * 0.3f), 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glUniform1f(loc_angle, t * 1.5f);
		glUniform1f(loc_phase, t * 2.0f);
		glDrawArrays(GL_TRIANGLE_FAN, 0, NSEG + 2);
		if (frames == 0) {
			GLubyte px[4] = { 0, 0, 0, 0 };

			glReadPixels((GLint)(w / 2), (GLint)(h / 2), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
			printf("XDEMO first_frame centre_rgba=%u,%u,%u,%u glerr=0x%x (expect bright, not the background)\n",
				px[0], px[1], px[2], px[3], glGetError());
		}
		t0 = now_us();
		if (!eglSwapBuffers(dpy, surf)) {
			printf("XDEMO swap FAIL frame=%lu err=0x%x\n", frames, eglGetError());
			break;
		}
		t1 = now_us();
		swap_us += t1 - t0;
		if (t1 - t0 > swap_max)
			swap_max = t1 - t0;
		if (t1 - t_prev > frame_max)
			frame_max = t1 - t_prev;
		t_prev = t1;
		frames++;
		if (frames == 1)
			printf("XDEMO first_swap ok t_ms=%llu\n", (unsigned long long)((t1 - t_start) / 1000u));
		if (t1 - t_last >= 2000000u) {
			unsigned long n = frames - frames_last;

			printf("XDEMO fps=%.2f frames=%lu t=%.1fs swap_avg_ms=%.2f swap_max_ms=%.2f frame_max_ms=%.2f\n",
				(double)n * 1e6 / (double)(t1 - t_last), frames, (double)(t1 - t_start) / 1e6,
				(double)swap_us / 1000.0 / (double)n, (double)swap_max / 1000.0, (double)frame_max / 1000.0);
			fflush(stdout);
			t_last = t1;
			frames_last = frames;
			swap_us = 0;
			swap_max = 0;
			frame_max = 0;
		}
		if ((frames_max > 0 && frames >= (unsigned long)frames_max) ||
				(secs_max > 0 && t1 - t_start >= (uint64_t)secs_max * 1000000u))
			break;
	}
	{
		uint64_t dt = now_us() - t_start;

		printf("XDEMO done rc=0 frames=%lu secs=%.1f fps=%.2f stop=%s\n", frames, (double)dt / 1e6,
			dt ? (double)frames * 1e6 / (double)dt : 0.0, stop_flag ? "signal" : "limit");
	}
	fflush(stdout);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	XDestroyWindow(xd, win);
	XFree(vi);
	XCloseDisplay(xd);
	return 0;
}
