# Apps

Each app is one `.c` file here. The Makefile picks up every `apps/*.c` file.

## Add an app in three steps

1. **Write `apps/hello.c`** with an `app_t` (see `include/amdroid/ui.h`):

```c
#include "amdroid/ui.h"

static void h_draw(rect_t body, int focused){
    ui_text(body.x, body.y, "Hello!", C_TEXT, UI_SCALE);
}

const app_t app_hello = {
    .name = "Hello",
    .icon = UI_ICO_ABOUT,   /* one of UI_ICO_* */
    .pinned = 0,            /* 1 = always on the taskbar */
    .open = NULL,           /* optional: called when the window opens */
    .close = NULL,          /* optional: called when the window closes */
    .draw = h_draw,         /* every frame, with the window body */
    .key = NULL,            /* optional: return 1 if you used the key */
    .click = NULL,          /* optional: a click inside the body */
};
```

2. **List it in `apps/registry.c`**: add `extern const app_t app_hello;` and put
   `&app_hello,` in the table (before the `NULL`).

3. **Build**: `make`.

## What you can use

- Drawing: `ui_fill`, `ui_rrect`, `ui_text`, `ui_text_right`, `ui_icon`.
- Colours and metrics: `C_*` in `ui.h`, `UI_SCALE`, `UI_CH`, `UI_CW`, `UI_ROW_H`.
- Lists: `ui_list_head`, `ui_list_row`, `ui_list_rows`, `ui_button`.
- Settings: `ui_accent()`, `ui_anim_on`, `ui_wall_*`.
- Notifications (shown in the Dynamic Island, one after another):
  `ui_notify(app_index, "Title", "one line of text")`. Use `ui_app_index(&app_yours)`
  for the index so a click on the notification opens your app, or `-1`.
  `ui_msg_set("...")` is a short form: a notification with only a title.
- Background work: set `.tick` in your `app_t`; it is called every frame while the app
  is open, even minimized (the browser loads pages from it).
- Strings: `ui_num`, `ui_cat`, `ui_copy`, `ui_streq`, `ui_len`, `ui_uptime`, `ui_clock`, `ui_ip`.
- Kernel services: include the usual headers (`fat32.h`, `sched.h`, `net.h`, ...).

Keep app state `static` in your file. Use `body` from `draw` for layout; the window
manager gives you the same body rectangle in `click`.

## Files

- One file per app: `files.c`, `tasks.c`, `settings.c`, `about.c`, `calc.c`, `clock.c`,
  `notes.c`, `network.c`, `browser.c` (Browser).
- `registry.c`: the list of apps and their order (keys 1, 2, 3, ...).
- Core GUI: `src/kernel/gui/` (`gui.c` loop, `windows.c`, `taskbar.c`, `island.c`
  notifications, `desktop.c` icons, `menu.c` right-click menus, `wallpaper.c`, `ui.c`,
  `icons.c`, `login.c`).
