/* Stub implementation of libperipheral_client.so (Qualcomm Peripheral Manager
 * client). cnss-daemon links against the real libperipheral_client.so and
 * calls pm_client_register()+pm_client_connect(), then blocks waiting for
 * the real pm-service QMI daemon to grant access -- a daemon that does not
 * exist on MiniOS. Confirmed live: cnss-daemon (pid 517) sits in state R
 * burning ~2 CPU-threads worth of time continuously (utime+stime far
 * exceeding wall-clock uptime) without ever reaching its WLFW bring-up
 * code. cnss-daemon's own undefined dynsyms confirm it needs exactly these
 * 5 symbols (readelf --dyn-syms on the real vendor binary): pm_client_
 * register/connect/disconnect/event_acknowledge/unregister.
 *
 * -nostdlib, no libc calls at all: built the same way as the project's
 * other proven LD_PRELOAD/staged shims (cnss_shim.c, libpropstub.so) --
 * linking against the cross toolchain's glibc here would pull in
 * GLIBC_2.17-versioned symbols that do not resolve against Android's
 * bionic libc.so on the device.
 */

typedef enum { PM_EVENT_ACCESS_ALLOWED = 0, PM_EVENT_ACCESS_BLOCKED = 1 } pm_event_t;
typedef void (*pm_client_notifier_t)(void *data, pm_event_t event);

typedef struct {
    int in_use;
    pm_client_notifier_t cb;
    void *data;
} pm_stub_handle_t;

#define MAX_HANDLES 8
static pm_stub_handle_t g_handles[MAX_HANDLES];

int pm_client_register(pm_client_notifier_t cb, void *data, const char *dev,
                        const char *cli, int *state, void **handle)
{
    int i;
    (void)dev;
    (void)cli;

    for (i = 0; i < MAX_HANDLES; i++) {
        if (!g_handles[i].in_use) {
            g_handles[i].in_use = 1;
            g_handles[i].cb = cb;
            g_handles[i].data = data;
            if (state)
                *state = PM_EVENT_ACCESS_ALLOWED;
            *handle = &g_handles[i];
            return 0;
        }
    }
    return -1;
}

int pm_client_connect(void *id)
{
    pm_stub_handle_t *h = (pm_stub_handle_t *)id;

    if (!h)
        return -1;
    if (h->cb)
        h->cb(h->data, PM_EVENT_ACCESS_ALLOWED);
    return 0;
}

int pm_client_disconnect(void *id)
{
    (void)id;
    return 0;
}

int pm_client_event_acknowledge(void *id, int event)
{
    (void)id;
    (void)event;
    return 0;
}

int pm_client_unregister(void *id)
{
    pm_stub_handle_t *h = (pm_stub_handle_t *)id;

    if (h)
        h->in_use = 0;
    return 0;
}
