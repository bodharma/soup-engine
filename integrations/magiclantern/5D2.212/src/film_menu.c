/* Native PLAY updates start disabled; immutable originals are backed up first. */
#include "dryos.h"
#include "config.h"
#include "fio-ml.h"
#include "menu.h"
#include "notify_box.h"
#include "film_transaction.h"

#define SOUP_STOCKS 16
#define SOUP_PATH 96

static int soup_on;
static int soup_recovery_ready;
static CONFIG_INT("soup.stock", soup_stock, -1);
static int soup_count;
static char soup_files[SOUP_STOCKS][64];
static char soup_names[SOUP_STOCKS][32];
static const char *soup_choices[SOUP_STOCKS];

int soup_is_on(void) { return soup_on && soup_recovery_ready && soup_count > 0; }
void soup_disable_for_recovery(void) { soup_on = 0; soup_recovery_ready = 0; }

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
        .help = "Update PLAY after processing. Originals backed up in ML/SOUP.",
        .children = (struct menu_entry[]) {
            {
                .name = "Enable",
                .priv = &soup_on,
                .max = 1,
                .choices = CHOICES("OFF", "ON"),
                .help = "Back up originals, then update JPEG/RAW playback previews.",
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
        soup_recovery_ready = recovery >= 0;
        if (recovery < 0) NotifyBox(10000, "Soup recovery needs attention; processing disabled");
        else if (recovery > 0) NotifyBox(8000, "Soup restored originals after an incomplete update");
    }
    soup_catalog();
    if (!soup_count) {
        NotifyBox(5000, "Soup: no ML/FILM stocks found");
        return;
    }
    if (soup_stock < 0 || soup_stock >= soup_count) soup_stock = 0;
    soup_menu[0].children[1].max = soup_count - 1;
    menu_add("Shoot", soup_menu, COUNT(soup_menu));
    task_create("soup_worker", 0x1f, 0x10000, soup_worker, 0);
}

TASK_CREATE("soup_init", soup_init, 0, 0x1f, 0x10000);
