#ifndef GUI_SERVER_H
#define GUI_SERVER_H

#include "kernel/sched/sync.h"
#include "kernel/gui/gfx.h"
#include "kernel/gui/theme.h"
#include <stdint.h>

struct shm_pool;

#define WL_MAX_CLIENTS 8
#define WL_MAX_SURFACES 12
#define WL_CLIENT_QUEUE 16
#define WL_MAX_WS 4

enum wl_event_type {
    WL_EV_NONE = 0,
    WL_EV_CONFIGURE,
    WL_EV_FRAME,
    WL_EV_KEY,
    WL_EV_CLOSE
};

struct wl_event {
    int type;
    int32_t a, b, c;
};

#define MOD_SHIFT 1
#define MOD_CTRL 2
#define MOD_ALT 4

struct wl_client {
    int used;
    char name[16];
    struct wl_event queue[WL_CLIENT_QUEUE];
    int qhead, qtail;
    struct semaphore sema;
    struct lock lock;
    struct wl_surface *surf;
};

struct wl_surface {
    int used;
    struct wl_client *client;
    char title[24];

    int x, y, w, h;
    int ws;
    int floating;

    uint8_t *buf;
    int buf_w, buf_h;
    int frame_pending;
};

struct wl_client *wl_display_connect(const char *name);
void wl_display_disconnect(struct wl_client *c);
struct wl_surface *wl_compositor_create_surface(struct wl_client *c,
                                                const char *title);
int wl_surface_attach(struct wl_surface *s, struct shm_pool *pool, int w,
                      int h);
void wl_surface_commit(struct wl_surface *s);
void wl_surface_destroy(struct wl_surface *s);
int wl_display_dispatch(struct wl_client *c, struct wl_event *ev);

void comp_init(void);
void comp_run(void);
void comp_request_exit(void);
void comp_damage_rect(int x, int y, int w, int h);
void comp_damage_surface(struct wl_surface *s);
void comp_post_key(uint8_t scancode, int pressed, uint8_t mods);
void comp_post_mouse(int dx, int dy, uint8_t buttons);
void comp_log(const char *s);

struct wl_surface **comp_surfaces(int *count);
int comp_screen_w(void);
int comp_screen_h(void);
void comp_send_configure(struct wl_surface *s, int w, int h);
void comp_send_close(struct wl_surface *s);
void comp_send_key(struct wl_surface *s, int scancode, int pressed, int mods);

void comp_destroy_surface_pool(struct wl_surface *s, struct shm_pool **pool);

#define COMP_BAR_H 22
#define COMP_TITLE_H 18
#define COMP_BORDER 2
#define COMP_GAP 8

#define WIN_RADIUS 8
#define WIN_SHADOW 6

#endif
