#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include "cache.h"
#include "defs.h"
#include "info.h"
#include "logging.h"
#include "rpi-aux.h"
#include "rpi-gpio.h"
#include "rpi-interrupts.h"
#include "rpi-mailbox-interface.h"
#include "startup.h"
#include "rpi-mailbox.h"
#include "osd.h"
#include "cpld.h"
#include "cpld_atom.h"
#include "cpld_rgb.h"
#include "cpld_yuv.h"
#include "cpld_null.h"
#include "geometry.h"
#include "filesystem.h"
#include "rgb_to_fb.h"

// #define INSTRUMENT_CAL
#define NUM_CAL_PASSES 1

typedef void (*func_ptr)();

// =============================================================
// Define the PLL to be used for the sampling clock
// =============================================================
//
// Choose between PLLA, PLLC and PLLD
//
// PLLA - not otherwise used
// PLLC - used for the Core Clock
// PLLD - possibly used for the SDRAM Clock, so might overclock it by 20%
//
// TODO: PLLA doesn't currently start on the Pi2/Pi3
//
// Power-on defaults are values for the Pi Zero

// SYS_CLK_DIVIDER is the ratio between PLLC and the Core Clock
// This is typically 3 or 4, depending on the Pi Model. We need
// to know this to correct serial speed when PLLC used.

#if defined(RPI3)
#define USE_PLLC
#define SYS_CLK_DIVIDER 3
#elif defined(RPI2)
#define USE_PLLC
#define SYS_CLK_DIVIDER 4
#else
#define USE_PLLA
#define SYS_CLK_DIVIDER 3
#endif

#ifdef USE_PLLA
#define PLL_NAME              "PLLA"      // power-on default = off
#define GPCLK_SOURCE               4      // PLLA_PER used as source
#define DEFAULT_GPCLK_DIVISOR      6      // 600MHz / 6 = 100MHz
#define PLL_CTRL           PLLA_CTRL
#define PLL_FRAC           PLLA_FRAC
#define PLL_SCALE                  2
#define MIN_PLL_FREQ       800000000      // PLLA_PER = 400MHz
#define MAX_PLL_FREQ      1200000000      // PLLA_PER = 600MHz
#endif

#ifdef USE_PLLC
#define PLL_NAME              "PLLC"      // power-on default = 1200MHz
#define GPCLK_SOURCE               5      // PLLC_PER used as source
#define DEFAULT_GPCLK_DIVISOR     12      // 1200MHz / 12 = 100MHz
#define PLL_CTRL           PLLC_CTRL
#define PLL_FRAC           PLLC_FRAC
#define PLL_SCALE                  1
#define MIN_PLL_FREQ       900000000     // PLLC_PER = 900MHz
#define MAX_PLL_FREQ      1200000000     // PLLC_PER = 1200MHz
#endif

#ifdef USE_PLLD
#define PLL_NAME              "PLLD"      // power-on default = 500MHz
#define GPCLK_SOURCE               6      // PLLD_PER used as source
#define DEFAULT_GPCLK_DIVISOR      5      // 500MHz / 5 = 100MHz
#define PLL_CTRL           PLLD_CTRL
#define PLL_FRAC           PLLD_FRAC
#define PLL_SCALE                  2
#define MIN_PLL_FREQ       800000000     // PLLD_PER = 400MHz
#define MAX_PLL_FREQ      1200000000     // PLLD_PER = 600MHz
#endif


// =============================================================
// Forward declarations
// =============================================================

static void cpld_init();

// =============================================================
// Global variables
// =============================================================

cpld_t *cpld = NULL;
int clock_error_ppm = 0;
int vsync_time_ns = 0;
capture_info_t *capinfo;
clk_info_t clkinfo;

// =============================================================
// Local variables
// =============================================================

static capture_info_t default_capinfo  __attribute__((aligned(32)));
static capture_info_t mode7_capinfo    __attribute__((aligned(32)));
static uint32_t cpld_version_id;
static int mode7;
static int paletteControl = PALETTECONTROL_INBAND;
static int interlaced;
static int clear;
static volatile int delay;
static double pllh_clock = 0;
static int genlocked = 0;
static int resync_count = 0;
static int target_difference = 0;
static int source_vsync_freq_hz = 0;
static int display_vsync_freq_hz = 0;
static char status[256];
static int restart_profile = 0;
// =============================================================
// OSD parameters
// =============================================================
static int profile     = 0;
static int subprofile  = 0;
static int resolution  = 0;
static char resolution_name[MAX_RESOLUTION_WIDTH];
static int scaling     = 0;
static int frontend    = 0;
static int border      = 0;
static int elk         = 0;
static int debug       = 0;
static int autoswitch  = 2;
static int scanlines   = 0;
static int scanlines_intensity = 0;
static int colour      = 0;
static int invert      = 0;
static int fontsize    = 0;
static int deinterlace = 6;
static int vsync       = 0;
static int vlockmode   = 1;
static int vlockline   = 10;
static int vlockspeed  = 2;
static int vlockadj    = 0;
static int lines_per_frame = 0;
static int one_line_time_ns = 0;
static int adjusted_clock;
static int reboot_required = 0;
static int resolution_warning = 0;
static int vlock_limited = 0;
static int current_display_buffer = 0;
#ifdef MULTI_BUFFER
static int nbuffers    = 0;
#endif

static int current_vlockmode = -1;
static const char *sync_names[] = {
    "-H-V",
    "+H-V",
    "-H+V",
    "+H+V",
    "Comp",
    "InvComp"
};
static const char *sync_names_long[] = {
    "Separate -H -V",
    "Separate +H -V",
    "Separate -H +V",
    "Separate +H +V",
    "Composite",
    "Inverted Composite"
};
static const char *mixed_names[] = {
    "Separate H & V CPLD",
    "Mixed H & V CPLD"
};
// Calculated so that the constants from librpitx work
static volatile uint32_t *gpioreg = (volatile uint32_t *)(PERIPHERAL_BASE + 0x101000UL);

// Temporary buffer that must be at least as large as a frame buffer
static unsigned char last[2048 * 1024] __attribute__((aligned(32)));

#ifndef USE_PROPERTY_INTERFACE_FOR_FB
typedef struct {
   uint32_t width;
   uint32_t height;
   uint32_t virtual_width;
   uint32_t virtual_height;
   volatile uint32_t pitch;
   volatile uint32_t depth;
   uint32_t x_offset;
   uint32_t y_offset;
   volatile uint32_t pointer;
   volatile uint32_t size;
} framebuf;
// The + 0x10000 is to miss the property buffer
static framebuf *fbp = (framebuf *) (UNCACHED_MEM_BASE + 0x10000);
#endif

// =============================================================
// Private methods
// =============================================================

void reboot() {
	*PM_WDOG = PM_PASSWORD | 1;
	*PM_RSTC = PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET;
	while(1);
}

// 0     0 Hz     Ground
// 1     19.2 MHz oscillator
// 2     0 Hz     testdebug0
// 3     0 Hz     testdebug1
// 4     0 Hz     PLLA
// 5     1000 MHz PLLC (changes with overclock settings)
// 6     500 MHz  PLLD
// 7     216 MHz  HDMI auxiliary
// 8-15  0 Hz     Ground

// Source 1 = OSC = 19.2MHz
// Source 4 = PLLA = 0MHz
// Source 5 = PLLC = core_freq * 3
// Source 6 = PLLD = 500MHz

static void init_gpclk(int source, int divisor) {
   log_debug("A GP_CLK1_DIV = %08"PRIx32, *GP_CLK1_DIV);

   log_debug("B GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   // Stop the clock generator (retaining the existing source)
   *GP_CLK1_CTL = CM_PASSWORD | ((*GP_CLK1_CTL) & ~GZ_CLK_ENA);

   // Wait for BUSY low
   log_debug("C GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);
   while ((*GP_CLK1_CTL) & GZ_CLK_BUSY) {}
   log_debug("D GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   // Configure the clock generator
   *GP_CLK1_CTL = CM_PASSWORD | source;
   *GP_CLK1_DIV = CM_PASSWORD | (divisor << 12);

   log_debug("E GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   // Start the clock generator
   *GP_CLK1_CTL = CM_PASSWORD | (source | GZ_CLK_ENA);

   log_debug("F GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);
   while (!((*GP_CLK1_CTL) & GZ_CLK_BUSY)) {}    // Wait for BUSY high
   log_debug("G GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   log_debug("H GP_CLK1_DIV = %08"PRIx32, *GP_CLK1_DIV);
}

#ifdef USE_PROPERTY_INTERFACE_FOR_FB

static void init_framebuffer(capture_info_t *capinfo) {
static int last_width = -1;
static int last_height = -1;

   rpi_mailbox_property_t *mp;

   if (capinfo->width != last_width || capinfo->height != last_height) {

       // Fill in the frame buffer structure with a small dummy frame buffer first
       /* Initialise a framebuffer... */
       RPI_PropertyInit();
       RPI_PropertyAddTag(TAG_ALLOCATE_BUFFER, 0x02000000);
       RPI_PropertyAddTag(TAG_SET_PHYSICAL_SIZE, 64, 64);
    #ifdef MULTI_BUFFER
       RPI_PropertyAddTag(TAG_SET_VIRTUAL_SIZE, 64, 64);
    #else
       RPI_PropertyAddTag(TAG_SET_VIRTUAL_SIZE, 64, 64);
    #endif
       RPI_PropertyAddTag(TAG_SET_DEPTH, capinfo->bpp);

       RPI_PropertyProcess();

       // FIXME: A small delay (like the log) is neccessary here
       // or the RPI_PropertyGet seems to return garbage
        log_info("Width or Height differ from last FB: Setting dummy 64x64 framebuffer");
   }

   /* work out if overscan needed */

   uint32_t h_size = (*PIXELVALVE2_HORZB) & 0xFFFF;
   uint32_t v_size = (*PIXELVALVE2_VERTB) & 0xFFFF;

   int width = capinfo->width >> ((capinfo->sizex2 & 2) >> 1);
   int height = capinfo->height >> (capinfo->sizex2 & 1);

   int actual_h_size = h_size / width * width;
   int actual_v_size = v_size / height * height;

   int h_overscan = h_size - actual_h_size;
   int v_overscan = v_size - actual_v_size;

   int left_overscan = h_overscan >> 1;
   int right_overscan = left_overscan + (h_overscan & 1);

   int top_overscan = v_overscan >> 1;
   int bottom_overscan = top_overscan + (v_overscan & 1);

   log_info("Overscan L=%d, R=%d, T=%d, B=%d",left_overscan, right_overscan, top_overscan, bottom_overscan);

   /* Initialise a framebuffer... */
   RPI_PropertyInit();
   RPI_PropertyAddTag(TAG_ALLOCATE_BUFFER, 0x02000000);
   RPI_PropertyAddTag(TAG_SET_PHYSICAL_SIZE, capinfo->width, capinfo->height);
#ifdef MULTI_BUFFER
   RPI_PropertyAddTag(TAG_SET_VIRTUAL_SIZE, capinfo->width, capinfo->height * NBUFFERS);
#else
   RPI_PropertyAddTag(TAG_SET_VIRTUAL_SIZE, capinfo->width, capinfo->height);
#endif
   RPI_PropertyAddTag(TAG_SET_DEPTH, capinfo->bpp);
   RPI_PropertyAddTag(TAG_SET_OVERSCAN, top_overscan, bottom_overscan, left_overscan, right_overscan);
   RPI_PropertyAddTag(TAG_GET_PITCH);
   RPI_PropertyAddTag(TAG_GET_PHYSICAL_SIZE);
   RPI_PropertyAddTag(TAG_GET_DEPTH);

   RPI_PropertyProcess();

   // FIXME: A small delay (like the log) is neccessary here
   // or the RPI_PropertyGet seems to return garbage
   log_info("Initialised Framebuffer");

   if ((mp = RPI_PropertyGet(TAG_GET_PHYSICAL_SIZE))) {
      int width = mp->data.buffer_32[0];
      int height = mp->data.buffer_32[1];
      log_info("Size: %dx%d ", width, height);
   }

   if ((mp = RPI_PropertyGet(TAG_GET_PITCH))) {
      capinfo->pitch = mp->data.buffer_32[0];
      log_info("Pitch: %d bytes", capinfo->pitch);
   }

   if ((mp = RPI_PropertyGet(TAG_ALLOCATE_BUFFER))) {
      capinfo->fb = (unsigned char*)mp->data.buffer_32[0];
      log_info("Framebuffer address: %8.8X", (unsigned int)capinfo->fb);
   }
   // On the Pi 2/3 the mailbox returns the address with bits 31..30 set, which is wrong
   capinfo->fb = (unsigned char *)(((unsigned int) capinfo->fb) & 0x3fffffff);

   // Initialize the palette
   osd_update_palette();
}

#else

// An alternative way to initialize the framebuffer using mailbox channel 1
//
// I was hoping it would then be possible to page flip just by modifying the structure
// in-place. Unfortunately that didn't work, but the code might be useful in the future.

static void init_framebuffer(capture_info_t *capinfo) {
   static int last_width = -1;
   static int last_height = -1;

   log_debug("Framebuf struct address: %p", fbp);

   if (capinfo->width != last_width || capinfo->height != last_height) {
       log_info("Width or Height differ from last FB: Setting dummy 64x64 framebuffer");
       // Fill in the frame buffer structure with a small dummy frame buffer first
       fbp->width          = 64;
       fbp->height         = 64;
       fbp->virtual_width  = 64;
    #ifdef MULTI_BUFFER
       fbp->virtual_height = 64;
    #else
       fbp->virtual_height = 64;
    #endif
       fbp->pitch          = 0;
       fbp->depth          = capinfo->bpp;
       fbp->x_offset       = 0;
       fbp->y_offset       = 0;
       fbp->pointer        = 0;
       fbp->size           = 0;

       // Send framebuffer struct to the mailbox
       //
       // The +0x40000000 ensures the GPU bypasses it's cache when accessing
       // the framebuffer struct. If this is not done, the screen still initializes
       // but the ARM doesn't see the updated value for a very long time
       // i.e. the commented out section of code below is needed, and this eventually
       // exits with i=4603039
       //
       // 0xC0000000 should be added if disable_l2cache=1
       RPI_Mailbox0Write(MB0_FRAMEBUFFER, ((unsigned int)fbp) + 0xC0000000);

       // Wait for the response (0)
       RPI_Mailbox0Read(MB0_FRAMEBUFFER);
   }

   last_width = capinfo->width;
   last_height = capinfo->height;

   // Fill in the frame buffer structure
   fbp->width          = capinfo->width;
   fbp->height         = capinfo->height;
   fbp->virtual_width  = capinfo->width;
#ifdef MULTI_BUFFER
   fbp->virtual_height = capinfo->height * NBUFFERS;
#else
   fbp->virtual_height = capinfo->height;
#endif
   fbp->pitch          = 0;
   fbp->depth          = capinfo->bpp;
   fbp->x_offset       = 0;
   fbp->y_offset       = 0;
   fbp->pointer        = 0;
   fbp->size           = 0;

   // Send framebuffer struct to the mailbox
   //
   // The +0x40000000 ensures the GPU bypasses it's cache when accessing
   // the framebuffer struct. If this is not done, the screen still initializes
   // but the ARM doesn't see the updated value for a very long time
   // i.e. the commented out section of code below is needed, and this eventually
   // exits with i=4603039
   //
   // 0xC0000000 should be added if disable_l2cache=1
   RPI_Mailbox0Write(MB0_FRAMEBUFFER, ((unsigned int)fbp) + 0xC0000000);

   // Wait for the response (0)
   RPI_Mailbox0Read(MB0_FRAMEBUFFER);

   capinfo->pitch = fbp->pitch;
   capinfo->fb = (unsigned char*)(fbp->pointer);
   int width = fbp->width;
   int height = fbp->height;

   // See comment above
   // int i  = 0;
   // while (!pitch || !fb) {
   //    pitch = fbp->pitch;
   //    fb = (unsigned char*)(fbp->pointer);
   //    i++;
   // }
   // log_info("i=%d", i);

   log_info("Initialised Framebuffer: %dx%d ", width, height);
   log_info("Pitch: %d bytes", capinfo->pitch);
   log_debug("Framebuffer address: %8.8X", (unsigned int)capinfo->fb);

   // On the Pi 2/3 the mailbox returns the address with bits 31..30 set, which is wrong
   capinfo->fb = (unsigned char *)(((unsigned int) capinfo->fb) & 0x3fffffff);

   // Initialize the palette
   osd_update_palette();
}

#endif

void log_plla() {
   double clock = 19.2 * ((double)(gpioreg[PLLA_CTRL] & 0x3ff) + ((double)gpioreg[PLLA_FRAC]) / ((double)(1 << 20)));
   log_debug("PLLA: %lf", clock);
   log_debug("PLLA: PDIV=%d NDIV=%d CTRL=%08x FRAC=%d DSI0=%d CORE=%d PER=%d CCP2=%d",
             (gpioreg[PLLA_CTRL] >> 12) & 0x7,
             gpioreg[PLLA_CTRL] & 0x3ff,
             gpioreg[PLLA_CTRL],
             gpioreg[PLLA_FRAC],
             gpioreg[PLLA_DSI0],
             gpioreg[PLLA_CORE],
             gpioreg[PLLA_PER],
             gpioreg[PLLA_CCP2]);
}

void log_pllb() {
   double clock = 19.2 * ((double)(gpioreg[PLLB_CTRL] & 0x3ff) + ((double)gpioreg[PLLB_FRAC]) / ((double)(1 << 20)));
   log_debug("PLLB: %lf", clock);
   log_debug("PLLB: PDIV=%d NDIV=%d CTRL=%08x FRAC=%d ARM=%d SP0=%d SP1=%d SP2=%d",
             (gpioreg[PLLB_CTRL] >> 12) & 0x7,
             gpioreg[PLLB_CTRL] & 0x3ff,
             gpioreg[PLLB_CTRL],
             gpioreg[PLLB_FRAC],
             gpioreg[PLLB_ARM],
             gpioreg[PLLB_SP0],
             gpioreg[PLLB_SP1],
             gpioreg[PLLB_SP2]);
}

void log_pllc() {
   double clock = 19.2 * ((double)(gpioreg[PLLC_CTRL] & 0x3ff) + ((double)gpioreg[PLLC_FRAC]) / ((double)(1 << 20)));
   log_debug("PLLC: %lf", clock);
   log_debug("PLLC: PDIV=%d NDIV=%d CTRL=%08x FRAC=%d CORE2=%d CORE1=%d PER=%d CORE0=%d",
             (gpioreg[PLLC_CTRL] >> 12) & 0x7,
             gpioreg[PLLC_CTRL] & 0x3ff,
             gpioreg[PLLC_CTRL],
             gpioreg[PLLC_FRAC],
             gpioreg[PLLC_CORE2],
             gpioreg[PLLC_CORE1],
             gpioreg[PLLC_PER],
             gpioreg[PLLC_CORE0]);
}

void log_plld() {
   double clock = 19.2 * ((double)(gpioreg[PLLD_CTRL] & 0x3ff) + ((double)gpioreg[PLLD_FRAC]) / ((double)(1 << 20)));
   log_debug("PLLD: %lf", clock);
   log_debug("PLLD: PDIV=%d NDIV=%d CTRL=%08x FRAC=%d DSI0=%d CORE=%d PER=%d DSI1=%d",
             (gpioreg[PLLD_CTRL] >> 12) & 0x7,
             gpioreg[PLLD_CTRL] & 0x3ff,
             gpioreg[PLLD_CTRL],
             gpioreg[PLLD_FRAC],
             gpioreg[PLLD_DSI0],
             gpioreg[PLLD_CORE],
             gpioreg[PLLD_PER],
             gpioreg[PLLD_DSI1]);
}

void log_pllh() {
   log_debug("PLLH: PDIV=%d NDIV=%d CTRL=%08x FRAC=%d AUX=%d RCAL=%d PIX=%d STS=%d",
             (gpioreg[PLLH_CTRL] >> 12) & 0x7,
             gpioreg[PLLH_CTRL] & 0x3ff,
             gpioreg[PLLH_CTRL],
             gpioreg[PLLH_FRAC],
             gpioreg[PLLH_AUX],
             gpioreg[PLLH_RCAL],
             gpioreg[PLLH_PIX],
             gpioreg[PLLH_STS]);
}

void set_pll_frequency(double f, int pll_ctrl, int pll_fract) {
   // Calculate the new dividers
   int div = (int) (f / 19.2);
   int fract = (int) ((double)(1<<20) * (f / 19.2 - (double) div));
   // Sanity check the range of the fractional divider (it should actually always be in range)
   if (fract < 0) {
      log_warn("PLL fraction < 0");
      fract = 0;
   }
   if (fract > (1<<20) - 1) {
      log_warn("PLL fraction > 1");
      fract = (1<<20) - 1;
   }

   // Read the existing values
   int old_ctrl = gpioreg[pll_ctrl];
   int old_div = old_ctrl & 0x3ff;
   int old_fract = gpioreg[pll_fract];

   // Check if there's been a change
   if (div != old_div || fract != old_fract) {

#ifdef USE_PLLC
      // Flush the UART, as the Core Clock is about to change
      RPI_AuxMiniUartFlush();
#endif

      // Update the integer divider
      if (div != old_div) {
         gpioreg[pll_ctrl] = CM_PASSWORD | (old_ctrl & 0x00FFFC00) | div;
      }

      // Update the fractional divider
      if (fract != old_fract) {
         gpioreg[pll_fract] = CM_PASSWORD | fract;
      }

      // Re-read the integer divider if it's changed
      if (div != old_div) {
         int new_ctrl = gpioreg[pll_ctrl];
         int new_div = new_ctrl & 0x3ff;
         if (new_div == div) {
            log_debug("   New int divider: %d", new_div);
         } else {
            log_warn("Failed to write int divider: wrote %d, read back %d", div, new_div);
         }
      }

      // Re-read the fraction divider if it's changed
      if (fract != old_fract) {
         int new_fract = gpioreg[pll_fract];
         if (new_fract == fract) {
            log_debug(" New fract divider: %d", new_fract);
         } else {
            log_warn("Failed to write fract divider: wrote %d, read back %d", fract, new_fract);
         }
      }
   }
}


static int calibrate_sampling_clock() {
   int a = 13;
   static int old_pll_freq = 0;
   static int old_clock = 0;
   // Default values for the Beeb
   clkinfo.clock      = 16000000;
   clkinfo.line_len   = 1024;

   // Update from configuration
   geometry_get_clk_params(&clkinfo);

   log_info("        clkinfo.clock = %d Hz",  clkinfo.clock);
   log_info("     clkinfo.line_len = %d",     clkinfo.line_len);
   log_info("    clkinfo.clock_ppm = %d ppm", clkinfo.clock_ppm);

   int         nlines = 100; // Measure over N=100 lines
   int  nlines_ref_ns = nlines * (int) (1e9 * ((double) clkinfo.line_len) / ((double) clkinfo.clock));
   int  nlines_time_ns = measure_n_lines(nlines);
   log_info("    Nominal %3d lines = %d ns", nlines, nlines_ref_ns);
   log_info("     Actual %3d lines = %d ns", nlines, nlines_time_ns);

   double error = (double) nlines_time_ns / (double) nlines_ref_ns;
   clock_error_ppm = ((error - 1.0) * 1e6);
   log_info("          Clock error = %d PPM", clock_error_ppm);

   int new_clock;

   if ((clkinfo.clock_ppm > 0 && abs(clock_error_ppm) > clkinfo.clock_ppm) || (sync_detected == 0)) {
      if (old_clock > 0 && sub_profiles_available(profile) == 0) {
         log_warn("PPM error too large, using previous clock");
         new_clock = old_clock;
         // work around problem with 24 Mhz mode 7 and labyrinth - can be removed when separate profiles used for BBC
         if (autoswitch == AUTOSWITCH_MODE7 && !mode7 && new_clock > 180000000) {
             log_warn("Compensating for 24 Mhz mode 7");
             new_clock >>= 1;
         }
      } else {
         log_warn("PPM error too large, using nominal clock");
         new_clock = clkinfo.clock * cpld->get_divider();
      }
   } else {
      new_clock = (int) (((double)  clkinfo.clock * cpld->get_divider()) / error);
   }

   old_clock = new_clock;

   adjusted_clock = new_clock / cpld->get_divider();

   log_info(" Error adjusted clock = %d Hz", adjusted_clock);

   // Pick the best value for pll_freq and gpclk_divisor
   int pll_scale     = PLL_SCALE;     // defined at the top
   int min_pll_freq  = MIN_PLL_FREQ;  // defined at the top
   int max_pll_freq  = MAX_PLL_FREQ;  // defined at the top
   int gpclk_divisor = max_pll_freq / pll_scale / new_clock;
   int pll_freq      = new_clock * pll_scale * gpclk_divisor ;
   log_info("        GPCLK Divisor = %d", gpclk_divisor);
   log_info(" Target PLL frequency = %d Hz", pll_freq);

   // sanity check
   if (pll_freq < min_pll_freq) {
      log_warn("PLL clock out of range, defaulting to minimum (%d Hz)", min_pll_freq);
      pll_freq = min_pll_freq;
   } else if (pll_freq > max_pll_freq) {
      log_warn("PLL clock out of range, defaulting to maxiumum (%d Hz)", max_pll_freq);
      pll_freq = max_pll_freq;
   }
   log_info(" Actual PLL frequency = %d Hz", pll_freq);

   // If the clock has changed from it's previous value, then actually change it
   if (pll_freq != old_pll_freq) {

      set_pll_frequency(((double) pll_freq) / 1e6, PLL_CTRL, PLL_FRAC);

#ifdef USE_PLLC
      // Reinitialize the UART as the Core Clock has changed
      RPI_AuxMiniUartInit_With_Freq(115200, 8, pll_freq / SYS_CLK_DIVIDER);
#endif

      // And remember for next time
      old_pll_freq = pll_freq;
   }

   // TODO: this should be superfluous (as the GPU is not changing the core clock)
   // However, if we remove it, the next osd_update_palette() call hangs
   get_clock_rate(CORE_CLK_ID);

   // Finally, set the new divisor
   log_debug("Setting up divisor");
   init_gpclk(GPCLK_SOURCE, gpclk_divisor);
   log_debug("Done setting up divisor");

   // Remeasure the hsync time
   nlines_time_ns = measure_n_lines(nlines);

   // Remeasure the vsync time
   vsync_time_ns = measure_vsync();

   // Ignore the interlaced flag, as this can be unreliable (e.g. Monsters)
   vsync_time_ns &= ~INTERLACED_FLAG;

   // Instead, calculate the number of lines per frame
   double lines_per_frame_double = ((double) vsync_time_ns) / (((double) nlines_time_ns) / ((double) nlines));

   one_line_time_ns = nlines_time_ns / nlines;

   // If number of lines is odd, then we must be interlaced
   interlaced = ((int)(lines_per_frame_double + 0.5)) % 2;

   // Log it

   if (interlaced) {
      lines_per_frame = (int) (lines_per_frame_double + 0.5);
      log_info("      Lines per frame = %d, (%g)", lines_per_frame, lines_per_frame_double);
      log_info("Actual frame time = %d ns (interlaced), line time = %d ns", vsync_time_ns, one_line_time_ns);
   } else {
      lines_per_frame = ((int) (lines_per_frame_double + 0.5) >> 1);
      log_info("      Lines per frame = %d, (%g)", lines_per_frame, lines_per_frame_double / 2);
      log_info("Actual frame time = %d ns (non-interlaced), line time = %d ns", vsync_time_ns / 2, one_line_time_ns);
   }

   // Invalidate the current vlock mode to force an updated, as vsync_time_ns will have changed
   current_vlockmode = -1;

   return a;
}

static void recalculate_hdmi_clock(int vlockmode, int genlock_adjust) {
   // The very first time we get called, vsync_time_ns has not been set
   // so exit gracefully
   if (vsync_time_ns == 0) {
       return;
   }

   // Dump the PLLH registers
   log_pllh();


   // Grab the original PLLH frequency once, at it's original value
   if (pllh_clock == 0) {
      pllh_clock = 19.2 * ((double)(gpioreg[PLLH_CTRL] & 0x3ff) + ((double)gpioreg[PLLH_FRAC]) / ((double)(1 << 20)));
   }

   //for (int i = 0; i < 32; i++) {
   //   log_debug("   PIXELVALVE2[%2d]: %08x", i, *(PIXELVALVE2_BASE + i));
   //}

   // Dump the PIXELVALVE2 registers
   log_debug(" PIXELVALVE2_HORZA: %08x", *PIXELVALVE2_HORZA);
   log_debug(" PIXELVALVE2_HORZB: %08x", *PIXELVALVE2_HORZB);
   log_debug(" PIXELVALVE2_VERTA: %08x", *PIXELVALVE2_VERTA);
   log_debug(" PIXELVALVE2_VERTB: %08x", *PIXELVALVE2_VERTB);

   // Work out the htotal and vtotal by summing the four  16-bit values:
   // A[31:16] - back porch width in pixels
   // A[15: 0] - synch width in pixels
   // B[31:16] - front porch width in pixels
   // B[15: 0] - active line width in pixels
   uint32_t htotal = (*PIXELVALVE2_HORZA) + (*PIXELVALVE2_HORZB);
   htotal = (htotal + (htotal >> 16)) & 0xFFFF;
   uint32_t vtotal = (*PIXELVALVE2_VERTA) + (*PIXELVALVE2_VERTB);
   vtotal = (vtotal + (vtotal >> 16)) & 0xFFFF;
   log_debug("           H-Total: %d pixels", htotal);
   log_debug("           V-Total: %d pixels", vtotal);

   // PLLH seems to use a fixed divider to generate the pixel clock
   int fixed_divider = 10;
   log_debug("     Fixed divider: %d", fixed_divider);

   // 720x576@50    PLLH: PDIV=1 NDIV=56 FRAC=262144 AUX=256 RCAL=256 PIX=4 STS=526655
   // 1920x1080@50  PLLH: PDIV=1 NDIV=77 FRAC=360448 AUX=256 RCAL=256 PIX=1 STS=526655
   //     An additional divider is used to get very low pixel clock rates ^
   int additional_divider = gpioreg[PLLH_PIX];
   log_debug("Additional divider: %d", additional_divider);

   // Calculate the pixel clock
   double pixel_clock = pllh_clock / ((double) fixed_divider) / ((double) additional_divider);
   log_debug("       Pixel Clock: %lf MHz", pixel_clock);

   // Calculate the error between the HDMI VSync and the Source VSync
   double source_vsync_freq = 2e9 / ((double) vsync_time_ns);
   double display_vsync_freq = 1e6 * pixel_clock / ((double) htotal) / ((double) vtotal);
   double error = display_vsync_freq / source_vsync_freq;
   double error_ppm = 1e6 * (error - 1.0);

   double f2 = pllh_clock;


   if (vlockmode != HDMI_ORIGINAL) {
      f2 /= error;
      f2 /= 1.0 + ((double) (genlock_adjust * GENLOCK_PPM_STEP) / 1000000);
   }

   // Sanity check HDMI pixel clock
   pixel_clock = f2 / ((double) fixed_divider) / ((double) additional_divider);

   vlock_limited = 0;

   if ((vlockadj == VLOCKADJ_NARROW) && (error_ppm < -50000 || error_ppm > 50000)) {
        f2 = pllh_clock;
        vlock_limited = 1;
   }

   int max_clock = MAX_PIXEL_CLOCK;
   if (vlockadj == VLOCKADJ_260MHZ) {
       max_clock = MAX_PIXEL_CLOCK_260;
   }

   if (pixel_clock < MIN_PIXEL_CLOCK) {
      log_debug("Pixel clock of %.2lf MHz is too low; leaving unchanged", pixel_clock);
      f2 = pllh_clock;
      vlock_limited = 1;
   } else if (pixel_clock > max_clock) {
      log_debug("Pixel clock of %.2lf MHz is too high; leaving unchanged", pixel_clock);
      f2 = pllh_clock;
      vlock_limited = 1;
   }

   log_debug(" Source vsync freq: %lf Hz (measured)",  source_vsync_freq);
   log_debug("Display vsync freq: %lf Hz",  display_vsync_freq);
   log_debug("       Vsync error: %lf ppm", error_ppm);
   log_debug("     Original PLLH: %lf MHz", pllh_clock);
   log_debug("       Target PLLH: %lf MHz", f2);
   source_vsync_freq_hz = (int) (source_vsync_freq + 0.5);
   display_vsync_freq_hz = (int) (display_vsync_freq + 0.5);

   set_pll_frequency(f2, PLLH_CTRL, PLLH_FRAC);

   // Dump the the actual PLL frequency
   double f3 = 19.2 * ((double)(gpioreg[PLLH_CTRL] & 0x3ff) + ((double)gpioreg[PLLH_FRAC]) / ((double)(1 << 20)));
   log_debug("        Final PLLH: %lf MHz", f3);

   log_pllh();
}

int recalculate_hdmi_clock_line_locked_update(int force) {
    static int framecount = 0;
    static int genlock_adjust = 0;
    static int last_vlock = -1;
    static int thresholds[GENLOCK_MAX_STEPS] = GENLOCK_THRESHOLDS;
    if (force) {
        last_vlock = 0x80000000;
        genlocked = 0;
        return 0;
    }
    lock_fail = 0;
    if (sync_detected && last_sync_detected) {
        int adjustment = 0;
        if (capinfo->nlines >= GENLOCK_NLINES_THRESHOLD) {
            adjustment = 1;
        }
        if (vlockmode != HDMI_EXACT) {
            genlocked = 0;
            target_difference = 0;
            resync_count = 0;
            genlock_adjust = 0;
            switch (vlockmode) {
                case HDMI_SLOW_2000PPM:
                    genlock_adjust = 6;
                    break;
                case HDMI_SLOW_1000PPM:
                    genlock_adjust = 3;
                    break;
                case HDMI_FAST_1000PPM:
                    genlock_adjust = -3;
                    break;
                case HDMI_FAST_2000PPM:
                    genlock_adjust = -6;
                    break;
            }
            if (last_vlock != vlockmode) {
                recalculate_hdmi_clock(vlockmode, genlock_adjust);
                last_vlock = vlockmode;
                framecount = 0;
            }
        } else {
            int max_steps = GENLOCK_MAX_STEPS;
            int locked_threshold = GENLOCK_LOCKED_THRESHOLD;
            int frame_delay = GENLOCK_FRAME_DELAY;
            if (vlockspeed == VLOCKSPEED_MEDIUM) {
                max_steps >>= 1;
                locked_threshold--;
                frame_delay <<= 1;
            } else {
                if (vlockspeed == VLOCKSPEED_SLOW) {
                    max_steps = 1;
                    locked_threshold = 1;
                    frame_delay <<= 1;
                }
            }
            signed int difference = (vsync_line >> adjustment) - ((total_lines >> adjustment) - vlockline);
            if (abs(difference) > (total_lines >> (adjustment + 1))) {
                difference = -difference;
            }
            if (genlocked == 1 && abs(difference) >= thresholds[locked_threshold]) {
                genlocked = 0;
                if (difference >= 0) {
                    target_difference = -2;
                } else {
                    target_difference = 2;
                }
                if (abs(difference) > thresholds[locked_threshold]) {
                    log_info("Lock lost probably due to mode change - resetting ReSync counter");
                    resync_count = 0;
                    target_difference = 0;
                    lock_fail = 1;
                } else {
                    log_info("ReSync: %d", ++resync_count);
                }
            }
            if(framecount == 0) {
                int new_genlock_adjust = genlock_adjust;
                if (genlocked == 0) {
                    if (difference - target_difference == 0) {
                        if (genlock_adjust < 0) {
                            new_genlock_adjust++;
                        }
                        if (genlock_adjust > 0) {
                            new_genlock_adjust--;
                        }
                        if (new_genlock_adjust == 0)
                        {
                            genlocked = 1;
                            target_difference = 0;
                            log_info("Locked");
                        }
                    } else {
                        if (difference >= target_difference) {
                            int threshold = 0;
                            if (genlock_adjust >= 0 && genlock_adjust < max_steps) {
                                threshold = thresholds[genlock_adjust];
                            }
                            if (genlock_adjust < max_steps && difference > threshold) {
                                new_genlock_adjust++;
                            }
                            if (genlock_adjust > 1 && difference <= thresholds[genlock_adjust - 1]) {
                                new_genlock_adjust--;
                            }
                        } else {
                            int threshold = 0;
                            if (genlock_adjust <= 0 && genlock_adjust > -max_steps) {
                                threshold = -thresholds[-genlock_adjust];
                            }
                            if (genlock_adjust > -max_steps && difference < threshold) {
                                new_genlock_adjust--;
                            }
                            if (genlock_adjust < -1 && difference >= -thresholds[-(genlock_adjust + 1)]) {
                                new_genlock_adjust++;
                            }
                        }
                    }
                    if (new_genlock_adjust != genlock_adjust || last_vlock != HDMI_EXACT) {
                        recalculate_hdmi_clock(HDMI_EXACT, new_genlock_adjust);
                        last_vlock = HDMI_EXACT;
                        genlock_adjust = new_genlock_adjust;
                        framecount = frame_delay;
                        //log_debug("%4d,%4d,%4d,%4d,%4d,%4d", genlocked, vlockline, vsync_line, difference, thresholds[abs(genlock_adjust)], genlock_adjust);
                    }
                }
            }
        }
    }
    if (framecount != 0) {
      framecount --;
    }
    if (vlockmode != HDMI_EXACT) {
      // Return 0 if genlock disabled
      return 0;
    } else {
      // Return 1 if genlock enabled but not yet locked
      // Return 2 if genlock enabled and locked
      return 1 + genlocked;
    }
}

// Configure PLLA so we can use it as a sampling clock source
//
// The logic to configure PLLA conmes from the Linux Kernel clk-bcm2835
// driver, specifically the following functions:
// - bcm2835_pll_divider_off
// - bcm2835_pll_divider_set_rate
// - bcm2835_pll_divider_on
// https://elixir.bootlin.com/linux/v4.4.70/source/drivers/clk/bcm/clk-bcm2835.c
#ifdef USE_PLLA
static void configure_plla(int divider) {

   // Log the before register values
   log_plla();

   // Disable PLLA_PER divider
   *CM_PLLA           = CM_PASSWORD | (((*CM_PLLA) & ~CM_PLLA_LOADPER) | CM_PLLA_HOLDPER);
   gpioreg[PLLA_PER]  = CM_PASSWORD | (A2W_PLL_CHANNEL_DISABLE);

   // Disable PLLA_CORE divider (to check it's not being used!)
   *CM_PLLA           = CM_PASSWORD | (((*CM_PLLA) & ~CM_PLLA_LOADCORE) | CM_PLLA_HOLDCORE);
   gpioreg[PLLA_CORE] = CM_PASSWORD | (A2W_PLL_CHANNEL_DISABLE);

   // Set the PLLA_PER divider to the value passed in
   gpioreg[PLLA_PER]  = CM_PASSWORD | (divider);
   *CM_PLLA           = CM_PASSWORD | ((*CM_PLLA) |  CM_PLLA_LOADPER);
   *CM_PLLA           = CM_PASSWORD | ((*CM_PLLA) & ~CM_PLLA_LOADPER);

   // Enable PLLA PER divider
   gpioreg[PLLA_PER]  = CM_PASSWORD | (gpioreg[PLLA_PER] & ~A2W_PLL_CHANNEL_DISABLE);
   *CM_PLLA           = CM_PASSWORD | (*CM_PLLA & ~CM_PLLA_HOLDPER);

   // Log the before register values
   log_plla();
}
#endif

static void init_hardware() {
   int i;
   for (i = 0; i < 12; i++) {
      RPI_SetGpioPinFunction(PIXEL_BASE + i, FS_INPUT);
   }
   RPI_SetGpioPinFunction(PSYNC_PIN,    FS_INPUT);
   RPI_SetGpioPinFunction(CSYNC_PIN,    FS_INPUT);
   RPI_SetGpioPinFunction(SW1_PIN,      FS_INPUT);
   RPI_SetGpioPinFunction(SW2_PIN,      FS_INPUT);
   RPI_SetGpioPinFunction(SW3_PIN,      FS_INPUT);
   RPI_SetGpioPinFunction(STROBE_PIN,   FS_OUTPUT);

   RPI_SetGpioPinFunction(VERSION_PIN,  FS_OUTPUT);
   RPI_SetGpioPinFunction(MODE7_PIN,    FS_OUTPUT);
   RPI_SetGpioPinFunction(MUX_PIN,      FS_OUTPUT);
   RPI_SetGpioPinFunction(SP_CLK_PIN,   FS_OUTPUT);
   RPI_SetGpioPinFunction(SP_DATA_PIN,  FS_OUTPUT);
   RPI_SetGpioPinFunction(SP_CLKEN_PIN, FS_OUTPUT);
   RPI_SetGpioPinFunction(LED1_PIN,     FS_OUTPUT);

   RPI_SetGpioValue(VERSION_PIN,        1);
   RPI_SetGpioValue(MODE7_PIN,          1);
   RPI_SetGpioValue(MUX_PIN,            0);
   RPI_SetGpioValue(SP_CLK_PIN,         1);
   RPI_SetGpioValue(SP_DATA_PIN,        0);
   RPI_SetGpioValue(SP_CLKEN_PIN,       0);
   RPI_SetGpioValue(LED1_PIN,           0); // active high

   // This line enables IRQ interrupts
   // Enable smi_int which is IRQ 48
   // https://github.com/raspberrypi/firmware/issues/67
   RPI_GetIrqController()->Enable_IRQs_2 = (1 << VSYNCINT);

   // Initialize hardware cycle counter
   _init_cycle_counter();

   // Configure the GPCLK pin as a GPCLK
   RPI_SetGpioPinFunction(GPCLK_PIN, FS_ALT5);

   log_info("Using %s as the sampling clock", PLL_NAME);

   // Log all the PLL values
   log_plla();
   log_pllb();
   log_pllc();
   log_plld();
   log_pllh();

#ifdef USE_PLLA
   // Enable the PLLA_PER divider
   configure_plla(4);
#endif

   // The divisor us now the same for both modes
   log_debug("Setting up divisor");
   init_gpclk(GPCLK_SOURCE, DEFAULT_GPCLK_DIVISOR);
   log_debug("Done setting up divisor");

   // Initialize the cpld after the gpclk generator has been started
   cpld_init();

   // Initialize the On-Screen Display
   osd_init();

   // Initialise the info system with cached values (as we break the GPU property interface)
   init_info();

#ifdef DEBUG
   dump_useful_info();
#endif
}

static void cpld_init() {
   // Assert the active low version pin
   RPI_SetGpioValue(VERSION_PIN, 0);
   // The CPLD now outputs a identifier and version number on the 12-bit pixel quad bus
   cpld_version_id = 0;
   for (int i = PIXEL_BASE + 11; i >= PIXEL_BASE; i--) {
      cpld_version_id <<= 1;
      cpld_version_id |= RPI_GetGpioValue(i) & 1;
   }
   // Release the active low version pin
   RPI_SetGpioValue(VERSION_PIN, 1);

   // Set the appropriate cpld "driver" based on the version
   if ((cpld_version_id >> VERSION_DESIGN_BIT) == DESIGN_BBC) {
      cpld = &cpld_bbc;
   } else if ((cpld_version_id >> VERSION_DESIGN_BIT) == DESIGN_ATOM) {
      cpld = &cpld_atom;
   } else if ((cpld_version_id >> VERSION_DESIGN_BIT) == DESIGN_YUV) {
      cpld = &cpld_yuv;
   } else if ((cpld_version_id >> VERSION_DESIGN_BIT) == DESIGN_RGB_TTL) {
      cpld = &cpld_rgb_ttl;
   } else if ((cpld_version_id >> VERSION_DESIGN_BIT) == DESIGN_RGB_ANALOG) {
      cpld = &cpld_rgb_analog;
   } else {
      log_info("Unknown CPLD: identifier = %03x", cpld_version_id);
      cpld = &cpld_null;
   }

   int keycount = key_press_reset();
   log_info("Keycount = %d", keycount);
   if (keycount == 7) {
        cpld = &cpld_null;
        cpld_version_id = 0xfff;
   }


   log_info("CPLD  Design: %s", cpld->name);
   log_info("CPLD Version: %x.%x", (cpld_version_id >> VERSION_MAJOR_BIT) & 0x0f, (cpld_version_id >> VERSION_MINOR_BIT) & 0x0f);

   // Initialize the CPLD's default sampling points
   cpld->init(cpld_version_id);
   // Initialize the geometry
   geometry_init(cpld_version_id);
}

static int extra_flags() {
   int extra = 0;
   if (cpld->old_firmware_support()) {
        extra |= BIT_OLD_FIRMWARE_SUPPORT;
   }
   if (autoswitch != AUTOSWITCH_MODE7) {
        extra |= BIT_NO_H_SCROLL;
   }
   if (autoswitch != AUTOSWITCH_PC) {
        extra |= BIT_NO_AUTOSWITCH;
   }
   if (!scanlines || ((capinfo->sizex2 & 1) == 0) || mode7 || osd_active()) {
        extra |= BIT_NO_SCANLINES;
   }
   if (osd_active()) {
        extra |= BIT_OSD;
   }
return extra;
}

static int test_for_elk(capture_info_t *capinfo, int elk, int mode7) {

   // If mode 7, then assume the Beeb
   if (mode7) {
      // Leave the setting unchanged
      return elk;
   }

   unsigned int ret;
   unsigned int flags = extra_flags() | BIT_CALIBRATE | (2 << OFFSET_NBUFFERS);

   // Set to capture exactly one field
   capinfo->ncapture = 1;

   // Grab one field
   ret = rgb_to_fb(capinfo, flags);
   unsigned char *fb1 = capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch;

   // Grab second field
   ret = rgb_to_fb(capinfo, flags);
   unsigned char *fb2 = capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch;

   if (fb1 == fb2) {
      log_warn("test_for_elk() failed, both buffers the same!");
      // Leave the setting unchanged
      return elk;
   }

   unsigned int min_diff = INT_MAX;
   unsigned int min_offset = 0;

   for (int offset = -2; offset <= 2; offset += 2) {

      uint32_t *p1 = (uint32_t *)(fb1 + 2 * capinfo->pitch);
      uint32_t *p2 = (uint32_t *)(fb2 + 2 * capinfo->pitch + offset * capinfo->pitch);
      unsigned int diff = 0;
      for (int i = 0; i < (capinfo->height - 4) * capinfo->pitch; i += 4) {
         uint32_t d = (*p1++) ^ (*p2++);
         while (d) {
            if (d & 0x0F) {
               diff++;
            }
            d >>= 4;
         }
      }
      if (diff < min_diff) {
         min_diff = diff;
         min_offset = offset;
      }
      log_debug("offset = %d, diff = %u", offset, diff);

   }
   log_debug("min offset = %d", min_offset);
   return min_offset != 0;
}

#ifdef HAS_MULTICORE
static void start_core(int core, func_ptr func) {
   printf("starting core %d\r\n", core);
   *(unsigned int *)(0x4000008C + 0x10 * core) = (unsigned int) func;
}
#endif

// =============================================================
// Public methods
// =============================================================

int diff_N_frames(capture_info_t *capinfo, int n, int mode7, int elk) {
   int result = 0;

   // Calculate frame differences, broken out by channel and by sample point (A..F)
   int *by_offset = diff_N_frames_by_sample(capinfo, n, mode7, elk);

   // Collapse the offset dimension
   for (int i = 0; i < NUM_OFFSETS; i++) {
      result += by_offset[i];
   }
   return result;
}

int *diff_N_frames_by_sample(capture_info_t *capinfo, int n, int mode7, int elk) {

   unsigned int ret;

   // NUM_OFFSETS is 6 (Sample Offset A..Sample Offset F)
   static int  sum[NUM_OFFSETS];
   static int  min[NUM_OFFSETS];
   static int  max[NUM_OFFSETS];
   static int diff[NUM_OFFSETS];

   for (int i = 0; i < NUM_OFFSETS; i++) {
      sum[i] = 0;
      min[i] = INT_MAX;
      max[i] = INT_MIN;
   }

#ifdef INSTRUMENT_CAL
   unsigned int t;
   unsigned int t_capture = 0;
   unsigned int t_memcpy = 0;
   unsigned int t_compare = 0;
#endif

   unsigned int flags = extra_flags() | mode7 | BIT_CALIBRATE | ((elk & (!mode7)) ? BIT_ELK : 0) | (2 << OFFSET_NBUFFERS);

   uint32_t bpp      = capinfo->bpp;
   uint32_t pix_mask = (bpp == 8) ? 0x0000007F : 0x00000007;
   uint32_t osd_mask = (bpp == 8) ? 0x7F7F7F7F : 0x77777777;

   geometry_get_fb_params(capinfo);            // required as calibration sets delay to 0 and the 2 high bits of that adjust the h offset
   // In mode 0..6, capture one field
   // In mode 7,    capture two fields
   capinfo->ncapture = mode7 ? 2 : 1;

#ifdef INSTRUMENT_CAL
   t = _get_cycle_counter();
#endif
   // Grab an initial frame
   ret = rgb_to_fb(capinfo, flags);
#ifdef INSTRUMENT_CAL
   t_capture += _get_cycle_counter() - t;
#endif

   for (int i = 0; i < n; i++) {

      for (int j = 0; j < NUM_OFFSETS; j++) {
         diff[j] = 0;
      }

#ifdef INSTRUMENT_CAL
      t = _get_cycle_counter();
#endif
      // Save the last frame
      memcpy((void *)last, (void *)(capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch), capinfo->height * capinfo->pitch);
#ifdef INSTRUMENT_CAL
      t_memcpy += _get_cycle_counter() - t;
      t = _get_cycle_counter();
#endif
      // Grab the next frame
      ret = rgb_to_fb(capinfo, flags);
#ifdef INSTRUMENT_CAL
      t_capture += _get_cycle_counter() - t;
      t = _get_cycle_counter();
#endif
      // Compare the frames
      uint32_t *fbp = (uint32_t *)(capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch + capinfo->v_adjust * capinfo->pitch);
      uint32_t *lastp = (uint32_t *)last + capinfo->v_adjust * (capinfo->pitch >> 2);
      for (int y = 0; y < (capinfo->nlines << (capinfo->sizex2 & 1)); y++) {
         int skip = 0;
         // Calculate the capture scan line number (allowing for a double hight framebuffer)
         // (capinfo->height is the framebuffer height after any doubling)
         int line = (capinfo->sizex2 & 1) ? (y >> 1) : y;
         // As v_offset increases, e.g. by one, the screen image moves up one capture line
         // (the hardcoded constant of 21 relates to the BBC video format)
         line += (capinfo->v_offset - 21);
         // Skip lines that might contain flashing cursor
         // (the cursor rows were determined empirically)
         if (line >= 0) {
            if (elk) {
               // Eliminate cursor lines in 32 row modes (0,1,2,4,5)
               if (!mode7 && (line % 8) == 5) {
                  skip = 1;
               }
               // Eliminate cursor lines in 25 row modes (3, 6)
               if (!mode7 && (line % 10) == 3) {
                  skip = 1;
               }
               // Eliminate cursor lines in mode 7
               // (this case is untested as I don't have a Jafa board)
               if (mode7 && (line % 10) == 7) {
                  skip = 1;
               }
            } else {
               // Eliminate cursor lines in 32 row modes (0,1,2,4,5)
               if (!mode7 && (line % 8) == 7) {
                  skip = 1;
               }
               // Eliminate cursor lines in 25 row modes (3, 6)
               if (!mode7 && (line % 10) >= 5 && (line % 10) <= 7) {
                  skip = 1;
               }
               // Eliminate cursor lines in mode 7
               if (mode7 && (line % 10) == 7) {
                  skip = 1;
               }
            }
         }
         if (skip) {
            // For debugging it's useful to see if the lines being eliminated align with the cursor
            // for (int x = 0; x < capinfo->pitch; x += 4) {
            //    *fbp++ = 0x11111111;
            // }
            fbp   += capinfo->pitch >> 2;
            lastp += capinfo->pitch >> 2;
         } else {
            for (int x = 0; x < capinfo->pitch; x += 4) {
               uint32_t d = (*fbp++) ^ (*lastp++);
               // Mask out OSD
               d &= osd_mask;
               // Work out the starting index
               int index = (x << 1) % 6;
               while (d) {
                  if (d & pix_mask) {
                     diff[index]++;
                  }
                  d >>= bpp;
                  index = (index + 1) % NUM_OFFSETS;
               }
            }
         }
      }
#ifdef INSTRUMENT_CAL
      t_compare += _get_cycle_counter() - t;
#endif
      // At this point the diffs correspond to the sample points in
      // an unusual order: A F C B E D
      //
      // This happens for three reasons:
      // - the CPLD starts with sample point B, so you get B C D E F A
      // - the firmware skips the first quad, so you get F A B C D E
      // - the frame buffer swaps odd and even pixels, so you get A F C B E D
      //
      // Mutate the result to correctly order the sample points:
      // A F C B E D => A B C D E F
      //
      // Then the downstream algorithms don't have to worry
      int f = diff[1];
      int b = diff[3];
      int d = diff[5];
      diff[1] = b;
      diff[3] = d;
      diff[5] = f;

      // Accumulate the result
      for (int j = 0; j < NUM_OFFSETS; j++) {
         sum[j] += diff[j];
         if (diff[j] < min[j]) {
            min[j] = diff[j];
         }
         if (diff[j] > max[j]) {
            max[j] = diff[j];
         }
      }
   }

#if 0
   for (int i = 0; i < NUM_OFFSETS; i++) {
      log_debug("offset %d diff:  sum = %d min = %d, max = %d", i, sum[i], min[i], max[i]);
   }
#endif

#ifdef INSTRUMENT_CAL
   log_debug("t_capture total = %d, mean = %d ", t_capture, t_capture / (n + 1));
   log_debug("t_compare total = %d, mean = %d ", t_compare, t_compare / n);
   log_debug("t_memcpy  total = %d, mean = %d ", t_memcpy,  t_memcpy / n);
   log_debug("total = %d", t_capture + t_compare + t_memcpy);
#endif
   return sum;
}

#define MODE7_CHAR_WIDTH 12

signed int analyze_mode7_alignment(capture_info_t *capinfo) {
    if (autoswitch != AUTOSWITCH_MODE7) {
        return -1;
    }

   // mode 7 character is 12 pixels wide
   int counts[MODE7_CHAR_WIDTH];
   // bit offset pixels 0..7
   int px_offset_map[] = {4, 0, 12, 8, 20, 16, 28, 24};

   unsigned int flags = extra_flags() | BIT_MODE7 | BIT_CALIBRATE | (2 << OFFSET_NBUFFERS);

   // Capture two fields
   capinfo->ncapture = 2;

   // Grab a frame
   int ret = rgb_to_fb(capinfo, flags);

   // Work out the base address of the frame buffer that was used
   uint32_t *fbp = (uint32_t *)(capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch + capinfo->v_adjust * capinfo->pitch + capinfo->h_adjust);

   // Initialize the counters
   for (int i = 0; i < MODE7_CHAR_WIDTH; i++) {
      counts[i] = 0;
   }

   // Count the pixels
   uint32_t *fbp_line;

   for (int line = 0; line < capinfo->nlines << (capinfo->sizex2 & 1); line++) {
      int index = 0;
      fbp_line = fbp;
      for (int byte = 0; byte < (capinfo->chars_per_line << 2); byte += 4) {
         uint32_t word = *fbp_line++;
         int *offset = px_offset_map;
         for (int i = 0; i < 8; i++) {
            int px = (word >> (*offset++)) & 7;
            if (px) {
               counts[index]++;
            }
            index = (index + 1) % MODE7_CHAR_WIDTH;
         }
      }
      fbp += capinfo->pitch >> 2;
   }

   // Log the raw counters
   for (int i = 0; i < MODE7_CHAR_WIDTH; i++) {
      log_info("counter %2d = %d", i, counts[i]);
   }

   // A typical distribution looks like
   // INFO: counter  0 = 647
   // INFO: counter  1 = 573
   // INFO: counter  2 = 871
   // INFO: counter  3 = 878
   // INFO: counter  4 = 572
   // INFO: counter  5 = 653
   // INFO: counter  6 = 869
   // INFO: counter  7 = 742
   // INFO: counter  8 = 2
   // INFO: counter  9 = 2
   // INFO: counter 10 = 906
   // INFO: counter 11 = 1019

   // There should be a two pixel minima
   int min_count = INT_MAX;
   int min_i = -1;
   for (int i = 0; i < MODE7_CHAR_WIDTH; i++) {
      int c = counts[i] + counts[(i + 1) % MODE7_CHAR_WIDTH];
      if (c < min_count) {
         min_count = c;
         min_i = i;
      }
   }
   log_info("minima at index: %d", min_i);

   // That minima should occur in pixels 0 and 1, so compute a delay to make this so
   return (MODE7_CHAR_WIDTH - min_i);
}

#define DEFAULT_CHAR_WIDTH 8

signed int analyze_default_alignment(capture_info_t *capinfo) {

    if (autoswitch != AUTOSWITCH_MODE7) {
        return -1;
    }
   // mode 0 character is 8 pixels wide
   int counts[DEFAULT_CHAR_WIDTH];
   // bit offset pixels 0..7
   int px_offset_map[] = {4, 0, 12, 8, 20, 16, 28, 24};

   unsigned int flags = extra_flags() | BIT_CALIBRATE | (2 << OFFSET_NBUFFERS);

   // Capture two fields
   capinfo->ncapture = 1;

   // Grab a frame
   int ret = rgb_to_fb(capinfo, flags);

   // Work out the base address of the frame buffer that was used
   uint32_t *fbp = (uint32_t *)(capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch + capinfo->v_adjust * capinfo->pitch + capinfo->h_adjust);

   // Initialize the counters
   for (int i = 0; i < DEFAULT_CHAR_WIDTH; i++) {
      counts[i] = 0;
   }

   // Count the pixels
   uint32_t *fbp_line;


   if (capinfo->bpp == 4)
   {

    for (int line = 0; line <  capinfo->nlines << (capinfo->sizex2 & 1); line++) {
      int index = 0;
      fbp_line = fbp;
      for (int byte = 0; byte < (capinfo->chars_per_line << 2); byte += 4) {
         uint32_t word = *fbp_line++;
         int *offset = px_offset_map;
         for (int i = 0; i < 8; i++) {
            int px = (word >> (*offset++)) & 7;
            if (px) {
               counts[index]++;
            }
            index = (index + 1) % DEFAULT_CHAR_WIDTH;
         }
      }
      fbp += capinfo->pitch >> 2;
    }

   } else {
    for (int line = 0; line <  capinfo->nlines << (capinfo->sizex2 & 1); line++) {
      int index = 0;
      fbp_line = fbp;
      for (int byte = 0; byte < (capinfo->chars_per_line << 2); byte += 4) {
         uint32_t word = *fbp_line++;
         for (int i = 0; i < 4; i++) {
            int px = (word >> (i*8)) & 0x7f;
            if (px) {
               counts[index]++;
            }
            index = (index + 1) % DEFAULT_CHAR_WIDTH;
         }
         word = *fbp_line++;
         for (int i = 0; i < 4; i++) {
            int px = (word >> (i*8)) & 0x7f;
            if (px) {
               counts[index]++;
            }
            index = (index + 1) % DEFAULT_CHAR_WIDTH;
         }
      }
      fbp += capinfo->pitch >> 2;
    }
   }
   // Log the raw counters
   for (int i = 0; i < DEFAULT_CHAR_WIDTH; i++) {
      log_info("counter %2d = %d", i, counts[i]);
   }

   // A typical distribution looks like
   // INFO: counter  0 = 878
   // INFO: counter  1 = 740
   // INFO: counter  2 = 212
   // INFO: counter  3 = 2
   // INFO: counter  4 = 1036
   // INFO: counter  5 = 1224
   // INFO: counter  6 = 648
   // INFO: counter  7 = 706

   // There should be a one pixel minima
   int min_count = INT_MAX;
   int min_i = -1;
   for (int i = 0; i < DEFAULT_CHAR_WIDTH; i++) {
      int c = counts[i];
      if (c < min_count) {
         min_count = c;
         min_i = i;
      }
   }
   log_info("minima at index: %d", min_i);

   // That minima should occur in pixels 0 and 1, so compute a delay to make this so
   return DEFAULT_CHAR_WIDTH - min_i;
}

#if 0
int total_N_frames(capture_info_t *capinfo, int n, int mode7, int elk) {

   int sum = 0;
   int min = INT_MAX;
   int max = INT_MIN;

#ifdef INSTRUMENT_CAL
   unsigned int t;
   unsigned int t_capture = 0;
   unsigned int t_compare = 0;
#endif

   unsigned int flags = extra_flags() | mode7 | BIT_CALIBRATE | ((elk & !mode7) ? BIT_ELK : 0) | (2 << OFFSET_NBUFFERS);

   // In mode 0..6, capture one field
   // In mode 7,    capture two fields
   capinfo->ncapture = mode7 ? 2 : 1;

   for (int i = 0; i < n; i++) {
      int total = 0;

      // Grab the next frame
      ret = rgb_to_fb(capinfo, flags);
#ifdef INSTRUMENT_CAL
      t_capture += _get_cycle_counter() - t;
      t = _get_cycle_counter();
#endif
      // Compare the frames
      uint32_t *fbp = (uint32_t *)(capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch);
      for (int j = 0; j < capinfo->height * capinfo->pitch; j += 4) {
         uint32_t f = *fbp++;
         // Mask out OSD
         f &= 0x77777777;
         while (f) {
            if (f & 0x0F) {
               total++;
            }
            f >>= 4;
         }
      }
#ifdef INSTRUMENT_CAL
      t_compare += _get_cycle_counter() - t;
#endif

      // Accumulate the result
      sum += total;
      if (total < min) {
         min = total;
      }
      if (total > max) {
         max = total;
      }
   }

   int mean = sum / n;
   log_debug("total: sum = %d mean = %d, min = %d, max = %d", sum, mean, min, max);
#ifdef INSTRUMENT_CAL
   log_debug("t_capture total = %d, mean = %d ", t_capture, t_capture / (n + 1));
   log_debug("t_compare total = %d, mean = %d ", t_compare, t_compare / n);
   log_debug("total = %d", t_capture + t_compare + t_memcpy);
#endif
   return sum;
}
#endif

#ifdef MULTI_BUFFER
void swapBuffer(int buffer) {
   RPI_PropertyInit();
   current_display_buffer = buffer;
   RPI_PropertyAddTag(TAG_SET_VIRTUAL_OFFSET, 0, capinfo->height * buffer);
   // Use version that doesn't wait for the response
   RPI_PropertyProcessNoCheck();
}
#endif

int get_current_display_buffer() {
   if (mode7) {
       return 0;
   } else {
       return current_display_buffer;
   }
}

void set_profile(int val) {
   log_info("Setting profile to %d", val);
   profile = val;
}

int get_profile() {
   return profile;
}

void set_subprofile(int val) {
   log_info("Setting subprofile to %d", val);
   subprofile = val;
}

int get_subprofile() {
   return subprofile;
}
void set_paletteControl(int value) {
   paletteControl = value;
}

int get_paletteControl() {
   return paletteControl;
}

void set_resolution(int mode, const char *name, int reboot) {
   //char osdline[80];
   if (resolution != mode) {
    //  if (osd_active()) {
    //     sprintf(osdline, "New setting requires reboot on menu exit");
    //     osd_set(1, 0, osdline);
    //  }
   reboot_required = reboot;
   resolution = mode;
   strcpy(resolution_name, name);
   resolution_warning = 1;
   }
}

int get_resolution() {
   return resolution;
}

void set_scaling(int mode, int reboot) {
   //char osdline[80];
   if (scaling != mode) {
     //  if (osd_active()) {
     //       osd_set(1, 0, "New setting requires reboot on menu exit");
     //  }
       reboot_required = reboot;
       scaling = mode;

       int gscaling = SCALING_INTEGER;

       switch (mode) {
           case SCALING_FILL43_MEDIUM:
           case SCALING_FILL43_SOFT:
           gscaling = SCALING_MANUAL43;
           break;

           case SCALING_FILLALL_MEDIUM:
           case SCALING_FILLALL_SOFT:
           gscaling = SCALING_MANUAL;
           break;
       }

       set_gscaling(gscaling);

   }
}

int get_scaling() {
   return scaling;
}

void set_frontend(int value, int save) {
   int min = cpld->frontend_info() & 0xffff;
   int max = cpld->frontend_info() >> 16;
   if (value >= min && value <= max) {
       frontend = value;
   } else {
       if (value == 0 || value > max) {
           frontend = min;
       } else {
           frontend = max;
       }
   }
   if (save != 0) {
       file_save_config(resolution_name, scaling, frontend);
   }
   cpld->set_frontend(frontend);
}

int get_frontend() {
       return frontend;
}

void set_deinterlace(int mode) {
   deinterlace = mode;
}

int get_deinterlace() {
   return deinterlace;
}

void set_scanlines(int on) {
   scanlines = on;
   clear = BIT_CLEAR;
}

int get_scanlines() {
   return scanlines;
}

void set_scanlines_intensity(int value) {
   scanlines_intensity =value;
}

int get_scanlines_intensity() {
   return scanlines_intensity;
}

void set_colour(int val) {
   colour = val;
}

int get_colour() {
   return colour;
}

void set_invert(int value) {
   invert =value;
}

int get_invert() {
   return invert;
}

void set_fontsize(int value) {
   fontsize=value;
}

int get_fontsize() {
   return fontsize;
}

void set_border(int value) {
   border = value;
   clear = BIT_CLEAR;
}

int  get_border() {
   return border;
}

void set_elk(int on) {
   elk = on;
   clear = BIT_CLEAR;
}

int get_elk() {
   return elk;
}

void set_vsync(int on) {
   vsync = on;
}

int get_vsync() {
   return vsync;
}

void set_vlockmode(int val) {
   vlockmode = val;
   recalculate_hdmi_clock_line_locked_update(GENLOCK_FORCE);
}

int get_vlockmode() {
   return vlockmode;
}

void set_vlockline(int val) {
   vlockline = val;
   recalculate_hdmi_clock_line_locked_update(GENLOCK_FORCE);
}

int get_vlockline() {
   return vlockline;
}

void set_vlockadj(int val) {
   vlockadj = val;
   recalculate_hdmi_clock_line_locked_update(GENLOCK_FORCE);
}

int get_vlockadj() {
   return vlockadj;
}

void set_vlockspeed(int val) {
   vlockspeed = val;
}

int get_vlockspeed() {
   return vlockspeed;
}

#ifdef MULTI_BUFFER
int get_nbuffers() {
   return nbuffers;
}

void set_nbuffers(int val) {
   nbuffers=val;
}
#endif

void set_debug(int on) {
   debug = on;
}

int get_debug() {
   return debug;
}

void set_autoswitch(int value) {
   // Prevent autoswitch (to mode 7) being accidentally with the Atom CPLD,
   // for example by selecting the BBC_Micro profile, as this results in
   // an unusable OSD which persists even after cycling power.
   //
   // Atom timing looks like Mode 7, but as we don't have 6bpp mode 7
   // line capture code, we end up using the default line capture code,
   // which immediately overwrites the OSD with capture data. But because the
   // mode7 flag is set, the OSD is not then repainted in the blanking interval.
   // The end result is the OSD is briefly appears when a button is pressed,
   // then vanishes, making it very tricky to navigate.
   //
   // It might be better to combine this with the cpld->old_firmware() and
   // rename this to cpld->get_capabilities().
   int cpld_ver = (cpld->get_version() >> VERSION_DESIGN_BIT) & 0x0F;
   if (value == AUTOSWITCH_MODE7 && (cpld_ver == DESIGN_ATOM || cpld_ver == DESIGN_YUV)) {
      autoswitch ^= AUTOSWITCH_PC;
   } else {
      autoswitch = value;
   }
   hsync_width = (autoswitch == AUTOSWITCH_MODE7) ? 6144 : 8192;
}

int get_autoswitch() {
   return autoswitch;
}

void action_calibrate_clocks() {
   // re-measure vsync and set the core/sampling clocks
   calibrate_sampling_clock();
   // set the hdmi clock property to match exactly
   set_vlockmode(HDMI_EXACT);
}

void action_calibrate_auto() {
   // re-measure vsync and set the core/sampling clocks
   calibrate_sampling_clock();
   // During calibration we do our best to auto-delect an Electron
   elk = test_for_elk(capinfo, elk, mode7);
   log_debug("Elk mode = %d", elk);
   for (int c = 0; c < NUM_CAL_PASSES; c++) {
      cpld->calibrate(capinfo, elk);
   }
}

int is_genlocked() {
   return genlocked;
}

void calculate_fb_adjustment() {
   int double_height = capinfo->sizex2 & 1;
   capinfo->v_adjust  = (capinfo->height >> double_height)  - capinfo->nlines;
   if (capinfo->v_adjust < 0) {
       capinfo->v_adjust = 0;
   }
   capinfo->v_adjust >>= (double_height ^ 1);

   capinfo->h_adjust = (capinfo->width >> 3) - capinfo->chars_per_line;
   if (capinfo->h_adjust < 0) {
       capinfo->h_adjust = 0;
   }

   capinfo->h_adjust = (capinfo->h_adjust >> 1) << (capinfo->bpp == 8 ? 3 : 2);
   //log_info("adjust=%d, %d", capinfo->h_adjust, capinfo->v_adjust);
}

void setup_profile() {

    // Switch the the approriate capinfo structure instance
    capinfo = mode7 ? &mode7_capinfo : &default_capinfo;

    log_debug("Setting mode7 = %d", mode7);

    geometry_set_mode(mode7);
    capinfo->palette_control = paletteControl;

    log_debug("Loading sample points");
    cpld->set_mode(mode7);
    log_debug("Done loading sample points");

    geometry_get_fb_params(capinfo);

    if (autoswitch != AUTOSWITCH_PC) {
        capinfo->detected_sync_type = cpld->analyse(capinfo->sync_type);
        log_info("Polarity state set from profile = %s (%s)", sync_names[capinfo->detected_sync_type & SYNC_BIT_MASK], mixed_names[(capinfo->detected_sync_type & SYNC_BIT_MIXED_SYNC) ? 1 : 0]);
    } else {
        capinfo->detected_sync_type = cpld->analyse(-1);
        log_info("Detected polarity state = %s (%s)", sync_names[capinfo->detected_sync_type & SYNC_BIT_MASK], mixed_names[(capinfo->detected_sync_type & SYNC_BIT_MIXED_SYNC) ? 1 : 0]);
    }

    cpld->update_capture_info(capinfo);
    calculate_fb_adjustment();

    rgb_to_fb(capinfo, extra_flags() | BIT_PROBE); // dummy mode7 probe to setup sync type from capinfo

   // Measure the frame time and set the sampling clock
    calibrate_sampling_clock();

    // force recalculation of the HDMI clock (if the vlockmode property requires this)
    recalculate_hdmi_clock_line_locked_update(GENLOCK_FORCE);

    double line_time = (double)  clkinfo.line_len * 1000000000 / clkinfo.clock;
    double window = (double) clkinfo.clock_ppm * line_time / 1000000;
    hsync_comparison_lo = (int) line_time - window;
    hsync_comparison_hi = (int) line_time + window;
    vsync_comparison_lo = hsync_comparison_lo * clkinfo.lines_per_frame;
    vsync_comparison_hi = hsync_comparison_hi * clkinfo.lines_per_frame;

    log_info("Window: H = %d to %d, V = %d to %d, S = %s", hsync_comparison_lo, hsync_comparison_hi, vsync_comparison_lo, vsync_comparison_hi, sync_names[capinfo->sync_type]);
}
void set_status_message(char *msg) {
    strcpy(status, msg);
    if (osd_active() && status[0]==0) {
        osd_set(1, 0, "");
    }
}

void rgb_to_hdmi_main() {
   int result = RET_SYNC_TIMING_CHANGED;   // make sure autoswitch works first time
   int last_mode7;
   int last_paletteControl = paletteControl;
   int mode_changed;
   int fb_size_changed;
   int active_size_decreased;
   int clk_changed = 0;
   int ncapture;
   int last_profile = -1;
   int last_subprofile = -1;
   int forcereprogram = 0;
   char osdline[80];
   capture_info_t last_capinfo;
   clk_info_t last_clkinfo;


   // Setup defaults (these may be overridden by the CPLD)
   default_capinfo.capture_line = capture_line_normal_3bpp_table;
   mode7_capinfo.capture_line   = capture_line_mode7_3bpp_table;
   capinfo = &default_capinfo;
   capinfo->v_adjust = 0;
   capinfo->h_adjust = 0;
   capinfo->border = 0;
   cpld->set_mode(0);
   current_display_buffer = 0;
   // Determine initial sync polarity (and correct whether inversion required or not)
   capinfo->detected_sync_type = cpld->analyse(-1);
   log_info("Detected polarity state at startup = %s (%s)", sync_names[capinfo->detected_sync_type & SYNC_BIT_MASK], mixed_names[(capinfo->detected_sync_type & SYNC_BIT_MIXED_SYNC) ? 1 : 0]);
   // Determine initial mode
   mode7 = rgb_to_fb(capinfo, extra_flags() | BIT_PROBE) & BIT_MODE7 & (autoswitch == AUTOSWITCH_MODE7);
   // Default to capturing indefinitely
   ncapture = -1;
   int keycount = key_press_reset();
   log_info("Keycount = %d", keycount);
   switch(keycount) {
       case 7 :
       {
            log_info("Entering CPLD reprogram mode");
            forcereprogram = 1;
            do {} while (key_press_reset() != 0);
       }
       break;
       case 1 :
       {
            if ((strcmp(resolution_name, "Default@60Hz") != 0 || scaling != 0)) {
                log_info("Resetting output resolution to Default@60Hz");
                int a = 13;
                file_save_config("Default@60Hz", 0, frontend);
                // Wait a while to allow UART time to empty
                for (delay = 0; delay < 100000; delay++) {
                   a = a * 13;
                }
            reboot();
            }
            else {
                do {} while (key_press_reset() != 0);
            }

       }
       break;

       default:
       break;

   }

   resolution_warning = 0;
   clear = BIT_CLEAR;
   while (1) {
      log_info("-----------------------LOOP------------------------");

      setup_profile();

      if ((autoswitch == AUTOSWITCH_PC) && ((result & RET_SYNC_TIMING_CHANGED) || profile != last_profile || last_subprofile != subprofile)) {
         int new_sub_profile = autoswitch_detect(one_line_time_ns, lines_per_frame, capinfo->detected_sync_type & SYNC_BIT_MASK);
         if (new_sub_profile >= 0) {
             set_subprofile(new_sub_profile);
             process_sub_profile(get_profile(), new_sub_profile);
             setup_profile();
         } else {
             log_info("Autoswitch: No profile matched");
         }
      }
      last_profile = profile;
      last_subprofile = subprofile;
      last_paletteControl = paletteControl;
      log_debug("Setting up frame buffer");
      init_framebuffer(capinfo);
      log_debug("Done setting up frame buffer");

      osd_refresh();

      // If the CPLD is unprogrammed, operate in a degraded mode that allows the menus to work
      if ((((cpld->get_version() >> VERSION_DESIGN_BIT) & 15) == DESIGN_NULL) || forcereprogram != 0) {
         while (1) {
            osd_set(1, 0, "CPLD is unprogrammed");
            int flags = 0;
            capinfo->ncapture = ncapture;
            log_info("Entering poll_keys_only, flags=%08x", flags);
            result = poll_keys_only(capinfo, flags);
            log_info("Leaving poll_keys_only, result=%04x", result);
            if (result & RET_EXPIRED) {
               ncapture = osd_key(OSD_EXPIRED);
            } else if (result & RET_SW1) {
               ncapture = osd_key(OSD_SW1);
            } else if (result & RET_SW2) {
               ncapture = osd_key(OSD_SW2);
            } else if (result & RET_SW3) {
               ncapture = osd_key(OSD_SW3);
            }
         }
      }

      if (restart_profile) {
         osd_set(1, 0, "Configuration restored");
         restart_profile = 0;
      }

     // unsigned int *i;
     // for (i=(unsigned int *)(PERIPHERAL_BASE + 0x400000); i<(unsigned int *)(PERIPHERAL_BASE + 0x4000e4); i++) {
     //    log_info(" Regs:%08x %08x = %02x",PERIPHERAL_BASE, i,  *i);
     // }

      if (border !=0) {
         clear = BIT_CLEAR;
      }
      do {

         geometry_get_fb_params(capinfo);
         capinfo->ncapture = ncapture;
         capinfo->border = border;
         calculate_fb_adjustment();
         capinfo->palette_control = paletteControl;
         // Update capture info, in case sample width has changed
         // (this also re-selects the appropriate line capture)
         cpld->update_capture_info(capinfo);

         int flags =  extra_flags() | mode7 | clear;
         if (autoswitch == AUTOSWITCH_MODE7) {
            flags |= BIT_MODE_DETECT;
         }

         if (interlaced) {
            flags |= BIT_INTERLACED;
         }
         if (vsync) {
            flags |= BIT_VSYNC;
         }
         if (elk & (!mode7)) {
            flags |= BIT_ELK;
         }
         if (debug) {
            flags |= BIT_DEBUG;
         }

         //paletteFlags |= BIT_MULTI_PALETTE;   // test multi palette

         flags |= deinterlace << OFFSET_INTERLACE;
#ifdef MULTI_BUFFER
         if (!mode7 && osd_active() && (nbuffers == 0)) {
            flags |= 2 << OFFSET_NBUFFERS;
         } else {
            flags |= nbuffers << OFFSET_NBUFFERS;
         }
#endif

         if (!osd_active() && reboot_required) {
             file_save_config(resolution_name, scaling, frontend);
             // Wait a while to allow UART time to empty
             delay_in_arm_cycles(100000000);
             if (resolution_warning != 0) {
                 osd_set(0, 0, "Hold menu during reset to recover");
                 osd_set(1, 0, "if no display at new resolution.");

                 for (int i = 5; i > 0; i--) {
                     sprintf(osdline, "Rebooting in %d secs ", i);
                     log_info(osdline);
                     osd_set(3, 0, osdline);
                     delay_in_arm_cycles(1000000000);
                  }
             }
             reboot();
         }

         log_debug("Entering rgb_to_fb, flags=%08x", flags);
         result = rgb_to_fb(capinfo, flags);
         log_debug("Leaving rgb_to_fb, result=%04x", result);

         if (result & RET_SYNC_TIMING_CHANGED) {
             log_info("Timing exceeds window: H = %d, V = %d, Lines = %d, VSync = %d", hsync_period, vsync_period, (int) (((double)vsync_period/hsync_period) + 0.5), (result & RET_VSYNC_POLARITY_CHANGED) ? 1 : 0);
         }
         clear = 0;

         // Possibly the size or offset has been adjusted, so update current capinfo
         memcpy(&last_capinfo, capinfo, sizeof last_capinfo);
         memcpy(&last_clkinfo, &clkinfo, sizeof last_clkinfo);

         if (result & RET_EXPIRED) {
            ncapture = osd_key(OSD_EXPIRED);
         } else if (result & RET_SW1) {
            ncapture = osd_key(OSD_SW1);
         } else if (result & RET_SW2) {
            ncapture = osd_key(OSD_SW2);
         } else if (result & RET_SW3) {
            ncapture = osd_key(OSD_SW3);
         }

         geometry_get_fb_params(capinfo);

         fb_size_changed = (capinfo->width != last_capinfo.width) || (capinfo->height != last_capinfo.height) || (capinfo->bpp != last_capinfo.bpp);
         active_size_decreased = (capinfo->chars_per_line < last_capinfo.chars_per_line) || (capinfo->nlines < last_capinfo.nlines);

         geometry_get_clk_params(&clkinfo);
         clk_changed = (clkinfo.clock != last_clkinfo.clock) || (clkinfo.line_len != last_clkinfo.line_len || (clkinfo.clock_ppm != last_clkinfo.clock_ppm));

         last_mode7 = mode7;

         mode7 = result & BIT_MODE7 & (autoswitch == AUTOSWITCH_MODE7);
         mode_changed = (mode7 != last_mode7) || (capinfo->px_sampling != last_capinfo.px_sampling || paletteControl != last_paletteControl || profile != last_profile || last_subprofile != subprofile || (result & RET_SYNC_TIMING_CHANGED) );

         if (active_size_decreased) {
            clear = BIT_CLEAR;
         }

         if (clk_changed || (result & RET_INTERLACE_CHANGED) || lock_fail != 0) {
            target_difference = 0;
            resync_count = 0;
            // Measure the frame time and set the sampling clock
            calibrate_sampling_clock();
            // Force recalculation of the HDMI clock (if the vlockmode property requires this)
            recalculate_hdmi_clock_line_locked_update(GENLOCK_FORCE);
         }

         if (osd_active()) {
             if (clk_changed || (clkinfo.lines_per_frame != last_clkinfo.lines_per_frame) || (capinfo->sync_type < last_capinfo.sync_type)) {
                sprintf(osdline, "%dHz %dPPM %d %s %dHz", adjusted_clock, clock_error_ppm, lines_per_frame, sync_names[capinfo->detected_sync_type & SYNC_BIT_MASK], source_vsync_freq_hz);
                osd_set(1, 0, osdline);
             } else {
                 if (status[0] != 0) {
                     osd_set(1, 0, status);
                     status[0] = 0;
                 } else {
                     if (!reboot_required) {
                         if (sync_detected) {
                             if (vlock_limited && (vlockmode != HDMI_ORIGINAL)) {
                                 sprintf(osdline, "Genlock disabled: Src=%dHz, Disp=%dHz", source_vsync_freq_hz, display_vsync_freq_hz);
                                 osd_set(1, 0, osdline);
                             }
                         } else {
                             osd_set(1, 0, "No sync detected");
                         }
                    } else {
                         osd_set(1, 0, "New setting requires reboot on menu exit");
                    }
                 }
             }
         }

      } while (!mode_changed && !fb_size_changed && !restart_profile);
      osd_clear();
      clear_full_screen();
   }
}

void force_reinit() {
    restart_profile = 1;
}

int show_detected_status(int line) {
    char message[80];
    sprintf(message, "    Clock error: %d PPM", clock_error_ppm);
    osd_set(line++, 0, message);
    sprintf(message, "   Sample clock: %d Hz", adjusted_clock);
    osd_set(line++, 0, message);
    sprintf(message, "  Line duration: %d ns", one_line_time_ns);
    osd_set(line++, 0, message);
    sprintf(message, "Lines per frame: %d", lines_per_frame);
    osd_set(line++, 0, message);
    sprintf(message, "     Frame rate: %d Hz", source_vsync_freq_hz);
    osd_set(line++, 0, message);
    sprintf(message, "      Sync type: %s", sync_names_long[capinfo->detected_sync_type & SYNC_BIT_MASK]);
    osd_set(line++, 0, message);
    return (line);
}

void kernel_main(unsigned int r0, unsigned int r1, unsigned int atags)
{
   RPI_AuxMiniUartInit(115200, 8);
   log_info("***********************RESET***********************");
   log_info("RGB to HDMI booted");

   enable_MMU_and_IDCaches();
   _enable_unaligned_access();

   init_hardware();

#ifdef HAS_MULTICORE
   int i;

   printf("main running on core %u\r\n", _get_core());

   for (i = 0; i < 10000000; i++);
   start_core(1, _spin_core);
   for (i = 0; i < 10000000; i++);
   start_core(2, _spin_core);
   for (i = 0; i < 10000000; i++);
   start_core(3, _spin_core);
   for (i = 0; i < 10000000; i++);
#endif

   rgb_to_hdmi_main();
}
