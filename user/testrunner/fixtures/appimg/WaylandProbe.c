/* A glibc Wayland client, built against the Debian side of the sysroot.
 *
 * Unlike AppRun.c this fixture ships no libs/ directory, so it cannot be
 * self-contained: it only loads where the sandbox provides both the glibc
 * loader and a client-side libwayland. That is the point -- a real AppImage
 * has the same shape, and the host's own libwayland is musl and therefore
 * useless to a glibc client.
 *
 * The probe has to report something useful in two different worlds: under a
 * real compositor it enumerates the globals it was offered, and in the test
 * harness, where no Weston exists, it still proves the library loaded and
 * executed by reporting the connect failure and errno.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

static int n_globals = 0;
static int saw_compositor = 0, saw_shm = 0, saw_xdg_wm = 0;
static uint32_t shm_name = 0;

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                       uint32_t version) {
    (void) data;
    (void) reg;
    n_globals++;
    printf("global=%s v%u\n", iface, version);
    if (strcmp(iface, "wl_compositor") == 0) {
        saw_compositor = 1;
    } else if (strcmp(iface, "wl_shm") == 0) {
        saw_shm = 1;
        shm_name = name;
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        saw_xdg_wm = 1;
    }
}

static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void) data;
    (void) reg;
    (void) name;
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_global_remove };

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Reaching this line at all means the dynamic loader resolved a glibc
     * libwayland-client out of the sysroot: with no libs/ in the image, the
     * musl copy on the host could not have satisfied the NEEDED entry. */
    printf("WL_PROBE ok\n");

    /* Which copy did the loader actually pick? The host ships a musl
     * libwayland-client under /usr/lib/wayland; if that path shows up here the
     * glibc client silently bind-mounted the wrong library. */
    {
        FILE *m = fopen("/proc/self/maps", "r");
        int from_sysroot = 0, from_wayland_dir = 0, from_app = 0;
        if (m) {
            char line[512];
            while (fgets(line, sizeof(line), m)) {
                if (!strstr(line, "libwayland-client")) continue;
                if (strstr(line, "/usr/lib/wayland/")) from_wayland_dir++;
                else if (strstr(line, "/app/libs/")) from_app++;
                else from_sysroot++;
            }
            fclose(m);
        }
        printf("wl_lib_sysroot=%d wl_lib_wayland_dir=%d wl_lib_app=%d\n", from_sysroot,
               from_wayland_dir, from_app);
    }

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *wl = getenv("WAYLAND_DISPLAY");
    printf("xdg_runtime=%s\n", xdg ? xdg : "(null)");
    printf("wayland_display=%s\n", wl ? wl : "(null)");

    errno = 0;
    struct wl_display *dpy = wl_display_connect(NULL);
    int err = errno;
    printf("wl_display_connect=%d errno=%d\n", dpy != NULL, err);

    if (!dpy) {
        /* No compositor in the harness. The marker above is the proof that
         * matters here; the connect itself is exercised by hand under Weston. */
        printf("wl_globals=0\n");
        const char *ec = getenv("AI_EXIT");
        return ec ? atoi(ec) : 0;
    }

    printf("wl_fd=%d\n", wl_display_get_fd(dpy));

    struct wl_registry *reg = wl_display_get_registry(dpy);
    if (!reg) {
        printf("wl_registry=0\n");
        wl_display_disconnect(dpy);
        return 1;
    }
    wl_registry_add_listener(reg, &reg_listener, NULL);

    /* Two roundtrips: the first delivers the globals, the second proves the
     * connection is live rather than a one-shot reply. */
    int rt1 = wl_display_roundtrip(dpy);
    int rt2 = wl_display_roundtrip(dpy);
    printf("wl_roundtrip=%d,%d\n", rt1, rt2);
    printf("wl_globals=%d\n", n_globals);
    printf("wl_has_compositor=%d wl_has_shm=%d wl_has_xdg_wm_base=%d\n", saw_compositor, saw_shm,
           saw_xdg_wm);

    /* wl_shm is the only buffer path this kernel can serve: no DRM, no
     * dma-buf, no GBM anywhere, so a client that needs a GPU buffer is out of
     * scope and a client that draws through shared memory is the target. */
    if (saw_shm) {
        struct wl_shm *shm = wl_registry_bind(reg, shm_name, &wl_shm_interface, 1);
        printf("wl_shm_bind=%d\n", shm != NULL);
        if (shm) wl_shm_destroy(shm);
    }

    wl_registry_destroy(reg);
    printf("wl_disconnected\n");

    const char *ec = getenv("AI_EXIT");
    return ec ? atoi(ec) : 0;
}
