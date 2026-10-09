#include <furi.h>
#include <furi_hal_subghz.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <gui/elements.h>

// CC1101 preset register tables are exported to apps; AM650 = wide OOK, good for a
// broadband RSSI survey. (Same names the stock Spectrum Analyzer / SubGHz use.)
extern const uint8_t subghz_device_cc1101_preset_ook_650khz_async_regs[];

#define MAX_BINS 512
#define SCR_W    128
#define BASE_Y   54 // spectrum baseline
#define MAX_H    40 // tallest bar
#define RSSI_FLOOR -100.0f
#define RSSI_CEIL  -40.0f

typedef struct {
    // scan config
    uint32_t f_start;
    uint32_t f_end;
    uint32_t f_step;
    uint16_t nbins;
    uint8_t settle_ms;

    // scan data (worker writes, GUI reads; display tolerates minor tears)
    float rssi[MAX_BINS];
    float peak[MAX_BINS];
    uint32_t sweeps;

    FuriThread* worker;
    volatile bool running;

    Gui* gui;
    ViewDispatcher* vd;
    View* view;
    FuriTimer* redraw;
} App;

static App* g_app;

// ---- radio sweep worker ----------------------------------------------------

static int32_t sweep_worker(void* ctx) {
    App* app = ctx;
    furi_hal_subghz_reset();
    furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);

    while(app->running) {
        for(uint16_t i = 0; i < app->nbins && app->running; i++) {
            uint32_t f = app->f_start + (uint32_t)i * app->f_step;
            if(!furi_hal_subghz_is_frequency_valid(f)) {
                app->rssi[i] = -128.0f;
                continue;
            }
            furi_hal_subghz_idle();
            furi_hal_subghz_set_frequency_and_path(f); // picks the matching RF path per band
            furi_hal_subghz_rx();
            furi_delay_ms(app->settle_ms);
            float r = furi_hal_subghz_get_rssi();
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

static int bar_h(float dbm) {
    if(dbm < RSSI_FLOOR) dbm = RSSI_FLOOR;
    if(dbm > RSSI_CEIL) dbm = RSSI_CEIL;
    return (int)((dbm - RSSI_FLOOR) / (RSSI_CEIL - RSSI_FLOOR) * MAX_H);
}

static void bars_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    App* app = g_app;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    // find the current peak bin
    float pmax = -200.0f;
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
        hdr, sizeof(hdr), "%lu-%lu  sw%lu", app->f_start / 1000000, app->f_end / 1000000,
        app->sweeps);
    canvas_draw_str(canvas, 2, 8, hdr);

    char pk[40];
    snprintf(pk, sizeof(pk), "%lu.%lu MHz  %d dBm", pfreq / 1000000, (pfreq / 100000) % 10,
             (int)pmax);
    canvas_draw_str(canvas, 2, 18, pk);

    // bars: map each display column to a bin, show current + peak-hold dot
    for(int px = 0; px < SCR_W; px++) {
        uint16_t bin = (uint16_t)((uint32_t)px * app->nbins / SCR_W);
        int h = bar_h(app->rssi[bin]);
        if(h > 0) canvas_draw_line(canvas, px, BASE_Y, px, BASE_Y - h);
        int ph = bar_h(app->peak[bin]);
        if(ph > 0) canvas_draw_dot(canvas, px, BASE_Y - ph);
    }
    canvas_draw_line(canvas, 0, BASE_Y + 1, SCR_W - 1, BASE_Y + 1);

    // frequency axis (start / end)
    char fa[12];
    snprintf(fa, sizeof(fa), "%lu", app->f_start / 1000000);
    canvas_draw_str(canvas, 2, 63, fa);
    snprintf(fa, sizeof(fa), "%lu", app->f_end / 1000000);
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
    app->nbins = (uint16_t)((app->f_end - app->f_start) / app->f_step + 1);
    if(app->nbins > MAX_BINS) app->nbins = MAX_BINS;
    for(uint16_t i = 0; i < app->nbins; i++) {
        app->rssi[i] = -128.0f;
        app->peak[i] = -200.0f;
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->vd = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(void*));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, bars_draw);
    view_set_input_callback(app->view, bars_input);
    view_set_previous_callback(app->view, view_exit);
    view_dispatcher_add_view(app->vd, 0, app->view);

    app->running = true;
    app->worker = furi_thread_alloc_ex("rfsurvey_sweep", 2048, sweep_worker, app);
    furi_thread_start(app->worker);

    app->redraw = furi_timer_alloc(redraw_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->redraw, 150);

    view_dispatcher_switch_to_view(app->vd, 0);
    view_dispatcher_run(app->vd);

    furi_timer_stop(app->redraw);
    furi_timer_free(app->redraw);
    app->running = false;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    view_dispatcher_remove_view(app->vd, 0);
    view_free(app->view);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
