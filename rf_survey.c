#include <furi.h>
#include <furi_hal_subghz.h>
#include <furi_hal_region.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <gui/elements.h>
#include <gui/modules/variable_item_list.h>
#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <notification/notification_messages.h>
#include <flipper_format/flipper_format.h>

// CC1101 preset register tables exported to apps. These set the RX filter bandwidth; the
// survey preset is user-selectable in config (same four names the stock Spectrum Analyzer /
// SubGHz use). AM650 (widest OOK) is the default -- best broadband energy pickup at wide steps.
extern const uint8_t subghz_device_cc1101_preset_ook_650khz_async_regs[];
extern const uint8_t subghz_device_cc1101_preset_ook_270khz_async_regs[];
extern const uint8_t subghz_device_cc1101_preset_2fsk_dev2_38khz_async_regs[];
extern const uint8_t subghz_device_cc1101_preset_2fsk_dev47_6khz_async_regs[];

typedef struct {
    const char* name;
    const uint8_t* regs;
    const char* sub; // stock Preset name written into a .sub so SubGHz can replay it
} SurveyPreset;

// available in both OFW (87.1) and Unleashed (88.9) SDKs
static const SurveyPreset PRESETS[] = {
    {"AM650", subghz_device_cc1101_preset_ook_650khz_async_regs, "FuriHalSubGhzPresetOok650Async"},
    {"AM270", subghz_device_cc1101_preset_ook_270khz_async_regs, "FuriHalSubGhzPresetOok270Async"},
    {"FM238",
     subghz_device_cc1101_preset_2fsk_dev2_38khz_async_regs,
     "FuriHalSubGhzPreset2FSKDev238Async"},
    {"FM476",
     subghz_device_cc1101_preset_2fsk_dev47_6khz_async_regs,
     "FuriHalSubGhzPreset2FSKDev476Async"},
};
#define PRESET_N COUNT_OF(PRESETS)

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
#define VIEW_RANGE 2 // custom start/end frequency editor
#define VIEW_CAP   3 // .sub capture screen
#define SCR_W      128
#define RE_MIN     3000 // 300.0 MHz in 100 kHz units (the CC1101 low-band floor)
#define RE_MAX     9280 // 928.0 MHz
#define RE_SPAN    10 // minimum span: 1.0 MHz
#define BASE_Y     50 // spectrum baseline (room for pill buttons below)
#define MAX_H      28 // tallest bar
#define BODY_Y0    22 // top of the page body
#define WF_ROWS    40 // waterfall freq rows (full-bleed height)
#define WF_COLS    120 // waterfall time columns: one per completed sweep, ring
#define WF_H       49 // waterfall heatmap height; y 49..63 holds the time scale + hints
#define NBARS      32 // thick grouped bars (4px pitch) -> easy cursor stepping
#define BAR_PITCH  (SCR_W / NBARS)
#define RSSI_FLOOR -100.0f
#define RSSI_CEIL  -40.0f
#define CSV_BUF    512 // one FAT sector: CSV rows flush in chunks instead of a 5 KB line buffer

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

// time-window presets (seconds) for the connected/waterfall pages, Up/Down cycles them
static const uint16_t WIN_SECS[] = {5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 300, 420, 600};
#define WIN_N       COUNT_OF(WIN_SECS)
#define WIN_DEFAULT 6 // 60 s

// "60s" / "2m" / "10m" into buf
static void fmt_win(uint16_t s, char* buf, size_t n) {
    if(s >= 60 && (s % 60) == 0)
        snprintf(buf, n, "%um", (unsigned)(s / 60));
    else
        snprintf(buf, n, "%us", (unsigned)s);
}

typedef struct {
    // scan config
    uint32_t f_start;
    uint32_t f_end;
    uint32_t f_step;
    uint16_t nbins;
    uint8_t settle_ms;
    volatile uint8_t preset_idx; // index into PRESETS[]; worker reloads the CC1101 on change
    volatile uint8_t gain; // 0 Auto(max) / 1 Mid / 2 Low -- caps the AGC's max usable gain
    uint8_t preset_buf[128]; // RAM copy of a preset with AGCCTRL2 patched (for Mid/Low gain)
    volatile bool reconfig; // config changed -> worker recomputes bins + resets buffers

    // scan data as int8 dBm (worker writes, GUI reads; per-byte loads/stores are
    // atomic on Cortex-M4, so a torn mix is only cosmetic). Sentinel = -128.
    int8_t rssi[MAX_BINS];
    int8_t peak[MAX_BINS];
    uint32_t sweeps;

    // waterfall: one column per completed sweep (1024 bins max-folded to WF_ROWS freq rows),
    // ring over time. col_ms stamps each column's wall-clock (ms) so both new pages window by
    // real time, not sweep count. The lit/unlit cut is the user Trigger (a fixed dBm floor),
    // so it never flickers under autoscale and separates weak from strong deterministically.
    // (col_ms is not byte-atomic, but a torn read only mis-windows one column -> cosmetic.)
    int8_t wf[WF_ROWS][WF_COLS];
    uint32_t col_ms[WF_COLS];
    uint8_t wf_head;
    uint8_t wf_count;
    volatile uint8_t win_idx; // index into WIN_SECS[] (connected/waterfall window)
    volatile int8_t trigger; // dBm floor: waterfall-lit / haptic / history / capture gate
    volatile bool haptic; // vibro pulse when a bin exceeds the trigger (hands-free locate)

    // session busy-channel history: which 1 MHz buckets peaked above the trigger, how often
    struct {
        uint16_t mhz;
        uint16_t count;
    } hist[8];
    uint8_t hist_n;

    // CSV logging. The worker owns the File*; the GUI (long-OK) only flips `recording`.
    // Stopped on any reconfig so the fixed-column CSV never mixes two ranges. Rows flush in
    // CSV_BUF chunks (one FAT sector) so this stays 512 B, not a 5 KB per-row line buffer.
    volatile bool recording;
    uint32_t rec_rows;
    char csv_line[CSV_BUF];

    // spectrum UI state
    uint8_t page; // 0=bars 1=connected 2=waterfall 3=numeric 4=history
    uint16_t cursor; // bars cursor column (framed); OK zooms into it
    uint8_t num_sel; // numeric page: which of the top-4 is selected (OK captures it)
    struct {
        uint32_t s, e, st;
    } zstack[4]; // zoom-out stack (previous ranges)
    uint8_t zdepth;

    FuriThread* worker;
    volatile bool running;

    NotificationApp* notif; // haptic feedback (vibro)
    Gui* gui;
    ViewDispatcher* vd;
    View* view; // spectrum
    VariableItemList* conf; // config screen
    VariableItem* it_range; // "Range MHz" readout, updated when a band is picked
    VariableItem* it_preset; // "Modulation" preset (AM650/AM270/FM238/FM476)

    // custom-range editor (VIEW_RANGE): start/end in 100 kHz units (779.0 MHz = 7790)
    View* range_view;
    uint32_t re_start, re_end;
    uint8_t re_field; // 0 = editing Start, 1 = editing End
    uint8_t re_cur; // active digit 0..3 (hundreds, tens, ones, tenths)

    // .sub capture (VIEW_CAP): a dedicated thread owns the file+stream; the ISR only feeds
    // the stream; the GUI flips `capturing`. The survey worker is stopped while capturing so
    // there is never two owners of the one radio.
    View* cap_view;
    FuriThread* cap_worker;
    FuriStreamBuffer* cap_stream; // ISR -> capture thread (int32 signed durations)
    uint32_t cap_freq; // locked capture frequency (Hz)
    uint8_t cap_preset; // preset index for capture (user can toggle OOK/FSK)
    volatile bool capturing;
    volatile bool cap_gate; // Up/Down: RSSI-gate the .sub (default off = record everything).
        // Instantaneous RSSI of an OOK burst is mostly at the noise floor,
        // so gating on the survey Trigger over-trims; off records the whole
        // capture (the >=1 s / <50 us duration filter still drops junk).
    volatile bool cap_paused; // gate engaged and RSSI below trigger -> not writing
    volatile int8_t cap_rssi; // last polled RSSI for the capture screen
    volatile uint32_t cap_samples; // durations written
    volatile uint32_t cap_overflow; // ISR drops (stream full) -> capture may be corrupt
    FuriTimer* redraw;
} App;

static App* g_app;

static void autoscale(const App* app, int* floor, int* ceil); // used by the worker too

// max rssi over the bins that map to waterfall freq-row r (frequency fold is always max,
// never average -- a narrow burst must not wash out).
static int8_t wf_row_max(const App* app, int r) {
    uint32_t b0 = (uint32_t)r * app->nbins / WF_ROWS;
    uint32_t b1 = (uint32_t)(r + 1) * app->nbins / WF_ROWS;
    if(b1 <= b0) b1 = b0 + 1;
    if(b1 > app->nbins) b1 = app->nbins;
    int8_t m = -128;
    for(uint32_t b = b0; b < b1; b++)
        if(app->rssi[b] > m) m = app->rssi[b];
    return m;
}

// record a 1 MHz bucket hit in the session history (MRU by count): bump if present, else
// add, else overwrite the weakest entry. Written by the worker, read by the GUI (cosmetic).
static void hist_add(App* app, uint16_t mhz) {
    for(uint8_t i = 0; i < app->hist_n; i++) {
        if(app->hist[i].mhz == mhz) {
            if(app->hist[i].count < 0xFFFF) app->hist[i].count++;
            return;
        }
    }
    if(app->hist_n < COUNT_OF(app->hist)) {
        app->hist[app->hist_n].mhz = mhz;
        app->hist[app->hist_n].count = 1;
        app->hist_n++;
        return;
    }
    uint8_t lo = 0; // full: replace the lowest-count slot
    for(uint8_t i = 1; i < app->hist_n; i++)
        if(app->hist[i].count < app->hist[lo].count) lo = i;
    app->hist[lo].mhz = mhz;
    app->hist[lo].count = 1;
}

// ---- CSV logging -----------------------------------------------------------

// Open /ext/apps_data/rfsurvey/survey_<rtc>.csv and write the metadata header. The
// columns are fixed for the file's lifetime (one per bin of the current range), so the
// worker stops recording on any reconfig rather than appending a second header.
static File* csv_open(App* app, Storage* storage) {
    storage_common_mkdir(storage, APP_DATA_PATH("")); // idempotent; ignore "exists"
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[96];
    // tick suffix makes the name unique even for two recordings within the same RTC second,
    // so CREATE_ALWAYS can never truncate a previous file.
    snprintf(
        path,
        sizeof(path),
        APP_DATA_PATH("survey_%04u%02u%02u_%02u%02u%02u_%03lu.csv"),
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        (unsigned long)(furi_get_tick() % 1000));
    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "csv: open failed %s", path);
        storage_file_close(f);
        storage_file_free(f);
        return NULL;
    }
    int n = snprintf(
        app->csv_line,
        sizeof(app->csv_line),
        "# RF Survey\n# start=%lu end=%lu step=%lu nbins=%u settle=%u preset=%s\n"
        "# rtc=%04u-%02u-%02u %02u:%02u:%02u\n"
        "# t_ms,bin0..bin%u (dBm; -128=invalid)  binfreq=start+idx*step\n",
        (unsigned long)app->f_start,
        (unsigned long)app->f_end,
        (unsigned long)app->f_step,
        app->nbins,
        app->settle_ms,
        PRESETS[app->preset_idx].name,
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        (unsigned)(app->nbins ? app->nbins - 1 : 0));
    if(storage_file_write(f, app->csv_line, n) != (size_t)n) { // SD full -> no half header
        FURI_LOG_E(TAG, "csv: header write failed");
        storage_file_close(f);
        storage_file_free(f);
        return NULL;
    }
    app->rec_rows = 0;
    FURI_LOG_I(TAG, "csv: recording -> %s", path);
    return f;
}

// flush the pending CSV chunk; returns false on a short write (SD full/failing)
static bool csv_flush(File* csv, const char* buf, int* off) {
    if(*off == 0) return true;
    bool ok = storage_file_write(csv, buf, *off) == (size_t)*off;
    *off = 0;
    return ok;
}

// smallest CC1101 RX-filter bandwidth >= step, as the MDMCFG4 top nibble (CHANBW). Used when
// zoomed so a carrier resolves to a sharp peak instead of a 650 kHz-wide mountain. 0xFF = leave
// the preset's own wide BW (for the un-zoomed survey, so nothing falls between bins).
static uint8_t bw_nibble(uint32_t step) {
    if(step <= 58000) return 0xF; // 58 kHz
    if(step <= 101000) return 0xC; // 101 kHz
    if(step <= 270000) return 0x6; // 270 kHz
    return 0xFF; // wider than 270 kHz -> keep the stock bandwidth
}

// Return the register array to load for preset p. Auto gain + wide BW (bwn==0xFF) use the stock
// array untouched; otherwise copy it into preset_buf and patch AGCCTRL2 (0x1B, gain cap) and/or
// MDMCFG4 (0x10, RX bandwidth -- keep the low data-rate nibble). RX config only.
static const uint8_t* preset_regs(App* app, uint8_t p, uint8_t gain, uint8_t bwn) {
    const uint8_t* src = PRESETS[p].regs;
    if(gain == 0 && bwn == 0xFF) return src; // nothing to patch -> stock preset
    size_t i = 0;
    while(!(src[i] == 0 && src[i + 1] == 0))
        i += 2; // {0,0} terminates the reg pairs
    size_t total = i + 2 + 8; // pairs + terminator + 8-byte PA table
    if(total > sizeof(app->preset_buf)) return src; // too big -> fall back, never overflow
    memcpy(app->preset_buf, src, total);
    for(size_t j = 0; j < i; j += 2) {
        if(gain != 0 && app->preset_buf[j] == 0x1B) // CC1101_AGCCTRL2
            app->preset_buf[j + 1] = (gain == 1) ? 0x1F : 0xB7; // Mid / Low gain cap
        if(bwn != 0xFF && app->preset_buf[j] == 0x10) // CC1101_MDMCFG4
            app->preset_buf[j + 1] = (uint8_t)((bwn << 4) | (app->preset_buf[j + 1] & 0x0F));
    }
    return app->preset_buf;
}

// ---- radio sweep worker ----------------------------------------------------

static int32_t sweep_worker(void* ctx) {
    App* app = ctx;
    FURI_LOG_I(TAG, "worker: start, reset");
    furi_hal_subghz_reset();

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* csv = NULL; // open while recording; owned entirely by this thread
    uint8_t cur_preset = 0xFF, cur_gain = 0xFF, cur_bw = 0x00; // force a load on the first loop

    while(app->running) {
        // (re)load the CC1101 preset on a preset/gain change OR a zoom bandwidth change. When
        // zoomed we narrow the RX filter to ~the step so a carrier shows a sharp peak, not a
        // 650 kHz mountain; un-zoomed keeps the wide survey BW. Snapshot once each.
        uint8_t p = app->preset_idx, g = app->gain;
        if(p >= PRESET_N) p = 0;
        uint8_t bwn = (app->zdepth > 0) ? bw_nibble(app->f_step) : 0xFF;
        if(cur_preset != p || cur_gain != g || cur_bw != bwn) {
            furi_hal_subghz_idle();
            furi_hal_subghz_load_custom_preset(preset_regs(app, p, g, bwn));
            cur_preset = p;
            cur_gain = g;
            cur_bw = bwn;
            FURI_LOG_I(
                TAG, "worker: preset -> %s gain %u bw %02X", PRESETS[p].name, (unsigned)g, bwn);
        }
        // apply a pending config change: recompute bins for the new range/step and
        // clear the buffers so stale readings from the old range don't linger
        if(app->reconfig) {
            // a range change invalidates an open CSV's fixed columns -> stop cleanly
            if(csv) {
                storage_file_close(csv);
                storage_file_free(csv);
                csv = NULL;
                app->recording = false;
                FURI_LOG_I(
                    TAG, "csv: stopped by reconfig, %lu rows", (unsigned long)app->rec_rows);
            }
            if(app->f_step == 0) app->f_step = 650000; // never divide by zero
            if(app->f_end < app->f_start) app->f_end = app->f_start;
            uint32_t n = (app->f_end - app->f_start) / app->f_step + 1;
            if(n > MAX_BINS) n = MAX_BINS;
            if(n < 1) n = 1;
            app->nbins = (uint16_t)n;
            // keep f_end honest: after the MAX_BINS cap (or a non-dividing step) the last bin
            // scanned is f_start+(nbins-1)*step, so pin the displayed end to it (labels can't lie)
            app->f_end = app->f_start + (uint32_t)(app->nbins - 1) * app->f_step;
            for(uint16_t i = 0; i < app->nbins; i++) {
                app->rssi[i] = -128;
                app->peak[i] = -128;
            }
            app->sweeps = 0;
            app->wf_head = 0;
            app->wf_count = 0;
            memset(app->wf, -128, sizeof(app->wf)); // waterfall history is per-range
            memset(app->col_ms, 0, sizeof(app->col_ms));
            app->hist_n = 0; // busy-channel history is per-range too
            app->reconfig = false;
            FURI_LOG_I(
                TAG,
                "worker: reconfig start=%lu end=%lu step=%lu nbins=%u",
                (unsigned long)app->f_start,
                (unsigned long)app->f_end,
                (unsigned long)app->f_step,
                app->nbins);
        }
        // reconcile the file handle with the GUI's recording flag (long-OK toggles it)
        if(app->recording && !csv) {
            csv = csv_open(app, storage);
            if(!csv) app->recording = false; // open failed -> don't get stuck "recording"
        } else if(!app->recording && csv) {
            storage_file_close(csv);
            storage_file_free(csv);
            csv = NULL;
            FURI_LOG_I(TAG, "csv: stopped, %lu rows", (unsigned long)app->rec_rows);
        }
        // snapshot the range for this sweep: a mid-sweep config change (GUI) then only
        // takes effect next loop via reconfig, so one row is never a torn A/B mix.
        uint32_t fs = app->f_start, st = app->f_step;
        int8_t smax = -128; // this sweep's strongest bin + its index (haptic + history)
        uint16_t smaxi = 0;
        for(uint16_t i = 0; i < app->nbins && app->running; i++) {
            uint32_t f = fs + (uint32_t)i * st;
            if(!furi_hal_subghz_is_frequency_valid(f) || !tune_rx(f)) {
                app->rssi[i] = -128; // sentinel: band gap / invalid / un-tunable
                continue;
            }
            furi_delay_ms(app->settle_ms);
            float rf = furi_hal_subghz_get_rssi();
            int8_t r = (rf < -127.0f) ? (int8_t)-127 : (int8_t)rf; // keep -128 as the sentinel
            app->rssi[i] = r;
            if(r > app->peak[i]) app->peak[i] = r;
            if(r > smax) {
                smax = r;
                smaxi = i;
            }
        }
        // commit this sweep as one waterfall column: max-fold the bins to WF_ROWS freq rows,
        // timestamp it (one col == one sweep, so the real time resolution is the sweep period
        // -- honest, no fabricated sub-bins). The lit cut is the user Trigger, applied at draw.
        {
            uint8_t h = (uint8_t)((app->wf_head + 1) % WF_COLS);
            for(int r = 0; r < WF_ROWS; r++)
                app->wf[r][h] = wf_row_max(app, r);
            app->col_ms[h] = furi_get_tick();
            app->wf_head = h;
            if(app->wf_count < WF_COLS) app->wf_count++;
        }
        app->sweeps++;
        if((app->sweeps & 0x0F) == 0)
            FURI_LOG_D(TAG, "worker: sweep %lu", (unsigned long)app->sweeps);

        // above-trigger peak this sweep -> haptic cue + busy-channel history
        if(smax > app->trigger) {
            hist_add(app, (uint16_t)((fs + (uint32_t)smaxi * st) / 1000000));
            // haptic locate cue: pulse the vibro, more often the stronger it is (Geiger-style).
            // Non-blocking via the notification service.
            if(app->haptic && app->notif) {
                uint8_t over = (uint8_t)(smax - app->trigger);
                uint8_t every = over >= 20 ? 1 : over >= 12 ? 2 : over >= 6 ? 3 : 4;
                if((app->sweeps % every) == 0)
                    notification_message(app->notif, &sequence_single_vibro);
            }
        }

        // log one CSV row for this sweep: "t_ms,rssi0,..,rssiN", flushed in CSV_BUF chunks
        // (one FAT sector) so the row buffer stays 512 B instead of ~5 KB.
        if(csv) {
            int off = snprintf(app->csv_line, CSV_BUF, "%lu", (unsigned long)furi_get_tick());
            bool ok = true;
            for(uint16_t i = 0; i < app->nbins; i++) {
                if(off > CSV_BUF - 8) { // keep room for the next ",-128" + newline
                    if(!(ok = csv_flush(csv, app->csv_line, &off))) break;
                }
                off += snprintf(app->csv_line + off, CSV_BUF - off, ",%d", app->rssi[i]);
            }
            if(ok && off < CSV_BUF - 1) app->csv_line[off++] = '\n';
            if(ok) ok = csv_flush(csv, app->csv_line, &off);
            if(!ok) { // SD full / failing -> stop cleanly, don't spin
                FURI_LOG_E(
                    TAG, "csv: write failed, stopping at %lu rows", (unsigned long)app->rec_rows);
                storage_file_close(csv);
                storage_file_free(csv);
                csv = NULL;
                app->recording = false;
            } else {
                app->rec_rows++;
            }
        }
    }

    if(csv) { // app closing mid-record -> flush + close
        storage_file_close(csv);
        storage_file_free(csv);
        FURI_LOG_I(TAG, "csv: closed on exit, %lu rows", (unsigned long)app->rec_rows);
    }
    furi_record_close(RECORD_STORAGE);
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
    // dotted trigger line: the floor that the waterfall/haptic use (where a bar clears it,
    // that bin counts as "signal")
    int th = bar_h(app->trigger, floor, ceil);
    int ty = BASE_Y - th;
    for(int x = 0; x < SCR_W; x += 4)
        canvas_draw_dot(canvas, x, ty);
    // wide cursor frame around the selected grouped bar (the band you'd zoom into)
    int cx = (int)app->cursor * BAR_PITCH;
    if(cx < 1) cx = 1;
    canvas_draw_frame(canvas, cx - 1, BODY_Y0 - 2, BAR_PITCH + 1, BASE_Y - BODY_Y0 + 3);
}

// avg + peak of waterfall freq-row r over the columns captured within the last win_sec.
// One column == one sweep, so the mean of those columns IS the true per-sweep window mean
// (no "average of per-slot maxima" fudge); peak = max over the same columns.
static void wf_window_stats(const App* app, int r, uint32_t now, int* avg, int8_t* pk) {
    uint32_t win_ms = (uint32_t)WIN_SECS[app->win_idx] * 1000;
    int sum = 0, cnt = 0;
    int8_t p = -128;
    for(int c = 0; c < WF_COLS; c++) {
        uint32_t cm = app->col_ms[c];
        if(cm == 0 || now - cm > win_ms) continue; // unwritten or outside the window
        int8_t v = app->wf[r][c];
        if(v == -128) continue; // band gap / invalid
        sum += v;
        cnt++;
        if(v > p) p = v;
    }
    *avg = cnt ? sum / cnt : -128;
    *pk = p;
}

// Connected page: per-frequency AVERAGE over the window as a solid polyline, plus the window
// peak as a dotted envelope above it. "Is this frequency consistently there, and how hot does
// it get?" -- a steadier read than the instantaneous bars.
static void draw_connected(Canvas* canvas, App* app, int floor, int ceil) {
    uint32_t now = furi_get_tick();
    int px_prev = -1, y_prev = 0;
    for(int r = 0; r < WF_ROWS; r++) {
        int avg;
        int8_t pk;
        wf_window_stats(app, r, now, &avg, &pk);
        int x = r * (SCR_W - 1) / (WF_ROWS - 1);
        if(avg > -128) {
            int y = BASE_Y - bar_h(avg, floor, ceil);
            if(px_prev >= 0) canvas_draw_line(canvas, px_prev, y_prev, x, y);
            px_prev = x;
            y_prev = y;
        } else {
            px_prev = -1; // break the line across a gap
        }
        if(pk > -128) canvas_draw_dot(canvas, x, BASE_Y - bar_h(pk, floor, ceil));
    }
    canvas_draw_line(canvas, 0, BASE_Y + 1, SCR_W - 1, BASE_Y + 1);
}

// Horizontal waterfall: X = time (oldest left, newest right), Y = frequency (start at top,
// end at bottom). Full-bleed above the pills. A cell is lit if that sweep's row is above the
// current user Trigger (changing it re-lights all history instantly). Columns are placed by
// real timestamp over
// the last win_sec, nearest-earlier sample per screen x -> fills width honestly (repeats a
// real sweep when sweeps are slower than 1px, never invents sub-sweep detail). Shows WHEN a
// band is busy and the spacing of bursts. Edge labels give the range; center gives the span.
static void draw_waterfall(Canvas* canvas, App* app) {
    uint16_t win = WIN_SECS[app->win_idx];
    uint32_t newest = app->col_ms[app->wf_head];
    if(newest) {
        uint32_t span = (uint32_t)win * 1000;
        uint32_t oldest = (newest > span) ? newest - span : 0;
        for(int x = 0; x < SCR_W; x++) {
            // (newest-oldest) <= 600 s * 1000 = 6e5; *127 = 7.6e7 < 2^32, so 32-bit math is
            // exact here -- no 64-bit software divide per column.
            uint32_t t = oldest + (newest - oldest) * (uint32_t)x / (SCR_W - 1);
            int best = -1;
            uint32_t bestt = 0;
            for(int c = 0; c < WF_COLS; c++) {
                uint32_t cm = app->col_ms[c];
                if(cm == 0 || cm > t) continue;
                if(best < 0 || cm > bestt) {
                    best = c;
                    bestt = cm;
                }
            }
            if(best < 0) continue;
            for(int r = 0; r < WF_ROWS; r++) {
                if(app->wf[r][best] > app->trigger) { // lit only above the user floor
                    int y0 = r * WF_H / WF_ROWS;
                    int y1 = (r + 1) * WF_H / WF_ROWS;
                    if(y1 <= y0) y1 = y0 + 1;
                    canvas_draw_box(canvas, x, y0, 1, y1 - y0);
                }
            }
        }
    }
    // freq labels over the heatmap (XOR so they read on lit or dark cells); kept inside the
    // heatmap band (y < WF_H) so they never collide with the scale/pills below
    char s[12];
    canvas_set_color(canvas, ColorXOR);
    snprintf(s, sizeof(s), "%lu", (unsigned long)(app->f_start / 1000000));
    canvas_draw_str(canvas, 1, 8, s); // top edge = start freq
    snprintf(s, sizeof(s), "%lu", (unsigned long)(app->f_end / 1000000));
    canvas_draw_str(canvas, 1, WF_H - 2, s); // bottom of heatmap = end freq
    canvas_set_color(canvas, ColorBlack);

    // time scale: baseline + tick notches at a "nice" division, newest (0) on the right.
    // ~6 ticks; the division size is labelled so bursts' spacing can be read off the grid.
    static const uint16_t NICE[] = {5, 10, 15, 30, 60, 120, 300};
    uint16_t target = win / 6 ? win / 6 : 5;
    uint16_t div = NICE[0];
    for(size_t i = 0; i < COUNT_OF(NICE); i++)
        if(NICE[i] <= target) div = NICE[i];
    int sy = WF_H; // scale strip top
    canvas_draw_line(canvas, 0, sy, SCR_W - 1, sy);
    for(uint16_t dt = 0; dt <= win; dt += div) {
        int x = (SCR_W - 1) - (int)((uint32_t)dt * (SCR_W - 1) / win);
        canvas_draw_line(canvas, x, sy, x, sy + 3);
    }
    // bottom row: page hint, division size, window total
    canvas_draw_str(canvas, 2, 62, "<>Pg");
    char dv[12];
    if(div >= 60 && (div % 60) == 0)
        snprintf(dv, sizeof(dv), "%um/div", (unsigned)(div / 60));
    else
        snprintf(dv, sizeof(dv), "%us/div", (unsigned)div);
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, dv);
    char w[8];
    fmt_win(win, w, sizeof(w));
    canvas_draw_str_aligned(canvas, SCR_W - 1, 62, AlignRight, AlignBottom, w);
}

// the 4 strongest DISTINCT peaks (descending), tv=dBm tb=bin index. Shared by the numeric
// page and the capture entry so OK captures exactly the slot the cursor is on. Bins within
// GROUP_HZ are treated as one peak (a single transmitter leaks across adjacent bins -- the
// CC1101 RX filter is 270-650 kHz wide, so sub-MHz-apart bins are the same signal), keeping
// only the strongest so the list shows 4 real signals, not 4 slices of one.
#define GROUP_HZ 400000u
static void numeric_top4(const App* app, int tv[4], uint16_t tb[4]) {
    tv[0] = tv[1] = tv[2] = tv[3] = -200;
    tb[0] = tb[1] = tb[2] = tb[3] = 0;
    for(uint16_t i = 0; i < app->nbins; i++) {
        int v = app->peak[i]; // rank by peak-hold, not live RSSI, so the list doesn't jitter
        if(v <= app->trigger) continue; // only real peaks above the floor (no noise filling slots)
        uint32_t fi = app->f_start + (uint32_t)i * app->f_step;
        int grp = -1; // same-peak slot, if any
        for(int m = 0; m < 4; m++) {
            if(tv[m] <= -200) continue;
            uint32_t fm = app->f_start + (uint32_t)tb[m] * app->f_step;
            uint32_t d = (fi > fm) ? (fi - fm) : (fm - fi);
            if(d < GROUP_HZ) {
                grp = m;
                break;
            }
        }
        if(grp >= 0) {
            if(v > tv[grp]) { // stronger sample of a peak we already have
                tv[grp] = v;
                tb[grp] = i;
            }
            continue;
        }
        int mn = 0; // no group -> replace the weakest slot if this is stronger
        for(int m = 1; m < 4; m++)
            if(tv[m] < tv[mn]) mn = m;
        if(v > tv[mn]) {
            tv[mn] = v;
            tb[mn] = i;
        }
    }
    // sort the 4 descending (tiny)
    for(int a = 0; a < 4; a++)
        for(int b = a + 1; b < 4; b++)
            if(tv[b] > tv[a]) {
                int t = tv[a];
                tv[a] = tv[b];
                tv[b] = t;
                uint16_t u = tb[a];
                tb[a] = tb[b];
                tb[b] = u;
            }
}

// Numeric page: 4 full-width horizontal bars (precise top-4 bins). Bar length ~ strength;
// the "<freq> <dBm> ~<band>" label is drawn in XOR so it reads black on the empty part and
// white over the filled part. The cursor row (Up/Down) is bracketed; OK captures it.
static void draw_numeric(Canvas* canvas, App* app) {
    int tv[4];
    uint16_t tb[4];
    numeric_top4(app, tv, tb);
    int floor, ceil;
    autoscale(app, &floor, &ceil);
    const int pitch = 12, bh = 10;
    int nshown = 0;
    for(int k = 0; k < 4; k++) {
        if(tv[k] <= -200) continue; // empty slot -> draw nothing (no stray outline)
        nshown++;
        int y = 1 + k * pitch;
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
        // selected row (the one OK will capture): "<" pointer at the right end, no box
        if(k == app->num_sel)
            canvas_draw_str_aligned(canvas, SCR_W - 1, y + bh - 2, AlignRight, AlignBottom, "<");
        canvas_set_color(canvas, ColorBlack);
    }
    if(nshown == 0) canvas_draw_str(canvas, 2, 30, "no peaks above trigger");
}

// History page: the session's busiest 1 MHz channels (peaked above the trigger), sorted by
// hit count, tagged with the likely band. "which channels are active, and how often."
static void draw_history(Canvas* canvas, App* app) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, "Busy channels (xN)");
    // snapshot the worker-owned count once: if it grew mid-draw, a reread would index
    // uninitialized order[] entries and run off hist[].
    uint8_t n = app->hist_n;
    if(n > COUNT_OF(app->hist)) n = COUNT_OF(app->hist);
    if(n == 0) {
        canvas_draw_str(canvas, 2, 30, "none above trigger yet");
        return;
    }
    // selection-sort the top rows by count (<=8 entries, trivial)
    uint8_t order[8];
    for(uint8_t i = 0; i < n; i++)
        order[i] = i;
    for(uint8_t i = 0; i < n; i++)
        for(uint8_t j = i + 1; j < n; j++)
            if(app->hist[order[j]].count > app->hist[order[i]].count) {
                uint8_t t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
    int rows = n < 4 ? n : 4;
    for(int k = 0; k < rows; k++) {
        uint16_t mhz = app->hist[order[k]].mhz;
        uint16_t cnt = app->hist[order[k]].count;
        const FreqBand* b = band_lookup((uint32_t)mhz * 1000000);
        char s[40];
        snprintf(s, sizeof(s), "%u %s x%u", (unsigned)mhz, b ? b->name : "", (unsigned)cnt);
        canvas_draw_str(canvas, 2, 20 + k * 10, s);
    }
}

static void spectrum_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    App* app = g_app;
    if(!app) return; // defensive: never draw after teardown
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    if(app->page == 3) {
        draw_numeric(canvas, app); // bars show absolute freqs; the Capture pill is below
    } else if(app->page == 2) {
        draw_waterfall(canvas, app); // full-bleed, carries its own range + span labels
    } else if(app->page == 4) {
        draw_history(canvas, app);
    } else {
        // header: range + likely band name (+ window seconds on the connected page)
        uint32_t fc = app->f_start / 2 + app->f_end / 2;
        const FreqBand* hb = band_lookup(fc);
        char hdr[48];
        if(app->page == 1) {
            char w[8];
            fmt_win(WIN_SECS[app->win_idx], w, sizeof(w));
            snprintf(
                hdr,
                sizeof(hdr),
                "%lu-%lu %s %s",
                (unsigned long)(app->f_start / 1000000),
                (unsigned long)(app->f_end / 1000000),
                hb ? hb->name : "",
                w);
        } else
            snprintf(
                hdr,
                sizeof(hdr),
                "%lu-%lu %s",
                (unsigned long)(app->f_start / 1000000),
                (unsigned long)(app->f_end / 1000000),
                hb ? hb->name : "");
        canvas_draw_str(canvas, 2, 8, hdr);
        // info line: cursor readout on bars, global peak on connected
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
            draw_connected(canvas, app, floor, ceil);
    }

    // native pill hints (the waterfall draws its own scale + page hint instead)
    if(app->page != 2) {
        elements_button_left(canvas, "Page");
        if(app->page == 0) elements_button_center(canvas, "Zoom");
        if(app->page == 3) elements_button_center(canvas, "Capture"); // OK on the cursor row
        if(app->zdepth) elements_button_right(canvas, "Out");
    }

    // recording indicator (long-OK toggles): filled dot + row count, top-right.
    // XOR on the full-bleed pages (2/3) so it reads over lit/dark cells.
    if(app->recording) {
        bool fb = (app->page == 2 || app->page == 3);
        if(fb) canvas_set_color(canvas, ColorXOR);
        char s[16];
        snprintf(s, sizeof(s), "REC %lu", (unsigned long)app->rec_rows);
        int w = (int)canvas_string_width(canvas, s);
        canvas_draw_disc(canvas, SCR_W - w - 7, 4, 2);
        canvas_draw_str(canvas, SCR_W - w - 1, 8, s);
        if(fb) canvas_set_color(canvas, ColorBlack);
    }
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
            app->page = (uint8_t)((app->page + 4) % 5); // edge / long -> prev page
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyRight && (sp || lp)) {
        if(sp && app->page == 0 && app->cursor < NBARS - 1)
            app->cursor++;
        else
            app->page = (uint8_t)((app->page + 1) % 5); // edge / long -> next page
        spec_redraw(app);
        return true;
    }
    // Up/Down: move the capture cursor on the numeric page (only over filled rows, which are
    // contiguous from 0 since the list is sorted), else cycle the time window
    if((event->key == InputKeyUp || event->key == InputKeyDown) && sp && app->page == 3) {
        int tv[4];
        uint16_t tb[4];
        numeric_top4(app, tv, tb);
        int cnt = 0;
        while(cnt < 4 && tv[cnt] > -200)
            cnt++;
        if(cnt < 1) cnt = 1;
        int d = (event->key == InputKeyUp) ? cnt - 1 : 1;
        app->num_sel = (uint8_t)((app->num_sel + d) % cnt);
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyUp && sp) {
        if(app->win_idx < (uint8_t)(WIN_N - 1)) app->win_idx++;
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyDown && sp) {
        if(app->win_idx > 0) app->win_idx--;
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyOk && lp) {
        app->recording = !app->recording; // long-press toggles CSV logging (any page)
        FURI_LOG_I(TAG, "input: rec %s", app->recording ? "start" : "stop");
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyOk && sp && app->page == 0) {
        zoom_in(app, app->cursor); // short-press on bars: zoom into the framed band
        spec_redraw(app);
        return true;
    }
    if(event->key == InputKeyOk && sp && app->page == 3) {
        // numeric page: lock the SELECTED top-4 bin (Up/Down cursor) and open .sub capture.
        // Stop the survey worker first (from this input thread, never the draw cb) so the one
        // radio has a single owner.
        int tv[4];
        uint16_t tb[4];
        numeric_top4(app, tv, tb);
        if(tv[app->num_sel] <= -200) return true; // empty slot -> nothing to capture
        app->recording = false; // the survey worker will close any open CSV as it stops
        app->running = false;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
        app->cap_freq = app->f_start + (uint32_t)tb[app->num_sel] * app->f_step;
        app->cap_preset = app->preset_idx;
        app->capturing = false;
        app->cap_samples = 0;
        app->cap_overflow = 0;
        FURI_LOG_I(TAG, "capture: lock %lu Hz", (unsigned long)app->cap_freq);
        view_dispatcher_switch_to_view(app->vd, VIEW_CAP);
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

// ---- custom range editor (VIEW_RANGE) --------------------------------------
// Styled after the stock Frequency Analyzer: the active field (Start or End) is a big
// boxed number; Left/Right pick a digit, Up/Down change it, OK swaps Start<->End, Back
// validates and applies. Values are in 100 kHz units (779.0 MHz = 7790).

static const uint32_t RE_PLACE[4] = {1000, 100, 10, 1}; // hundreds, tens, ones, tenths

static void range_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    App* app = g_app;
    if(!app) return;
    canvas_clear(canvas);
    uint32_t a = app->re_field ? app->re_end : app->re_start;
    char big[12];
    snprintf(big, sizeof(big), "%lu.%lu", (unsigned long)(a / 10), (unsigned long)(a % 10));

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 9, app->re_field ? "End" : "Start");
    canvas_draw_str(canvas, 108, 9, "MHz");

    // inverted box with the big number (like the analyzer's locked readout)
    canvas_draw_box(canvas, 2, 12, 124, 22);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, 6, 30, big);
    // underline the active digit (digit idx -> char idx; the '.' is char 3)
    int ci = app->re_cur < 3 ? app->re_cur : 4;
    char pre[8];
    memcpy(pre, big, (size_t)ci);
    pre[ci] = 0;
    int x0 = 6 + (int)canvas_string_width(canvas, pre);
    char one[2] = {big[ci], 0};
    int w = (int)canvas_string_width(canvas, one);
    canvas_draw_line(canvas, x0, 32, x0 + (w > 0 ? w - 1 : 2), 32);
    canvas_set_color(canvas, ColorBlack);

    // the other field + the resulting span, below the box
    canvas_set_font(canvas, FontSecondary);
    uint32_t o = app->re_field ? app->re_start : app->re_end;
    uint32_t span = (app->re_end > app->re_start) ? app->re_end - app->re_start : 0;
    char info[40];
    snprintf(
        info,
        sizeof(info),
        "%s %lu.%lu  span %lu.%lu",
        app->re_field ? "Start" : "End",
        (unsigned long)(o / 10),
        (unsigned long)(o % 10),
        (unsigned long)(span / 10),
        (unsigned long)(span % 10));
    canvas_draw_str(canvas, 2, 46, info);

    elements_button_left(canvas, "Digit");
    elements_button_center(canvas, "Swap");
    elements_button_right(canvas, "Set");
}

static bool range_input(InputEvent* e, void* ctx) {
    UNUSED(ctx);
    App* app = g_app;
    if(!app) return false;
    bool act = (e->type == InputTypeShort || e->type == InputTypeRepeat);

    if(e->key == InputKeyBack && e->type == InputTypeShort) {
        // validate: clamp to the CC1101 window and enforce a minimum span, then apply
        if(app->re_start < RE_MIN) app->re_start = RE_MIN;
        if(app->re_end > RE_MAX) app->re_end = RE_MAX;
        if(app->re_end < app->re_start + RE_SPAN) {
            app->re_end = app->re_start + RE_SPAN;
            if(app->re_end > RE_MAX) {
                app->re_end = RE_MAX;
                app->re_start = RE_MAX - RE_SPAN;
            }
        }
        app->f_start = app->re_start * 100000;
        app->f_end = app->re_end * 100000;
        app->reconfig = true;
        char r[24];
        snprintf(
            r,
            sizeof(r),
            "%lu-%lu",
            (unsigned long)(app->f_start / 1000000),
            (unsigned long)(app->f_end / 1000000));
        variable_item_set_current_value_text(app->it_range, r);
        FURI_LOG_I(
            TAG, "range: set %lu-%lu", (unsigned long)app->f_start, (unsigned long)app->f_end);
        view_dispatcher_switch_to_view(app->vd, VIEW_CONF);
        return true;
    }

    uint32_t* v = app->re_field ? &app->re_end : &app->re_start;
    if(e->key == InputKeyOk && e->type == InputTypeShort) {
        app->re_field ^= 1; // swap Start <-> End
    } else if(e->key == InputKeyLeft && act) {
        if(app->re_cur > 0) app->re_cur--;
    } else if(e->key == InputKeyRight && act) {
        if(app->re_cur < 3) app->re_cur++;
    } else if(e->key == InputKeyUp && act) {
        uint32_t nv = *v + RE_PLACE[app->re_cur];
        *v = (nv > RE_MAX) ? RE_MAX : nv;
    } else if(e->key == InputKeyDown && act) {
        uint32_t p = RE_PLACE[app->re_cur];
        *v = (*v > RE_MIN + p) ? (*v - p) : RE_MIN;
    } else {
        return true;
    }
    with_view_model(app->range_view, void** m, { UNUSED(m); }, true);
    return true;
}

// ---- .sub capture (VIEW_CAP) -----------------------------------------------
// Re-implements the stock RAW recorder: start_async_rx streams (level,duration) edges from an
// ISR. The ISR drops <50 us noise and >=1 s gaps (the latter also keeps the signed int32
// packing from overflowing) and pushes the rest to a stream buffer; the capture thread writes
// RAW_Data rows to a .sub the stock SubGHz app can replay. The survey worker is fully stopped
// first so only one owner touches the radio. (Dropped edges are not merged -- fine for clean
// OOK/FSK bursts; very noisy inputs may not replay perfectly.)

#define CAP_STREAM 4096 // int32 slots ISR -> thread (sized for a burst; overflow is counted)
#define CAP_MAXSPL 60000 // auto-stop after this many durations (bounds the file)

// ISR context: pack the edge and push it; never block, never allocate, never touch the file.
static void capture_isr_cb(bool level, uint32_t duration, void* ctx) {
    App* app = ctx;
    FuriStreamBuffer* sb = app->cap_stream;
    if(!sb) return;
    if(duration < 50 || duration >= 1000000) return; // noise / huge gap -> drop (and no overflow)
    int32_t d = level ? (int32_t)duration : -(int32_t)duration;
    if(furi_stream_buffer_send(sb, &d, sizeof(d), 0) != sizeof(d)) app->cap_overflow++;
}

static int32_t capture_worker(void* ctx) {
    App* app = ctx;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    const SurveyPreset* ps = &PRESETS[app->cap_preset < PRESET_N ? app->cap_preset : 0];

    storage_common_mkdir(storage, "/ext/subghz");
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[96];
    snprintf(
        path,
        sizeof(path),
        "/ext/subghz/rfsurvey_%lu_%02u%02u%02u_%03lu.sub",
        (unsigned long)(app->cap_freq / 1000),
        dt.hour,
        dt.minute,
        dt.second,
        (unsigned long)(furi_get_tick() % 1000)); // tick suffix: never overwrite a prior .sub

    FlipperFormat* ff = flipper_format_file_alloc(storage);
    int32_t* buf = malloc(sizeof(int32_t) * 512);
    app->cap_stream = furi_stream_buffer_alloc(sizeof(int32_t) * CAP_STREAM, sizeof(int32_t));
    bool ok = buf && app->cap_stream && flipper_format_file_open_always(ff, path);
    if(ok) {
        uint32_t ver = 1, freq = app->cap_freq;
        ok = flipper_format_write_header_cstr(ff, "Flipper SubGhz RAW File", ver) &&
             flipper_format_write_uint32(ff, "Frequency", &freq, 1) &&
             flipper_format_write_string_cstr(ff, "Preset", ps->sub) &&
             flipper_format_write_string_cstr(ff, "Protocol", "RAW");
    }
    if(!ok) {
        FURI_LOG_E(TAG, "cap: open/header failed %s", path);
        app->capturing = false;
    } else {
        FURI_LOG_I(
            TAG, "cap: recording %lu Hz %s -> %s", (unsigned long)app->cap_freq, ps->name, path);
        furi_hal_subghz_reset();
        furi_hal_subghz_load_custom_preset(ps->regs);
        furi_hal_subghz_idle();
        furi_hal_subghz_set_frequency(app->cap_freq);
        if(app->cap_freq >= 299999755 && app->cap_freq <= 348000335)
            furi_hal_subghz_set_path(FuriHalSubGhzPath315);
        else if(app->cap_freq >= 386999938 && app->cap_freq <= 464000000)
            furi_hal_subghz_set_path(FuriHalSubGhzPath433);
        else
            furi_hal_subghz_set_path(FuriHalSubGhzPath868);
        furi_hal_subghz_start_async_rx(capture_isr_cb, app);

        int ind = 0;
        bool werr = false;
        uint32_t last_poll = 0;
        app->cap_paused = app->cap_gate; // if gating, wait for signal before the first write
        while(app->capturing && !werr && app->cap_samples < CAP_MAXSPL) {
            // poll the level ~50 Hz for the on-screen readout; only PAUSE writing when the gate
            // is on and we're below the Trigger (off by default -> record everything).
            uint32_t now = furi_get_tick();
            if(now - last_poll >= 20) {
                float r = furi_hal_subghz_get_rssi();
                app->cap_rssi = (r < -127.0f) ? (int8_t)-127 : (int8_t)r;
                app->cap_paused = app->cap_gate && (app->cap_rssi < app->trigger);
                last_poll = now;
            }
            int32_t d;
            if(furi_stream_buffer_receive(app->cap_stream, &d, sizeof(d), 20) == sizeof(d)) {
                if(app->cap_paused) continue; // below trigger -> drain but don't record
                buf[ind++] = d;
                app->cap_samples++;
                if(ind == 512) {
                    werr = !flipper_format_write_int32(ff, "RAW_Data", buf, 512);
                    ind = 0;
                }
            }
        }
        // teardown order is non-negotiable: stop the ISR FIRST so no callback can fire into
        // the stream after we free it; then drain the tail, flush, close, free.
        furi_hal_subghz_stop_async_rx();
        int32_t d;
        while(furi_stream_buffer_receive(app->cap_stream, &d, sizeof(d), 0) == sizeof(d)) {
            buf[ind++] = d;
            app->cap_samples++;
            if(ind == 512) { // keep draining the tail instead of dropping it at 512
                flipper_format_write_int32(ff, "RAW_Data", buf, 512);
                ind = 0;
            }
        }
        if(ind > 0) flipper_format_write_int32(ff, "RAW_Data", buf, (uint16_t)ind);
        furi_hal_subghz_idle();
        furi_hal_subghz_sleep();
        FURI_LOG_I(
            TAG,
            "cap: stop, %lu samples, %lu overflow",
            (unsigned long)app->cap_samples,
            (unsigned long)app->cap_overflow);
    }

    // stop_async_rx already ran in the success path (before any free); if the file never
    // opened, nothing was started, so there is nothing to stop here. Free in the safe order.
    if(ff) flipper_format_free(ff);
    if(buf) free(buf);
    if(app->cap_stream) {
        furi_stream_buffer_free(app->cap_stream);
        app->cap_stream = NULL;
    }
    furi_record_close(RECORD_STORAGE);
    app->capturing = false;
    return 0;
}

static void capture_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    App* app = g_app;
    if(!app) return;
    canvas_clear(canvas);
    const SurveyPreset* ps = &PRESETS[app->cap_preset < PRESET_N ? app->cap_preset : 0];

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 9, "Capture .sub");
    canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom, ps->name); // preset, right

    canvas_draw_box(canvas, 2, 12, 124, 22);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontBigNumbers);
    char big[12];
    snprintf(
        big,
        sizeof(big),
        "%lu.%03lu",
        (unsigned long)(app->cap_freq / 1000000),
        (unsigned long)((app->cap_freq / 1000) % 1000));
    canvas_draw_str(canvas, 6, 30, big);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 104, 30, "MHz"); // small, inside the box next to the number
    canvas_set_color(canvas, ColorBlack);

    // one status line above the pills (never over them)
    canvas_set_font(canvas, FontSecondary);
    char line[40];
    if(app->capturing) {
        if(app->cap_gate && app->cap_paused)
            snprintf(line, sizeof(line), "signal below range (%d)", app->cap_rssi);
        else
            snprintf(
                line,
                sizeof(line),
                "REC  %lu spl%s  %d dBm",
                (unsigned long)app->cap_samples,
                app->cap_overflow ? "!" : "",
                app->cap_rssi);
    } else {
        snprintf(line, sizeof(line), "RSSI filter: %s  (Up/Dn)", app->cap_gate ? "on" : "off");
    }
    canvas_draw_str(canvas, 2, 45, line);

    if(app->capturing) {
        elements_button_center(canvas, "Stop");
    } else {
        elements_button_left(canvas, "Mod");
        elements_button_center(canvas, "Rec");
        elements_button_right(canvas, "Mod");
    }
}

// Join + free the capture thread handle. Safe whether the thread is still running (sets
// capturing=false to ask it to stop, then waits) or already self-finished (auto-stop / open
// or write failure left the handle unreaped). Called before every start so there is never a
// second worker racing the old one's stream free.
static void capture_stop(App* app) {
    if(!app->cap_worker) return;
    app->capturing = false;
    furi_thread_join(app->cap_worker); // returns at once if the thread already exited
    furi_thread_free(app->cap_worker);
    app->cap_worker = NULL;
}

static void capture_start(App* app) {
    capture_stop(app); // reap any prior worker (running or self-finished) before a new one
    app->capturing = true;
    app->cap_samples = 0;
    app->cap_overflow = 0;
    app->cap_worker = furi_thread_alloc_ex("rfsurvey_cap", 3072, capture_worker, app);
    furi_thread_start(app->cap_worker);
}

static bool capture_input(InputEvent* e, void* ctx) {
    UNUSED(ctx);
    App* app = g_app;
    if(!app) return false;
    if(e->type != InputTypeShort) return true;

    if(e->key == InputKeyBack) {
        capture_stop(app);
        // restart a fresh survey worker (re-inits the radio exactly like first boot)
        app->running = true;
        app->reconfig = true;
        app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 3072, sweep_worker, app);
        furi_thread_start(app->worker);
        view_dispatcher_switch_to_view(app->vd, VIEW_SPEC);
        return true;
    }
    if(e->key == InputKeyOk) {
        if(app->capturing)
            capture_stop(app);
        else
            capture_start(app);
    } else if((e->key == InputKeyLeft || e->key == InputKeyRight) && !app->capturing) {
        // toggle the capture modulation (OOK vs 2FSK) -- the user must match the signal
        if(e->key == InputKeyRight)
            app->cap_preset = (uint8_t)((app->cap_preset + 1) % PRESET_N);
        else
            app->cap_preset = (uint8_t)((app->cap_preset + PRESET_N - 1) % PRESET_N);
    } else if(e->key == InputKeyUp || e->key == InputKeyDown) {
        app->cap_gate = !app->cap_gate; // RSSI gate on/off (off = record everything)
    }
    with_view_model(app->cap_view, void** m, { UNUSED(m); }, true);
    return true;
}

// ---- config screen ---------------------------------------------------------

static void band_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    const FreqBand* b = preset_at(idx);
    variable_item_set_current_value_text(item, b->name);
    app->f_start = b->lo;
    app->f_end = b->hi;
    app->reconfig = true;
    FURI_LOG_I(
        TAG,
        "config: band[%u]=%s %lu-%lu",
        idx,
        b->name,
        (unsigned long)b->lo,
        (unsigned long)b->hi);
    // reflect the chosen band into the range readout
    char r[24];
    snprintf(
        r,
        sizeof(r),
        "%lu-%lu",
        (unsigned long)(b->lo / 1000000),
        (unsigned long)(b->hi / 1000000));
    variable_item_set_current_value_text(app->it_range, r);
    // suggest a matching preset from the band's modulation (AM bands -> AM650, FM -> FM238).
    // Only a default: the user can still override the Modulation item afterwards.
    uint8_t def = (b->mod == ModFM) ? 2 : 0;
    app->preset_idx = def;
    variable_item_set_current_value_index(app->it_preset, def);
    variable_item_set_current_value_text(app->it_preset, PRESETS[def].name);
}

static void preset_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= PRESET_N) idx = PRESET_N - 1;
    app->preset_idx = idx; // worker reloads the CC1101 next loop
    variable_item_set_current_value_text(item, PRESETS[idx].name);
    app->reconfig = true; // changed measurement -> clear stale readings
}

static void step_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= COUNT_OF(STEP_HZ)) idx = COUNT_OF(STEP_HZ) - 1;
    app->f_step = STEP_HZ[idx];
    variable_item_set_current_value_text(item, STEP_TXT[idx]);
    app->reconfig = true;
}

// Trigger floor: -100..-45 dBm in 5 dB steps (12 values). Separates weak from strong for
// the waterfall, haptic, history and (later) capture gating.
#define TRIG_N   12
#define TRIG_MIN -100
static void trigger_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->trigger = (int8_t)(TRIG_MIN + (int)variable_item_get_current_value_index(item) * 5);
    char t[8];
    snprintf(t, sizeof(t), "%d", app->trigger);
    variable_item_set_current_value_text(item, t);
}

static void haptic_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->haptic = variable_item_get_current_value_index(item) ? true : false;
    variable_item_set_current_value_text(item, app->haptic ? "On" : "Off");
}

static const char* const GAIN_TXT[] = {"Auto", "Mid", "Low"};
static void gain_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= COUNT_OF(GAIN_TXT)) idx = 0;
    app->gain = idx; // worker reloads the preset with patched AGC next loop
    variable_item_set_current_value_text(item, GAIN_TXT[idx]);
}

static void conf_enter(void* ctx, uint32_t index) {
    App* app = ctx;
    if(index == 1) { // "Range MHz" row -> open the custom range editor
        app->re_start = app->f_start / 100000;
        app->re_end = app->f_end / 100000;
        if(app->re_start < RE_MIN) app->re_start = RE_MIN;
        if(app->re_end > RE_MAX) app->re_end = RE_MAX;
        app->re_field = 0;
        app->re_cur = 0;
        FURI_LOG_I(TAG, "config: open range editor");
        view_dispatcher_switch_to_view(app->vd, VIEW_RANGE);
        return;
    }
    FURI_LOG_I(TAG, "config: OK -> spectrum");
    view_dispatcher_switch_to_view(app->vd, VIEW_SPEC); // OK on any other row starts the scan
}

static void redraw_cb(void* ctx) {
    App* app = ctx;
    // commit the (empty) models to trigger a redraw; only the active view actually draws, so
    // committing both the spectrum and the capture view is cheap and keeps whichever is up live
    with_view_model(app->view, void** m, { UNUSED(m); }, true);
    if(app->cap_view) with_view_model(app->cap_view, void** m, { UNUSED(m); }, true);
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
    app->preset_idx = 0; // AM650: widest OOK filter, best broadband survey pickup
    app->cursor = NBARS / 2;
    app->win_idx = WIN_DEFAULT; // 60 s
    app->trigger = -85; // default floor: above the CC1101 noise floor, below real signals
    app->haptic = false;
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

    app->notif = furi_record_open(RECORD_NOTIFICATION);
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
    // known allocation) -> fills range; also Range readout, Modulation preset and Step. OK scans.
    app->conf = variable_item_list_alloc();
    VariableItem* vi = variable_item_list_add(app->conf, "Band", PRESET_COUNT, band_cb, app);
    variable_item_set_current_value_index(vi, 3); // default "Full high" (779-928)
    variable_item_set_current_value_text(vi, preset_at(3)->name);
    app->it_range = variable_item_list_add(app->conf, "Range MHz", 1, NULL, app);
    variable_item_set_current_value_text(app->it_range, "779-928");
    app->it_preset = variable_item_list_add(app->conf, "Modulation", PRESET_N, preset_cb, app);
    variable_item_set_current_value_index(app->it_preset, app->preset_idx);
    variable_item_set_current_value_text(app->it_preset, PRESETS[app->preset_idx].name);
    vi = variable_item_list_add(app->conf, "Step", COUNT_OF(STEP_HZ), step_cb, app);
    variable_item_set_current_value_index(vi, 4); // 650k
    variable_item_set_current_value_text(vi, "650k");
    vi = variable_item_list_add(app->conf, "Trigger", TRIG_N, trigger_cb, app);
    variable_item_set_current_value_index(vi, (uint8_t)((app->trigger - TRIG_MIN) / 5));
    {
        char t[8];
        snprintf(t, sizeof(t), "%d", app->trigger);
        variable_item_set_current_value_text(vi, t);
    }
    vi = variable_item_list_add(app->conf, "Haptic", 2, haptic_cb, app);
    variable_item_set_current_value_index(vi, app->haptic ? 1 : 0);
    variable_item_set_current_value_text(vi, app->haptic ? "On" : "Off");
    vi = variable_item_list_add(app->conf, "Gain", COUNT_OF(GAIN_TXT), gain_cb, app);
    variable_item_set_current_value_index(vi, app->gain);
    variable_item_set_current_value_text(vi, GAIN_TXT[app->gain]);
    variable_item_list_set_enter_callback(app->conf, conf_enter, app);
    view_set_previous_callback(variable_item_list_get_view(app->conf), view_exit);
    view_dispatcher_add_view(app->vd, VIEW_CONF, variable_item_list_get_view(app->conf));

    // custom range editor view (Back returns to config)
    app->range_view = view_alloc();
    view_allocate_model(app->range_view, ViewModelTypeLockFree, sizeof(void*));
    view_set_context(app->range_view, app);
    view_set_draw_callback(app->range_view, range_draw);
    view_set_input_callback(app->range_view, range_input);
    view_set_previous_callback(app->range_view, to_conf);
    view_dispatcher_add_view(app->vd, VIEW_RANGE, app->range_view);

    // .sub capture view (Back is handled in its input callback: stop + restart the survey)
    app->cap_view = view_alloc();
    view_allocate_model(app->cap_view, ViewModelTypeLockFree, sizeof(void*));
    view_set_context(app->cap_view, app);
    view_set_draw_callback(app->cap_view, capture_draw);
    view_set_input_callback(app->cap_view, capture_input);
    view_dispatcher_add_view(app->vd, VIEW_CAP, app->cap_view);

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
    capture_stop(app); // join a capture thread if one is somehow still running
    app->running = false;
    if(app->worker) { // NULL only if we somehow exit mid-capture; normally restarted on Back
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
    }
    furi_hal_region_set((FuriHalRegion*)orig_region); // restore the user's region

    view_dispatcher_remove_view(app->vd, VIEW_SPEC);
    view_dispatcher_remove_view(app->vd, VIEW_CONF);
    view_dispatcher_remove_view(app->vd, VIEW_RANGE);
    view_dispatcher_remove_view(app->vd, VIEW_CAP);
    view_free(app->view);
    view_free(app->range_view);
    view_free(app->cap_view);
    variable_item_list_free(app->conf);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    g_app = NULL; // defensive: a stray draw after this sees NULL, not freed memory
    free(app);
    FURI_LOG_I(TAG, "app: end");
    return 0;
}
