/* Our own minimal test app, written from scratch -- NOT compiled
 * Qualcomm/BREW SDK code. Structurally identical to hello_brew.c's
 * AEEMod_Load -> IModule::CreateInstance -> HandleEvent lifecycle (see
 * that file for the reverse-engineered contract this follows), but
 * HandleEvent here dispatches through the real, verified IGL/IEGL
 * vtable slot order (see TASKS.md/ARCHITECTURE.md Phase 5 for how that
 * order was confirmed against the real AEEGL.h) instead of IDisplay.
 * This is what actually validates GlHle's dispatch against code we
 * didn't write in C++ ourselves -- real compiled ARM code indexing into
 * a real-shaped vtable -- rather than only unit tests driving it
 * directly via HleRuntime::CallArmFunction.
 *
 * Deliberately does NOT reproduce Qualcomm's real EGL_1x.c/GLES_1x.c
 * wrapper glue (that's real Qualcomm source, kept research-only, never
 * committed -- see TASKS.md Phase 5). Real games get that wrapper for
 * free since it's statically linked in from the SDK and internally
 * dispatches through gpIGL/gpIEGL; this app dispatches through the
 * IGL/IEGL vtables directly itself, which is a legitimate simplification
 * for a clean-room ABI-conformance test -- we only need to prove our
 * vtable ABI is correct, not reproduce Qualcomm's convenience layer.
 *
 * No floating point anywhere in this file (fixed-point constants are
 * plain integer literals, compile-time-folded) -- avoids dragging in
 * soft-float library routines the interpreter doesn't need to support,
 * and avoids the still-unimplemented CPU multiply instruction (all array
 * indices below are compile-time constants, not runtime-computed).
 */

typedef unsigned int uint32;
typedef int boolean;

/* IShell / IModule: same minimal stand-ins as hello_brew.c -- this app
 * doesn't call IShell either, just needs the right pointer shape. */
typedef struct { void *fn[42]; } IShellVtbl;
typedef struct { IShellVtbl *pvt; } IShell;

/* IGL: 80 vtable slots (AddRef/Release/QueryInterface + 77 gl* methods).
 * IEGL: 28 vtable slots (AddRef/Release/QueryInterface + 25 egl*
 * methods). Real order verified against AEEGL.h -- mirrors gl_hle.cpp's
 * BuildGl/BuildEgl exactly. */
typedef struct { void *fn[80]; } IGLVtbl;
typedef struct { IGLVtbl *pvt; } IGL;
typedef struct { void *fn[28]; } IEGLVtbl;
typedef struct { IEGLVtbl *pvt; } IEGL;

typedef struct {
  IGL *pIGL;
  IEGL *pIEGL;
} GlDemoParams;

typedef unsigned int EGLDisplayT;
typedef unsigned int EGLSurfaceT;
typedef unsigned int EGLContextT;
typedef unsigned int EGLConfigT;
typedef unsigned int EGLBooleanT;

typedef EGLDisplayT (*EglGetDisplayFn)(unsigned int);
typedef EGLBooleanT (*EglInitializeFn)(EGLDisplayT, int *, int *);
typedef EGLBooleanT (*EglChooseConfigFn)(EGLDisplayT, const int *, EGLConfigT *, int, int *);
typedef EGLSurfaceT (*EglCreateWindowSurfaceFn)(EGLDisplayT, EGLConfigT, unsigned int, const int *);
typedef EGLContextT (*EglCreateContextFn)(EGLDisplayT, EGLConfigT, EGLContextT, const int *);
typedef EGLBooleanT (*EglMakeCurrentFn)(EGLDisplayT, EGLSurfaceT, EGLSurfaceT, EGLContextT);
typedef EGLBooleanT (*EglSwapBuffersFn)(EGLDisplayT, EGLSurfaceT);

typedef void (*GlClearFn)(unsigned int);
typedef void (*GlClearColorxFn)(int, int, int, int);
typedef void (*GlViewportFn)(int, int, int, int);
typedef void (*GlMatrixModeFn)(unsigned int);
typedef void (*GlLoadIdentityFn)(void);
typedef void (*GlOrthoxFn)(int, int, int, int, int, int);
typedef void (*GlEnableClientStateFn)(unsigned int);
typedef void (*GlVertexPointerFn)(int, unsigned int, int, const void *);
typedef void (*GlColorPointerFn)(int, unsigned int, int, const void *);
typedef void (*GlDrawArraysFn)(unsigned int, int, int);

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_VERTEX_ARRAY 0x8074
#define GL_COLOR_ARRAY 0x8076
#define GL_TRIANGLES 0x0004
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401

#define FIX_ONE 0x00010000

/* A single hardcoded triangle -- no per-vertex loop, so no runtime
 * index multiplication is ever needed to walk these arrays. */
static const signed char kTriangleVerts[6] = {
    0,  40,  /* top */
    -40, -40, /* bottom-left */
    40, -40,  /* bottom-right */
};
static const unsigned char kTriangleColors[12] = {
    255, 0,   0,   255, /* red */
    0,   255, 0,   255, /* green */
    0,   0,   255, 255, /* blue */
};

static boolean RunGlDemo(GlDemoParams *params) {
  IGL *pIGL = params->pIGL;
  IEGL *pIEGL = params->pIEGL;

  EglGetDisplayFn eglGetDisplay = (EglGetDisplayFn)pIEGL->pvt->fn[4];
  EglInitializeFn eglInitialize = (EglInitializeFn)pIEGL->pvt->fn[5];
  EglChooseConfigFn eglChooseConfig = (EglChooseConfigFn)pIEGL->pvt->fn[10];
  EglCreateWindowSurfaceFn eglCreateWindowSurface =
      (EglCreateWindowSurfaceFn)pIEGL->pvt->fn[12];
  EglCreateContextFn eglCreateContext = (EglCreateContextFn)pIEGL->pvt->fn[17];
  EglMakeCurrentFn eglMakeCurrent = (EglMakeCurrentFn)pIEGL->pvt->fn[19];
  EglSwapBuffersFn eglSwapBuffers = (EglSwapBuffersFn)pIEGL->pvt->fn[26];

  GlClearFn glClear = (GlClearFn)pIGL->pvt->fn[7];
  GlClearColorxFn glClearColorx = (GlClearColorxFn)pIGL->pvt->fn[8];
  GlViewportFn glViewport = (GlViewportFn)pIGL->pvt->fn[79];
  GlMatrixModeFn glMatrixMode = (GlMatrixModeFn)pIGL->pvt->fn[51];
  GlLoadIdentityFn glLoadIdentity = (GlLoadIdentityFn)pIGL->pvt->fn[46];
  GlOrthoxFn glOrthox = (GlOrthoxFn)pIGL->pvt->fn[56];
  GlEnableClientStateFn glEnableClientState = (GlEnableClientStateFn)pIGL->pvt->fn[29];
  GlVertexPointerFn glVertexPointer = (GlVertexPointerFn)pIGL->pvt->fn[78];
  GlColorPointerFn glColorPointer = (GlColorPointerFn)pIGL->pvt->fn[14];
  GlDrawArraysFn glDrawArrays = (GlDrawArraysFn)pIGL->pvt->fn[26];

  EGLConfigT config;
  int num_config;
  unsigned int display = eglGetDisplay(0);
  eglInitialize(display, 0, 0);
  eglChooseConfig(display, 0, &config, 1, &num_config);
  unsigned int surface = eglCreateWindowSurface(display, config, 0, 0);
  unsigned int context = eglCreateContext(display, config, 0, 0);
  eglMakeCurrent(display, surface, surface, context);

  glViewport(0, 0, 640, 480);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrthox(-64 * FIX_ONE, 64 * FIX_ONE, -48 * FIX_ONE, 48 * FIX_ONE, -FIX_ONE, FIX_ONE);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glClearColorx(0, 0, 0, FIX_ONE);
  glClear(GL_COLOR_BUFFER_BIT);

  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_BYTE, 0, kTriangleVerts);
  glEnableClientState(GL_COLOR_ARRAY);
  glColorPointer(4, GL_UNSIGNED_BYTE, 0, kTriangleColors);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  eglSwapBuffers(display, surface);
  return 1;
}

/* Arbitrary shared constant for this test only, exactly like
 * hello_brew.c's TEST_EVT_APP_START -- both sides of this test (app +
 * harness) just need to agree on it. */
#define TEST_EVT_GL_DEMO 2

static boolean HandleEvent(void *pMe, int eventCode, unsigned short wParam, uint32 dwParam) {
  (void)pMe;
  (void)wParam;
  if (eventCode == TEST_EVT_GL_DEMO) {
    return RunGlDemo((GlDemoParams *)dwParam);
  }
  return 0;
}

/* --- Module: our own minimal "IModule" stand-in, identical shape to
 * hello_brew.c's --- */
typedef struct {
  void *fn[4]; /* AddRef, Release, CreateInstance, FreeResources */
} IModuleVtbl;
typedef struct {
  IModuleVtbl *pvt;
} IModule;

static int ModuleCreateInstance(IModule *pMod, IShell *pIShell, uint32 ClsId, void **ppObj) {
  (void)pMod;
  (void)pIShell;
  (void)ClsId;
  *ppObj = (void *)HandleEvent;
  return 0; /* AEE_SUCCESS */
}

static IModuleVtbl g_moduleVtbl = {
    {0 /* AddRef */, 0 /* Release */, (void *)ModuleCreateInstance, 0 /* FreeResources */}};
static IModule g_module = {&g_moduleVtbl};

/* Entry point. Must be the first function in the file -- see
 * hello_brew.c's README.md for why, and how to re-verify its offset
 * after any change to this file. */
int AEEMod_Load(IShell *pIShell, void *ph, IModule **ppMod) {
  (void)pIShell;
  (void)ph;
  *ppMod = &g_module;
  return 0; /* AEE_SUCCESS */
}
