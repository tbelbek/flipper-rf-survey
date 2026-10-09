#include <furi.h>
#include <furi_hal_subghz.h>
#include <furi_hal_region.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <gui/elements.h>
#include <gui/modules/variable_item_list.h>

// CC1101 preset register tables are exported to apps; AM650 = wide OOK, good for a
// broadband RSSI survey. (Same names the stock Spectrum Analyzer / SubGHz use.)
extern const uint8_t subghz_device_cc1101_preset_ook_650khz_async_regs[];

// A permissive region covering the three CC1101 bands, installed for the app's lifetime
// (restored on exit). Receiving is legal everywhere; without this, out-of-"default-range"
// frequencies (e.g. cellular 816/836) fail to tune cleanly -> the radio stalls on a 10 ms
// settle timeout per bin (whole-sweep freeze) and can furi_crash. We never transmit.
// Layout matches FuriHalRegion (flexible array) with a fixed 3-band tail.
static struct {
    char country_code[4];
    uint16_t bands_count;
    FuriHalRegionBand bands[3];
} s_region = {
    .country_code = {'W', 'W', 0, 0},
    .bands_count = 3,
    .bands =
        {
            {299999755, 348000335, 20, 100},
            {386999938, 464000000, 20, 100},
            {778999847, 928000000, 20, 100},
        },
};

// set the CC1101 to freq and the matching RF path WITHOUT furi_hal_subghz_set_frequency_and_path,
// which furi_crash()es if the quantised real frequency lands in a band gap. Returns false
// (caller marks the bin invalid) instead of crashing.
static bool tune_rx(uint32_t f) {
    furi_hal_subghz_idle();
    uint32_t real = furi_hal_subghz_set_frequency(f);
    if(real >= 299999755 && real <= 348000335)
        furi_hal_subghz_set_path(FuriHalSubGhzPath315);
    else if(real >= 386999938 && real <= 464000000)
        furi_hal_subghz_set_path(FuriHalSubGhzPath433);
    else if(real >= 778999847 && real <= 928000000)
        furi_hal_subghz_set_path(FuriHalSubGhzPath868);
    else
        return false; // out of band after quantisation -> skip, never crash
    furi_hal_subghz_rx();
    return true;
}

// Verbose debug logging: every lifecycle step, config change, worker transition and
// (coming) logging/capture op is traced so a crash on the remote device can be pinned
// from the serial `log` without a redeploy. Stream it with the CLI `log` command.
#define TAG "RFSurvey"

#define MAX_BINS   1024 // enough for a full 300-928 MHz sweep @650k (~967 bins); int8 -> 2 KB
#define VIEW_SPEC  0
#define VIEW_CONF  1
#define SCR_W      128
#define BASE_Y     50 // spectrum baseline (room for pill buttons below)
#define MAX_H      28 // tallest bar
#define BODY_Y0    22 // top of the page body
#define HIST_ROWS  28 // waterfall history depth (1px per sweep)
#define NBARS      32 // thick grouped bars (4px pitch) -> easy cursor stepping
#define BAR_PITCH  (SCR_W / NBARS)
#define RSSI_FLOOR -100.0f
#define RSSI_CEIL  -40.0f

// Known sub-GHz allocations across the whole CC1101 range (global: US/EU/JP/CN/ITU),
// each with a typical modulation (AM = OOK, FM = 2-FSK). Used two ways: to tag a detected
// peak with its likely band, and as the selectable presets in the config screen. Ranges
// are NON-OVERLAPPING and ascending, so the same table works for both and a lookup just
// returns the one slot a frequency falls in. Names kept short for the 128px screen.
typedef enum {
    ModAM, // OOK (most remotes, garage, keyfobs, weather)
    ModFM, // 2-FSK (LoRa/Z-Wave, PMR/FRS voice, cellular, modern sensors)
} Mod;

typedef struct {
    uint32_t lo;
    uint32_t hi;
    const char* name;
    Mod mod;
} FreqBand;

static const FreqBand KNOWN_BANDS[] = {
    // 300-348 MHz (CC1101 low): US/Asia remotes, garages, car keys, TPMS -- mostly OOK
    {300000000, 306000000, "300 remote", ModAM},
    {306000000, 312000000, "310 garage", ModAM}, // US
    {312000000, 316500000, "315 remote", ModAM}, // US/JP/Asia car/garage/TPMS
    {316500000, 320000000, "318 garage", ModAM}, // US
    {320000000, 335000000, "330 car", ModAM}, // US/CN
    {335000000, 348000000, "345 remote", ModAM},
    // 387-464 MHz (CC1101 mid): 433 ISM (global), PMR/FRS, UK/US remotes, LMR
    {387000000, 400000000, "390 remote", ModAM}, // US
    {400000000, 433050000, "UHF SRD", ModAM},
    {433050000, 434790000, "433 ISM", ModAM}, // GLOBAL keyfob/TPMS/weather (433.92)
    {434790000, 438000000, "434 SRD", ModAM},
    {438000000, 440000000, "70cm ham", ModFM}, // amateur
    {440000000, 446000000, "UHF LMR", ModFM},
    {446000000, 446200000, "PMR446", ModFM}, // EU walkie-talkie
    {446200000, 462000000, "UHF LMR", ModFM},
    {462000000, 464000000, "FRS/GMRS US", ModFM}, // US walkie-talkie (462-467)
    // 779-928 MHz (CC1101 high): 868 EU ISM, 915 US ISM, 920 JP, 800/900 cellular
    {779000000, 787000000, "780 ISM CN", ModFM},
    {791000000, 821000000, "LTE800 DL", ModFM}, // cell tower -> you (EU b20)
    {832000000, 862000000, "LTE800 UL", ModFM}, // device -> tower (chase this)
    {863000000, 868000000, "863 SRD EU", ModFM}, // audio/alarms
    {868000000, 870000000, "868 ISM EU", ModFM}, // LoRa/Z-Wave/keyfob/meter (868.3)
    {870000000, 876000000, "SRD 870", ModFM},
    {876000000, 880000000, "GSM-R rail", ModFM},
    {880000000, 902000000, "GSM900 UL", ModFM}, // mobile uplink (EU/global)
    {902000000, 915000000, "915 ISM US", ModFM}, // US/ITU-2 LoRa/Zigbee/cordless
    {915000000, 921000000, "915 ISM US", ModFM},
    {921000000, 925000000, "920 ISM JP", ModFM}, // Japan/Asia 920 MHz band
    {925000000, 928000000, "GSM900 DL", ModFM}, // tower downlink
};

static const char* mod_str(Mod m) {
    return m == ModAM ? "AM" : "FM";
}

static const FreqBand* band_lookup(uint32_t f) {
    for(size_t i = 0; i < COUNT_OF(KNOWN_BANDS); i++) {
        if(f >= KNOWN_BANDS[i].lo && f < KNOWN_BANDS[i].hi) return &KNOWN_BANDS[i];
    }
    return NULL;
}

// Config "Band" selector = full-band sweeps first (incl. the whole 300-928 MHz, gaps
// auto-skipped), then every known allocation. Selecting one fills the range + modulation.
static const FreqBand FULL_BANDS[] = {
    {300000000, 928000000, "Full spectrum", ModAM}, // all three CC1101 bands end to end
    {300000000, 348000000, "Full low", ModAM},
    {387000000, 464000000, "Full mid", ModAM},
    {779000000, 928000000, "Full high", ModFM},
};
#define FULL_PRESETS (COUNT_OF(FULL_BANDS))
#define PRESET_COUNT (FULL_PRESETS + COUNT_OF(KNOWN_BANDS))

static const FreqBand* preset_at(uint16_t i) {
    return (i < FULL_PRESETS) ? &FULL_BANDS[i] : &KNOWN_BANDS[i - FULL_PRESETS];
}

// step choices (Hz) and their short labels for the config "Step" item
static const uint32_t STEP_HZ[] = {50000, 100000, 200000, 325000, 650000};
static const char* const STEP_TXT[] = {"50k", "100k", "200k", "325k", "650k"};

typedef struct {
    // scan config
    uint32_t f_start;
    uint32_t f_end;
    uint32_t f_step;
    uint16_t nbins;
    uint8_t settle_ms;
    Mod mod;
    volatile bool reconfig; // config changed -> worker recomputes bins + resets buffers

    // scan data as int8 dBm (worker writes, GUI reads; per-byte loads/stores are
    // atomic on Cortex-M4, so a torn mix is only cosmetic). Sentinel = -128.
    int8_t rssi[MAX_BINS];
    int8_t peak[MAX_BINS];
    uint32_t sweeps;

    // waterfall history: one downsampled (col-max) int8 row per finished sweep, ring buffer
    int8_t hist[HIST_ROWS][SCR_W];
    uint8_t hist_head;
    uint8_t hist_count;

    // spectrum UI state
    uint8_t page; // 0=bars 1=waterfall 2=numeric
    uint16_t cursor; // bars cursor column (framed); OK zooms into it
    struct {
        uint32_t s, e, st;
    } zstack[4]; // zoom-out stack (previous ranges)
    uint8_t zdepth;

    FuriThread* worker;
    volatile bool running;

    Gui* gui;
    ViewDispatcher* vd;
    View* view; // spectrum
    VariableItemList* conf; // config screen
    VariableItem* it_range; // "Range MHz" readout, updated when a band is picked
    VariableItem* it_mod; // "Mod" AM/FM
    FuriTimer* redraw;
} App;

static App* g_app;

// max rssi/peak over the bins that map to display column px (shared by the bars draw
// and the waterfall row builder so both downsample the same way)
static void col_max(const App* app, int px, int8_t* cur, int8_t* pk) {
    uint32_t b0 = (uint32_t)px * app->nbins / SCR_W;
    uint32_t b1 = (uint32_t)(px + 1) * app->nbins / SCR_W;
    if(b1 <= b0) b1 = b0 + 1;
    if(b1 > app->nbins) b1 = app->nbins;
    int8_t c = -128, p = -128;
    for(uint32_t b = b0; b < b1; b++) {
        if(app->rssi[b] > c) c = app->rssi[b];
        if(app->peak[b] > p) p = app->peak[b];
    }
    *cur = c;
    *pk = p;
}

// ---- radio sweep worker ----------------------------------------------------

static int32_t sweep_worker(void* ctx) {
    App* app = ctx;
    FURI_LOG_I(TAG, "worker: start, reset+preset");
    furi_hal_subghz_reset();
    furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);

    while(app->running) {
        // apply a pending config change: recompute bins for the new range/step and
        // clear the buffers so stale readings from the old range don't linger
        if(app->reconfig) {
            uint32_t n = (app->f_end - app->f_start) / app->f_step + 1;
            if(n > MAX_BINS) n = MAX_BINS;
            if(n < 1) n = 1;
            app->nbins = (uint16_t)n;
            for(uint16_t i = 0; i < app->nbins; i++) {
                app->rssi[i] = -128;
                app->peak[i] = -128;
            }
            app->sweeps = 0;
            app->hist_head = 0;
            app->hist_count = 0; // waterfall history is per-range
            app->reconfig = false;
            FURI_LOG_I(
                TAG,
                "worker: reconfig start=%lu end=%lu step=%lu nbins=%u",
                (unsigned long)app->f_start,
                (unsigned long)app->f_end,
                (unsigned long)app->f_step,
                app->nbins);
        }
        for(uint16_t i = 0; i < app->nbins && app->running; i++) {
            uint32_t f = app->f_start + (uint32_t)i * app->f_step;
            if(!furi_hal_subghz_is_frequency_valid(f) || !tune_rx(f)) {
                app->rssi[i] = -128; // sentinel: band gap / invalid / un-tunable
                continue;
            }
            furi_delay_ms(app->settle_ms);
            float rf = furi_hal_subghz_get_rssi();
            int8_t r = (rf < -127.0f) ? (int8_t)-127 : (int8_t)rf; // keep -128 as the sentinel
            app->rssi[i] = r;
            if(r > app->peak[i]) app->peak[i] = r;
        }
        // push a downsampled row into the waterfall ring
        int8_t* row = app->hist[app->hist_head];
        for(int px = 0; px < SCR_W; px++) {
            int8_t c, p;
            col_max(app, px, &c, &p);
            row[px] = c;
        }
        app->hist_head = (uint8_t)((app->hist_head + 1) % HIST_ROWS);
        if(app->hist_count < HIST_ROWS) app->hist_count++;
        app->sweeps++;
        if((app->sweeps & 0x0F) == 0)
            FURI_LOG_D(TAG, "worker: sweep %lu", (unsigned long)app->sweeps);
    }

    FURI_LOG_I(TAG, "worker: exit, radio idle+sleep");
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    return 0;
}

// ---- bars view -------------------------------------------------------------

// bar height for a dBm against a dynamic [floor,ceil] window (autoscale)
static int bar_h(int dbm, int floor, int ceil) {
    if(ceil <= floor) ceil = floor + 1;
    if(dbm < floor) dbm = floor;
    if(dbm > ceil) dbm = ceil;
    return (dbm - floor) * MAX_H / (ceil - floor);
}

// autoscale the vertical window to the data in view, so the noise floor shows texture
// and weak signals aren't clamped flat. min 15 dB window.
static void autoscale(const App* app, int* floor, int* ceil) {
    int lo = 127, hi = -128;
    for(uint16_t i = 0; i < app->nbins; i++) {
        int8_t v = app->rssi[i];
        if(v == -128) continue; // sentinel / band gap
        if(v < lo) lo = v;
        if(v > hi) hi = v;
    }
    if(hi < lo) {
        lo = (int)RSSI_FLOOR;
        hi = (int)RSSI_CEIL;
    }
    if(hi - lo < 15) {
        hi = lo + 15;
    }
    *floor = lo;
    *ceil = hi;
}

// center frequency of grouped bar k (0..NBARS-1)
static uint32_t grp_freq(const App* app, uint16_t k) {
    uint32_t bin = ((uint32_t)k * 2 + 1) * app->nbins / (NBARS * 2);
    if(bin >= app->nbins) bin = app->nbins ? app->nbins - 1 : 0;
    return app->f_start + bin * app->f_step;
}

// max rssi/peak over the bins of grouped bar k
static void grp_max(const App* app, uint16_t k, int8_t* cur, int8_t* pk) {
    uint32_t b0 = (uint32_t)k * app->nbins / NBARS;
    uint32_t b1 = (uint32_t)(k + 1) * app->nbins / NBARS;
    if(b1 <= b0) b1 = b0 + 1;
    if(b1 > app->nbins) b1 = app->nbins;
    int8_t c = -128, p = -128;
    for(uint32_t b = b0; b < b1; b++) {
        if(app->rssi[b] > c) c = app->rssi[b];
        if(app->peak[b] > p) p = app->peak[b];
    }
    *cur = c;
    *pk = p;
}

static void spec_redraw(App* app) {
    with_view_model(app->view, void** m, { UNUSED(m); }, true);
}

static void zoom_in(App* app, uint16_t k) {
    if(app->zdepth >= COUNT_OF(app->zstack)) return;
    uint32_t fc = grp_freq(app, k); // center of the framed grouped bar
    uint32_t span = app->f_end - app->f_start;
    uint32_t ns_span = span / 5;
    if(ns_span < 2000000) ns_span = 2000000; // don't zoom below ~2 MHz
    uint32_t ns = (fc > ns_span / 2) ? (fc - ns_span / 2) : 300000000;
    uint32_t ne = ns + ns_span;
    if(ns < 300000000) ns = 300000000;
    if(ne > 928000000) {
        ne = 928000000;
        ns = (ne > ns_span) ? (ne - ns_span) : 300000000;
    }
    app->zstack[app->zdepth].s = app->f_start;
    app->zstack[app->zdepth].e = app->f_end;
    app->zstack[app->zdepth].st = app->f_step;
    app->zdepth++;
    app->f_start = ns;
    app->f_end = ne;
    uint32_t st = app->f_step / 4;
    if(st < 10000) st = 10000; // finer step, min 10 kHz
    app->f_step = st;
    app->cursor = NBARS / 2;
    app->reconfig = true;
    FURI_LOG_I(
        TAG,
        "zoom in -> %lu-%lu step=%lu depth=%u",
        (unsigned long)ns,
        (unsigned long)ne,
        (unsigned long)st,
        app->zdepth);
}

static void zoom_out(App* app) {
    if(!app->zdepth) return;
    app->zdepth--;
    app->f_start = app->zstack[app->zdepth].s;
    app->f_end = app->zstack[app->zdepth].e;
    app->f_step = app->zstack[app->zdepth].st;
    app->cursor = NBARS / 2;
    app->reconfig = true;
    FURI_LOG_I(TAG, "zoom out depth=%u", app->zdepth);
}

// peak line: "<freq>.<d> <dBm> ~<band> <AM/FM>" for a given frequency+dBm
static void draw_info(Canvas* canvas, uint32_t f, int dbm) {
    char s[48];
    const FreqBand* b = band_lookup(f);
    if(b) {
        snprintf(
            s,
            sizeof(s),
            "%lu.%lu %d ~%s %s",
            (unsigned long)(f / 1000000),
            (unsigned long)((f / 100000) % 10),
            dbm,
            b->name,
            mod_str(b->mod));
    } else {
        snprintf(
            s,
            sizeof(s),
            "%lu.%lu MHz  %d dBm",
            (unsigned long)(f / 1000000),
            (unsigned long)((f / 100000) % 10),
            dbm);
    }
    canvas_draw_str(canvas, 2, 18, s);
}

static void draw_bars(Canvas* canvas, App* app, int floor, int ceil) {
    for(uint16_t k = 0; k < NBARS; k++) {
        int8_t cur, pk;
        grp_max(app, k, &cur, &pk);
        int x = k * BAR_PITCH;
        int h = bar_h(cur, floor, ceil);
        if(h > 0) canvas_draw_box(canvas, x, BASE_Y - h, BAR_PITCH - 1, h);
        int ph = bar_h(pk, floor, ceil);
        if(ph > 0) canvas_draw_line(canvas, x, BASE_Y - ph, x + BAR_PITCH - 2, BASE_Y - ph);
    }
    canvas_draw_line(canvas, 0, BASE_Y + 1, SCR_W - 1, BASE_Y + 1);
    // wide cursor frame around the selected grouped bar (the band you'd zoom into)
    int cx = (int)app->cursor * BAR_PITCH;
    if(cx < 1) cx = 1;
    canvas_draw_frame(canvas, cx - 1, BODY_Y0 - 2, BAR_PITCH + 1, BASE_Y - BODY_Y0 + 3);
}

static void draw_waterfall(Canvas* canvas, App* app, int floor, int ceil) {
    int thr = floor + (ceil - floor) * 3 / 10; // activity threshold within the dynamic window
    for(uint8_t r = 0; r < app->hist_count && r < HIST_ROWS; r++) {
        uint8_t idx = (uint8_t)((app->hist_head + HIST_ROWS - 1 - r) % HIST_ROWS);
        int y = BODY_Y0 + r;
        if(y > BASE_Y) break;
        const int8_t* row = app->hist[idx];
        for(int px = 0; px < SCR_W; px++)
            if(row[px] > thr) canvas_draw_dot(canvas, px, y);
    }
}

// Numeric page: 4 full-width horizontal bars (precise top-4 bins). Bar length ~ strength;
// the "<freq> <dBm> ~<band>" label is drawn in XOR so it reads black on the empty part and
// white over the filled part. Fills the whole screen; the locked range sits by the pills.
static void draw_numeric(Canvas* canvas, App* app) {
    int tv[4] = {-200, -200, -200, -200};
    uint16_t tb[4] = {0}; // precise top-4 bins
    for(uint16_t i = 0; i < app->nbins; i++) {
        int v = app->rssi[i];
        if(v <= -128) continue;
        for(int m = 0; m < 4; m++) {
            if(v > tv[m]) {
                for(int j = 3; j > m; j--) {
                    tv[j] = tv[j - 1];
                    tb[j] = tb[j - 1];
                }
                tv[m] = v;
                tb[m] = i;
                break;
            }
        }
    }
    int floor, ceil;
    autoscale(app, &floor, &ceil);
    const int pitch = 12, bh = 10;
    for(int k = 0; k < 4; k++) {
        int y = 1 + k * pitch;
        if(tv[k] <= -200) {
            canvas_draw_frame(canvas, 0, y, SCR_W, bh); // empty slot outline
            continue;
        }
        uint32_t f = app->f_start + (uint32_t)tb[k] * app->f_step;
        int len = (tv[k] - floor) * SCR_W / ((ceil > floor) ? (ceil - floor) : 1);
        if(len < 2) len = 2;
        if(len > SCR_W) len = SCR_W;
        canvas_draw_box(canvas, 0, y, len, bh);
        const FreqBand* b = band_lookup(f);
        char s[36];
        snprintf(
            s,
            sizeof(s),
            "%lu.%03lu  %d  %s",
            (unsigned long)(f / 1000000),
            (unsigned long)((f / 1000) % 1000),
            tv[k],
            b ? b->name : "");
        canvas_set_color(canvas, ColorXOR); // reverse over the filled part, normal over empty
        canvas_draw_str(canvas, 3, y + bh - 2, s);
        canvas_set_color(canvas, ColorBlack);
    }
}

static void spectrum_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    App* app = g_app;
    if(!app) return; // defensive: never draw after teardown
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    if(app->page == 2) {
        // numeric fills the whole screen; show the locked range by the pills
        draw_numeric(canvas, app);
        char r[24];
        snprintf(
            r,
            sizeof(r),
            "%lu-%lu",
            (unsigned long)(app->f_start / 1000000),
            (unsigned long)(app->f_end / 1000000));
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, r);
    } else {
        // header: range + likely band name for the range center
        uint32_t fc = app->f_start / 2 + app->f_end / 2;
        const FreqBand* hb = band_lookup(fc);
        char hdr[40];
        snprintf(
            hdr,
            sizeof(hdr),
            "%lu-%lu %s",
            (unsigned long)(app->f_start / 1000000),
            (unsigned long)(app->f_end / 1000000),
            hb ? hb->name : "");
        canvas_draw_str(canvas, 2, 8, hdr);
        // info line: cursor readout on bars, global peak on waterfall
        if(app->page == 0) {
            int8_t cur, pk;
            grp_max(app, app->cursor, &cur, &pk);
            draw_info(canvas, grp_freq(app, app->cursor), cur);
        } else {
            int pmax = -200;
            uint16_t pidx = 0;
            for(uint16_t i = 0; i < app->nbins; i++) {
                if(app->rssi[i] > pmax) {
                    pmax = app->rssi[i];
                    pidx = i;
                }
            }
            draw_info(canvas, app->f_start + (uint32_t)pidx * app->f_step, pmax);
        }
        int floor, ceil;
        autoscale(app, &floor, &ceil);
        if(app->page == 0)
            draw_bars(canvas, app, floor, ceil);
        else
            draw_waterfall(canvas, app, floor, ceil);
    }

    // native pill hints
    elements_button_left(canvas, "Page");
    if(app->page == 0) elements_button_center(canvas, "Zoom");
    if(app->zdepth) elements_button_right(canvas, "Out");
}

static bool spectrum_input(InputEvent* event, void* context) {
    UNUSED(context);
    App* app = g_app;
    if(!app) return false;
    bool sp = (event->type == InputTypeShort);
    bool lp = (event->type == InputTypeLong);

    if(event->key == InputKeyBack && sp) {
        if(app->zdepth) {
            zoom_out(app);
            spec_redraw(app);
            return true;
        }
        return false; // -> config
    }
    if(event->key == InputKeyLeft && (sp || lp)) {
        if(sp && app->page == 0 && app->cursor > 0)
            app->cursor--; // move cursor
        else
            app->page = (uint8_t)((app->page + 2) % 3); // edge / long -> prev page
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyRight && (sp || lp)) {
        if(sp && app->page == 0 && app->cursor < NBARS - 1)
            app->cursor++;
        else
            app->page = (uint8_t)((app->page + 1) % 3); // edge / long -> next page
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyOk && sp && app->page == 0) {
        zoom_in(app, app->cursor); // click the framed band -> zoom in
        spec_redraw(app);
        return true;
    }
    return true;
}

static uint32_t view_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

static uint32_t to_conf(void* ctx) {
    UNUSED(ctx);
    return VIEW_CONF; // Back from the spectrum returns to config
}

// ---- config screen ---------------------------------------------------------

static void band_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    const FreqBand* b = preset_at(idx);
    variable_item_set_current_value_text(item, b->name);
    app->f_start = b->lo;
    app->f_end = b->hi;
    app->mod = b->mod;
    app->reconfig = true;
    FURI_LOG_I(
        TAG,
        "config: band[%u]=%s %lu-%lu",
        idx,
        b->name,
        (unsigned long)b->lo,
        (unsigned long)b->hi);
    // reflect the chosen band into the range readout and the modulation item
    char r[24];
    snprintf(
        r,
        sizeof(r),
        "%lu-%lu",
        (unsigned long)(b->lo / 1000000),
        (unsigned long)(b->hi / 1000000));
    variable_item_set_current_value_text(app->it_range, r);
    variable_item_set_current_value_index(app->it_mod, b->mod);
    variable_item_set_current_value_text(app->it_mod, mod_str(b->mod));
}

static void mod_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->mod = variable_item_get_current_value_index(item) ? ModFM : ModAM;
    variable_item_set_current_value_text(item, mod_str(app->mod));
}

static void step_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= COUNT_OF(STEP_HZ)) idx = COUNT_OF(STEP_HZ) - 1;
    app->f_step = STEP_HZ[idx];
    variable_item_set_current_value_text(item, STEP_TXT[idx]);
    app->reconfig = true;
}

static void conf_enter(void* ctx, uint32_t index) {
    UNUSED(index);
    App* app = ctx;
    FURI_LOG_I(TAG, "config: OK -> spectrum");
    view_dispatcher_switch_to_view(app->vd, VIEW_SPEC); // OK starts the scan
}

static void redraw_cb(void* ctx) {
    App* app = ctx;
    // commit the (empty) model to trigger a redraw from the current buffers
    with_view_model(app->view, void** m, { UNUSED(m); }, true);
}

// ---- entry -----------------------------------------------------------------

int32_t rf_survey_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "app: start (sizeof App=%u)", (unsigned)sizeof(App));
    App* app = malloc(sizeof(App));
    if(!app) {
        FURI_LOG_E(TAG, "app: malloc failed");
        return -1;
    }
    memset(app, 0, sizeof(App));
    g_app = app;

    // default scan: the whole 779-928 MHz window
    app->f_start = 779000000;
    app->f_end = 928000000;
    app->f_step = 650000;
    app->settle_ms = 3;
    app->mod = ModFM; // matches the default "Full high" band preset
    app->cursor = NBARS / 2;
    // defensive against bad config (future editable ranges): never div-by-zero,
    // never an inverted range, always 1..MAX_BINS bins, and keep f_end consistent
    // with the bins actually scanned so the axis labels can't lie.
    if(app->f_step == 0) app->f_step = 650000;
    if(app->f_end < app->f_start) app->f_end = app->f_start;
    uint32_t n = (app->f_end - app->f_start) / app->f_step + 1;
    if(n > MAX_BINS) n = MAX_BINS;
    if(n < 1) n = 1;
    app->nbins = (uint16_t)n;
    app->f_end = app->f_start + (uint32_t)(app->nbins - 1) * app->f_step;
    for(uint16_t i = 0; i < app->nbins; i++) {
        app->rssi[i] = -128;
        app->peak[i] = -128;
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->vd = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    // spectrum view (custom): Back returns to config
    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(void*));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, spectrum_draw);
    view_set_input_callback(app->view, spectrum_input);
    view_set_previous_callback(app->view, to_conf);
    view_dispatcher_add_view(app->vd, VIEW_SPEC, app->view);

    // config view (VariableItemList): pick a band preset (full spectrum / full band /
    // known allocation) -> fills range + AM/FM; also Range readout, Mod and Step. OK scans.
    app->conf = variable_item_list_alloc();
    VariableItem* vi = variable_item_list_add(app->conf, "Band", PRESET_COUNT, band_cb, app);
    variable_item_set_current_value_index(vi, 3); // default "Full high" (779-928)
    variable_item_set_current_value_text(vi, preset_at(3)->name);
    app->it_range = variable_item_list_add(app->conf, "Range MHz", 1, NULL, app);
    variable_item_set_current_value_text(app->it_range, "779-928");
    app->it_mod = variable_item_list_add(app->conf, "Mod", 2, mod_cb, app);
    variable_item_set_current_value_index(app->it_mod, app->mod);
    variable_item_set_current_value_text(app->it_mod, mod_str(app->mod));
    vi = variable_item_list_add(app->conf, "Step", COUNT_OF(STEP_HZ), step_cb, app);
    variable_item_set_current_value_index(vi, 4); // 650k
    variable_item_set_current_value_text(vi, "650k");
    variable_item_list_set_enter_callback(app->conf, conf_enter, app);
    view_set_previous_callback(variable_item_list_get_view(app->conf), view_exit);
    view_dispatcher_add_view(app->vd, VIEW_CONF, variable_item_list_get_view(app->conf));

    // install the permissive region for the scan (RX only); restored on exit
    const FuriHalRegion* orig_region = furi_hal_region_get();
    furi_hal_region_set((FuriHalRegion*)&s_region);
    FURI_LOG_I(TAG, "app: region set permissive (was %s)", furi_hal_region_get_name());

    app->running = true;
    // 3 KB stack: the sweep worker calls into the subghz HAL (SPI); past apps in this
    // family hit an MPU/stack fault from underestimating, so leave headroom.
    app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 3072, sweep_worker, app);
    furi_thread_start(app->worker);

    app->redraw = furi_timer_alloc(redraw_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->redraw, 150);

    FURI_LOG_I(TAG, "app: views ready, worker+timer up, entering dispatcher");
    view_dispatcher_switch_to_view(app->vd, VIEW_CONF); // land on config
    view_dispatcher_run(app->vd);
    FURI_LOG_I(TAG, "app: dispatcher returned, tearing down");

    // teardown order: stop the redraw timer (no more draw callbacks), then stop and
    // join the worker (no more radio/buffer writes), only then free views and data.
    furi_timer_stop(app->redraw);
    furi_timer_free(app->redraw);
    app->running = false;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    furi_hal_region_set((FuriHalRegion*)orig_region); // restore the user's region

    view_dispatcher_remove_view(app->vd, VIEW_SPEC);
    view_dispatcher_remove_view(app->vd, VIEW_CONF);
    view_free(app->view);
    variable_item_list_free(app->conf);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_GUI);
    g_app = NULL; // defensive: a stray draw after this sees NULL, not freed memory
    free(app);
    FURI_LOG_I(TAG, "app: end");
    return 0;
}
