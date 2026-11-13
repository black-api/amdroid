/* The list of apps. To add one: write apps/<name>.c with an app_t, declare it here
 * and add it to the table. The order here is the order of the keys 1, 2, 3, ...
 * (key 0 opens the Terminal). */
#include "amdroid/ui.h"

extern const app_t app_files;
extern const app_t app_tasks;
extern const app_t app_settings;
extern const app_t app_about;
extern const app_t app_calc;
extern const app_t app_clock;
extern const app_t app_notes;
extern const app_t app_network;
extern const app_t app_browser;
extern const app_t app_store;

const app_t *const ui_apps[UI_MAX_APPS + 1] = {
    &app_files,
    &app_tasks,
    &app_store,
    &app_settings,
    &app_about,
    &app_calc,
    &app_clock,
    &app_notes,
    &app_network,
    &app_browser,
    NULL,
};
