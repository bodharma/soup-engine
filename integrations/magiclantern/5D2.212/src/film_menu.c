/* Native PLAY updates start disabled; temporary verified backups protect publication. */
#include "dryos.h"
#include "config.h"
#include "fio-ml.h"
#include "menu.h"
#include "notify_box.h"
#include "film_transaction.h"
#include "lens.h"
#include "property.h"
#include "propvalues.h"
#include "gui-common.h"
#include "debug.h"
#include "fileprefix.h"
#include "arm-mcr.h"

#define SOUP_STOCKS 16
#define SOUP_PATH 96
#define SOUP_STOCK_ENTRY 4

static int soup_on;
static int soup_recovery_ready;
static CONFIG_INT("soup.stock", soup_stock, -1);
static CONFIG_INT("soup.mode", soup_mode, SOUP_MODE_EFFECTS);
static CONFIG_INT("soup.grain", soup_grain, 20);
static CONFIG_INT("soup.grain.size", soup_grain_size, 2);
static CONFIG_INT("soup.halation", soup_halation, 15);
static CONFIG_INT("soup.halation.radius", soup_hal_radius, 3);
static CONFIG_INT("soup.bloom", soup_bloom, 8);
static CONFIG_INT("soup.bloom.radius", soup_bloom_radius, 6);
static int soup_count;
static char soup_files[SOUP_STOCKS][64];
static char soup_names[SOUP_STOCKS][32];
static const char *soup_choices[SOUP_STOCKS];

int soup_is_on(void) { return soup_on && soup_recovery_ready && (soup_mode == SOUP_MODE_EFFECTS || soup_count > 0); }
void soup_disable_for_recovery(void) { soup_capture_disable(); soup_on = 0; soup_recovery_ready = 0; }

void soup_get_settings(soup_settings *settings)
{
    uint32_t irq = cli();
    __asm__ __volatile__("" ::: "memory");
    *settings = (soup_settings){soup_mode, soup_grain, soup_grain_size, soup_halation,
                              soup_hal_radius, soup_bloom, soup_bloom_radius};
    __asm__ __volatile__("" ::: "memory");
    sei(irq);
}

/* Snapshot before publishing ON; the first shutter press cannot become the baseline. */
static int soup_prepare_enable(int monochrome)
{
    struct card_info *card;
    int result = -1, acquired = 0;
    if (!soup_recovery_ready || soup_develop_busy() || lens_info.job_state ||
        get_halfshutter_pressed() || RECORDING || (icu_uilock & 0xffff)) return -1;
    gui_uilock(UILOCK_SHUTTER);
    acquired = !!(icu_uilock & UILOCK_SHUTTER);
    if (!acquired || lens_info.job_state || get_halfshutter_pressed() || RECORDING) goto done;
    if (monochrome) {
        /* Canon's raw Monochrome value; independent of ML Auto/no-Auto menu indices. */
        int style = 0x86;
        if (!prop_request_change_wait(PROP_PICTURE_STYLE, &style, 4, 1000) ||
            lens_info.raw_picstyle != 0x86) goto done;
    }
    if (lens_info.job_state || get_halfshutter_pressed() || RECORDING) goto done;
    card = get_shooting_card();
    if (card) result = soup_capture_arm(get_dcim_dir(), get_file_prefix(), card->file_number);
done:
    if (acquired) gui_uilock((icu_uilock & 0xffff) & ~UILOCK_SHUTTER);
    return result;
}

static MENU_SELECT_FUNC(soup_enable_select)
{
    (void)priv; (void)delta;
    if (soup_on) { soup_capture_disable(); soup_on = 0; return; }
    if (soup_mode == SOUP_MODE_FILM && !soup_count) { NotifyBox(5000, "Soup: no film stocks loaded"); return; }
    if (soup_prepare_enable(0)) { NotifyBox(5000, "Soup not ready: release shutter and wait for idle"); return; }
    soup_on = 1;
}

static MENU_SELECT_FUNC(soup_mode_select)
{
    (void)priv; (void)delta;
    if (soup_on) { NotifyBox(5000, "Turn Soup OFF before changing Mode"); return; }
    soup_capture_disable();
    soup_mode = soup_mode == SOUP_MODE_EFFECTS ? SOUP_MODE_FILM : SOUP_MODE_EFFECTS;
}

static MENU_SELECT_FUNC(soup_bw_test)
{
    (void)priv; (void)delta;
    if (soup_on) { NotifyBox(5000, "Turn Soup OFF before selecting B&W test"); return; }
    /* Complete settings before arm publishes a claimable capture generation. */
    soup_mode = SOUP_MODE_EFFECTS;
    soup_grain = 20; soup_grain_size = 2;
    soup_halation = 15; soup_hal_radius = 3; soup_bloom = 8; soup_bloom_radius = 6;
    if (soup_prepare_enable(1)) { NotifyBox(5000, "B&W not ready: idle camera/style acknowledgement required"); return; }
    soup_on = 1;
    NotifyBox(5000, "B&W effects ready: native Monochrome, Soup ON");
}

int soup_stock_path(char *path, int size)
{
    int n = soup_stock;
    if (n < 0 || n >= soup_count) return -1;
    return snprintf(path, size, "%s", soup_files[n]) < size ? 0 : -1;
}

static void soup_catalog(void)
{
    char *text = fio_malloc(4096);
    int got, i, start = 0;
    if (!text) return;
    got = (int)read_file("ML/FILM/INDEX.TXT", text, 4095);
    if (got <= 0 || got >= 4095) { fio_free(text); return; }
    text[got] = 0;
    for (i = 0; i <= got && soup_count < SOUP_STOCKS; i++) {
        int end, k, valid = 1;
        if (text[i] && text[i] != '\n' && text[i] != '\r') continue;
        text[i] = 0;
        end = i;
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
        text[end] = 0;
        if (end - start < 4 || end - start > 31 || text[start] == '#') {
            start = i + 1; continue;
        }
        for (k = start; k < end; k++) {
            char c = text[k];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) valid = 0;
        }
        if (!valid || strstr(text + start, "..") || strcmp(text + end - 3, ".fs")) {
            start = i + 1; continue;
        }
        snprintf(soup_files[soup_count], 64, "ML/FILM/%s", text + start);
        text[end - 3] = 0;
        snprintf(soup_names[soup_count], 32, "%s", text + start);
        soup_choices[soup_count] = soup_names[soup_count];
        soup_count++;
        start = i + 1;
    }
    fio_free(text);
    if (soup_stock < 0) {
        for (i = 0; i < soup_count; i++)
            if (!strcmp(soup_names[i], "classicchrome")) { soup_stock = i; break; }
    }
}

static MENU_UPDATE_FUNC(soup_status_display)
{
    MENU_SET_VALUE("%s", soup_status_text());
}

static struct menu_entry soup_menu[] = {
    {
        .name = "Soup Film",
        .select = menu_open_submenu,
        .help = "Update PLAY. Temporary originals are removed after verified success.",
        .children = (struct menu_entry[]) {
            {
                .name = "Enable",
                .select = soup_enable_select,
                .priv = &soup_on,
                .max = 1,
                .choices = CHOICES("OFF", "ON"),
                .help = "Arm capture cursor; preserve native Picture Style. Backups are temporary.",
            },
            {
                .name = "Mode", .priv = &soup_mode, .select = soup_mode_select, .max = 1,
                .choices = CHOICES("Effects only", "Film stock"),
                .help = "Effects preserve Canon colour/tone. Film stock also applies its grade.",
            },
            {
                .name = "B&W test", .select = soup_bw_test,
                .help = "Select native Monochrome and restrained effects, then arm Soup ON.",
            },
            {
                .name = "Effects", .select = menu_open_submenu,
                .help = "Neutral spatial effects; sizes are pixels in the output JPEG.",
                .children = (struct menu_entry[]) {
                    {.name = "Grain", .priv = &soup_grain, .max = 100, .unit = UNIT_DEC,
                     .help = "Grain strength 0..100; zero disables grain."},
                    {.name = "Grain size", .priv = &soup_grain_size, .min = 1, .max = 5, .unit = UNIT_DEC,
                     .help = "Grain cell size in output pixels, 1..5."},
                    {.name = "Halation", .priv = &soup_halation, .max = 100, .unit = UNIT_DEC,
                     .help = "Neutral highlight halo strength 0..100; zero disables it."},
                    {.name = "Halo radius", .priv = &soup_hal_radius, .min = 1, .max = 8, .unit = UNIT_DEC,
                     .help = "Halo radius in output pixels, 1..8."},
                    {.name = "Bloom", .priv = &soup_bloom, .max = 30, .unit = UNIT_DEC,
                     .help = "Restrained neutral bloom strength 0..30; zero disables it."},
                    {.name = "Bloom radius", .priv = &soup_bloom_radius, .min = 1, .max = 16, .unit = UNIT_DEC,
                     .help = "Bloom radius in output pixels, 1..16."},
                    MENU_EOL,
                },
            },
            {
                .name = "Stock",
                .priv = &soup_stock,
                .choices = soup_choices,
                .help = "Looks from ML/FILM/INDEX.TXT. Use sRGB colour photos.",
            },
            {
                .name = "Status",
                .update = soup_status_display,
                .help = "Last processing phase/return code. Details: ML/SOUP/DIAG.TXT.",
            },
            MENU_EOL,
        },
    },
};

void soup_worker(void *unused);

static void soup_init(void *unused)
{
    extern int ml_started;
    (void)unused;
    while (!ml_started) msleep(100);
    msleep(2000);
    {
        int recovery;
        do {
            recovery = soup_recover_pending();
            if (recovery && recovery != SOUP_RECOVERY_BUSY) {
                if (recovery < 0) soup_diag_fail("boot recovery", recovery);
                else soup_diag_phase("boot recovery", recovery);
                soup_diag_flush();
            }
            if (recovery == SOUP_RECOVERY_BUSY) msleep(250);
        } while (recovery == SOUP_RECOVERY_BUSY);
        soup_recovery_ready = recovery >= 0 && recovery != SOUP_CLEANUP_PENDING;
        if (recovery < 0) NotifyBox(10000, "Soup recovery needs attention; processing disabled");
        else if (recovery == SOUP_CLEANUP_PENDING) NotifyBox(10000, "Soup applied; cleanup pending. Processing disabled");
        else if (recovery == SOUP_CLEANUP_DONE) NotifyBox(8000, "Soup completed temporary-file cleanup");
        else if (recovery > 0) NotifyBox(8000, "Soup restored originals after an incomplete update");
    }
    soup_catalog();
    if (!soup_count) {
        static const char *empty[] = {"Unavailable"};
        soup_stock = 0;
        soup_menu[0].children[SOUP_STOCK_ENTRY].choices = empty;
    }
    if (soup_stock < 0 || soup_stock >= soup_count) soup_stock = 0;
    soup_menu[0].children[SOUP_STOCK_ENTRY].max = soup_count ? soup_count - 1 : 0;
    menu_add("Shoot", soup_menu, COUNT(soup_menu));
    task_create("soup_worker", 0x1f, 0x10000, soup_worker, 0);
}

TASK_CREATE("soup_init", soup_init, 0, 0x1f, 0x10000);
