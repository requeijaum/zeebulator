/* Our own minimal test app, written from scratch -- NOT compiled
 * Qualcomm/BREW SDK code. It exercises the real BREW app-lifecycle
 * contract (AEEMod_Load -> IModule::CreateInstance -> HandleEvent on
 * app start) as reverse-engineered from the official AEEModGen.c /
 * AEEAppGen.c reference sources (see TASKS.md Phase 3), and calls
 * through an IDisplay vtable using the real, verified Qualcomm slot
 * order (AEEIDisplay.h) so it's a meaningful test of loader/HLE
 * compatibility with real compiled BREW code -- not just an arbitrary
 * C function.
 *
 * Deliberately skips AEEModGen.c/AEEAppGen.c's own heap-allocated
 * IModule/IApplet machinery: this app uses static (not malloc'd)
 * objects instead, so it doesn't require a heap-allocator HLE. That's a
 * legitimate simplification for OUR test app; it isn't a requirement
 * real compiled games will get to skip.
 */

/* AECHAR is a real single 8-bit byte per character on real Zeebo/BREW
 * hardware -- not the 16-bit UTF-16 code unit real BREW's AEEText.h
 * documents as the general case (see IDisplayHle::DrawText's own doc
 * comment, core/brew/idisplay.cpp, for the real evidence this was
 * corrected against: a real Double Dragon DrawText call site's in-memory
 * string only decodes to legible English text when read one byte per
 * character). Matches real hardware rather than the generic
 * documentation on purpose, since this fixture exists to test
 * loader/HLE compatibility with real compiled code. */
typedef unsigned char AECHAR;
typedef unsigned int uint32;
typedef int boolean;

/* IShell: 42 vtable slots, order verified against real AEEIShell.h.
 * This app never calls any of them -- it only needs to exist as a
 * pointer of the right shape. */
typedef struct {
  void *fn[42];
} IShellVtbl;
typedef struct {
  IShellVtbl *pvt;
} IShell;

/* IDisplay: 13 vtable slots, order verified against real
 * AEEIDisplay.h. DrawText = slot 4, Update = slot 7. */
typedef struct {
  void *fn[13];
} IDisplayVtbl;
typedef struct {
  IDisplayVtbl *pvt;
} IDisplay;

typedef struct {
  int x, y, dx, dy;
} AEERect;

/* Matches the real AEEShell.h AEEAppStart layout: error, clsApp,
 * pDisplay, rc -- this is what EVT_APP_START's dwParam points at. */
typedef struct {
  int error;
  uint32 clsApp;
  IDisplay *pDisplay;
  AEERect rc;
} AEEAppStart;

typedef int (*DrawTextFn)(IDisplay *, int, const AECHAR *, int, int, int,
                           const AEERect *, uint32);
typedef void (*UpdateFn)(IDisplay *, boolean);

/* Arbitrary shared constant for this test only -- not the real BREW
 * numeric value for EVT_APP_START (we don't have AEEVCodes.h to confirm
 * it), but nothing here depends on matching Qualcomm's real value since
 * both sides of this test (app + loader) agree on it directly. */
#define TEST_EVT_APP_START 1

static const AECHAR kHello[] = {'H', 'e', 'l', 'l', 'o', 0};

/* --- Applet: our own minimal "IApplet" stand-in ---
 * Real BREW returns a full IApplet object; we simplify to returning the
 * HandleEvent function pointer directly, since that's the only part of
 * it our loader actually needs to drive the app -- see the .h-equivalent
 * comment in the test harness for how this address gets used. */
static boolean HandleEvent(void *pMe, int eventCode, unsigned short wParam,
                            uint32 dwParam) {
  (void)pMe;
  (void)wParam;
  if (eventCode == TEST_EVT_APP_START) {
    AEEAppStart *pStart = (AEEAppStart *)dwParam;
    IDisplay *pDisplay = pStart->pDisplay;
    DrawTextFn drawText = (DrawTextFn)pDisplay->pvt->fn[4];
    UpdateFn update = (UpdateFn)pDisplay->pvt->fn[7];
    drawText(pDisplay, 0, kHello, -1, 10, 10, 0, 0);
    update(pDisplay, 0);
    return 1;
  }
  return 0;
}

/* --- Module: our own minimal "IModule" stand-in --- */
typedef struct {
  void *fn[4]; /* AddRef, Release, CreateInstance, FreeResources */
} IModuleVtbl;
typedef struct {
  IModuleVtbl *pvt;
} IModule;

static int ModuleCreateInstance(IModule *pMod, IShell *pIShell, uint32 ClsId,
                                 void **ppObj) {
  (void)pMod;
  (void)pIShell;
  (void)ClsId;
  *ppObj = (void *)HandleEvent;
  return 0; /* AEE_SUCCESS */
}

static IModuleVtbl g_moduleVtbl = {
    {0 /* AddRef */, 0 /* Release */, (void *)ModuleCreateInstance,
     0 /* FreeResources */}};
static IModule g_module = {&g_moduleVtbl};

/* Entry point. Real BREW calls this AEEMod_Load and requires it to be
 * the very first thing in the module (see AEEModGen.c) -- our loader
 * enforces that the same way: it's the first (and only) function placed
 * in this translation unit, at the start of the flat image. */
int AEEMod_Load(IShell *pIShell, void *ph, IModule **ppMod) {
  (void)pIShell;
  (void)ph;
  *ppMod = &g_module;
  return 0; /* AEE_SUCCESS */
}
