#include <furi.h>
#include <furi_hal_subghz.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <gui/elements.h>
#include <gui/modules/variable_item_list.h>

// CC1101 preset register tables are exported to apps; AM650 = wide OOK, good for a
// broadband RSSI survey. (Same names the stock Spectrum Analyzer / SubGHz use.)
extern const uint8_t subghz_device_cc1101_preset_ook_650khz_async_regs[];

#define MAX_BINS   1024 // enough for a full 300-928 MHz sweep @650k (~967 bins); int8 -> 2 KB
#define VIEW_SPEC  0
#define VIEW_CONF  1
#define SCR_W      128
#define BASE_Y     54 // spectrum baseline
#define MAX_H      40 // tallest bar
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

// ---- radio sweep worker ----------------------------------------------------

static int32_t sweep_worker(void* ctx) {
    App* app = ctx;
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
            app->reconfig = false;
        }
        for(uint16_t i = 0; i < app->nbins && app->running; i++) {
            uint32_t f = app->f_start + (uint32_t)i * app->f_step;
            if(!furi_hal_subghz_is_frequency_valid(f)) {
                app->rssi[i] = -128; // sentinel: band gap / invalid
                continue;
            }
            furi_hal_subghz_idle();
            furi_hal_subghz_set_frequency_and_path(f); // picks the matching RF path per band
            furi_hal_subghz_rx();
            furi_delay_ms(app->settle_ms);
            float rf = furi_hal_subghz_get_rssi();
            int8_t r = (rf < -127.0f) ? (int8_t)-127 : (int8_t)rf; // keep -128 as the sentinel
            app->rssi[i] = r;
            if(r > app->peak[i]) app->peak[i] = r;
        }
        app->sweeps++;
    }

    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    return 0;
}

// ---- bars view -------------------------------------------------------------

static int bar_h(int dbm) {
    if(dbm < (int)RSSI_FLOOR) dbm = (int)RSSI_FLOOR;
    if(dbm > (int)RSSI_CEIL) dbm = (int)RSSI_CEIL;
    return (dbm - (int)RSSI_FLOOR) * MAX_H / ((int)RSSI_CEIL - (int)RSSI_FLOOR);
}

static void bars_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    App* app = g_app;
    if(!app) return; // defensive: never draw after teardown
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    // find the current peak bin
    int pmax = -200;
    uint16_t pidx = 0;
    for(uint16_t i = 0; i < app->nbins; i++) {
        if(app->rssi[i] > pmax) {
            pmax = app->rssi[i];
            pidx = i;
        }
    }
    uint32_t pfreq = app->f_start + (uint32_t)pidx * app->f_step;

    char hdr[40];
    snprintf(
        hdr,
        sizeof(hdr),
        "%lu-%lu  sw%lu",
        (unsigned long)(app->f_start / 1000000),
        (unsigned long)(app->f_end / 1000000),
        (unsigned long)app->sweeps);
    canvas_draw_str(canvas, 2, 8, hdr);

    char pk[48];
    const FreqBand* bn = band_lookup(pfreq);
    if(bn) {
        // compact "<freq> <dBm> ~<band> <AM/FM>" so the tag fits one 128px line
        snprintf(
            pk,
            sizeof(pk),
            "%lu.%lu %d ~%s %s",
            (unsigned long)(pfreq / 1000000),
            (unsigned long)((pfreq / 100000) % 10),
            pmax,
            bn->name,
            mod_str(bn->mod));
    } else {
        snprintf(
            pk,
            sizeof(pk),
            "%lu.%lu MHz  %d dBm",
            (unsigned long)(pfreq / 1000000),
            (unsigned long)((pfreq / 100000) % 10),
            pmax);
    }
    canvas_draw_str(canvas, 2, 18, pk);

    // bars: each column spans nbins/SCR_W bins; take the MAX over that span so a peak
    // between bins isn't skipped when the range is wide (e.g. full spectrum downsample)
    for(int px = 0; px < SCR_W; px++) {
        uint32_t b0 = (uint32_t)px * app->nbins / SCR_W;
        uint32_t b1 = (uint32_t)(px + 1) * app->nbins / SCR_W;
        if(b1 <= b0) b1 = b0 + 1;
        if(b1 > app->nbins) b1 = app->nbins;
        int8_t cur = -128, pk2 = -128;
        for(uint32_t b = b0; b < b1; b++) {
            if(app->rssi[b] > cur) cur = app->rssi[b];
            if(app->peak[b] > pk2) pk2 = app->peak[b];
        }
        int h = bar_h(cur);
        if(h > 0) canvas_draw_line(canvas, px, BASE_Y, px, BASE_Y - h);
        int ph = bar_h(pk2);
        if(ph > 0) canvas_draw_dot(canvas, px, BASE_Y - ph);
    }
    canvas_draw_line(canvas, 0, BASE_Y + 1, SCR_W - 1, BASE_Y + 1);

    // frequency axis (start / end)
    char fa[12];
    snprintf(fa, sizeof(fa), "%lu", (unsigned long)(app->f_start / 1000000));
    canvas_draw_str(canvas, 2, 63, fa);
    snprintf(fa, sizeof(fa), "%lu", (unsigned long)(app->f_end / 1000000));
    canvas_draw_str(canvas, SCR_W - 20, 63, fa);
}

static bool bars_input(InputEvent* event, void* context) {
    UNUSED(context);
    if(event->type == InputTypeShort && event->key == InputKeyBack) return false; // -> exit
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
    App* app = malloc(sizeof(App));
    if(!app) return -1;
    memset(app, 0, sizeof(App));
    g_app = app;

    // default scan: the whole 779-928 MHz window
    app->f_start = 779000000;
    app->f_end = 928000000;
    app->f_step = 650000;
    app->settle_ms = 3;
    app->mod = ModFM; // matches the default "Full high" band preset
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
    view_set_draw_callback(app->view, bars_draw);
    view_set_input_callback(app->view, bars_input);
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

    app->running = true;
    // 3 KB stack: the sweep worker calls into the subghz HAL (SPI); past apps in this
    // family hit an MPU/stack fault from underestimating, so leave headroom.
    app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 3072, sweep_worker, app);
    furi_thread_start(app->worker);

    app->redraw = furi_timer_alloc(redraw_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->redraw, 150);

    view_dispatcher_switch_to_view(app->vd, VIEW_CONF); // land on config
    view_dispatcher_run(app->vd);

    // teardown order: stop the redraw timer (no more draw callbacks), then stop and
    // join the worker (no more radio/buffer writes), only then free views and data.
    furi_timer_stop(app->redraw);
    furi_timer_free(app->redraw);
    app->running = false;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    view_dispatcher_remove_view(app->vd, VIEW_SPEC);
    view_dispatcher_remove_view(app->vd, VIEW_CONF);
    view_free(app->view);
    variable_item_list_free(app->conf);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_GUI);
    g_app = NULL; // defensive: a stray draw after this sees NULL, not freed memory
    free(app);
    return 0;
}
