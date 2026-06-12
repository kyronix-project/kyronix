/* minimal raw X11 client: connects to server, creates a green window */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <stdint.h>

typedef struct { uint8_t b,p; uint16_t mj,mn,al,dl,p2; } setup_req_t;
typedef struct { uint8_t ok,p; uint16_t mj,mn; uint16_t len; } setup_hdr_t;
typedef struct {
    uint32_t rel,rid_base,rid_mask,motion_buf;
    uint16_t vendor_len,max_req; uint8_t ns,nf,ibo,bbo,bsu,bsp,min_kc,max_kc; uint32_t pad;
} setup_body_t;
typedef struct {
    uint32_t root,cmap,white,black,input_mask;
    uint16_t wp,hp,wm,hm,min_cm,max_cm;
    uint32_t vis; uint8_t backs,su,depth,ndepth;
} screen_t;

static int xfd = -1;

static int xconnect(void) {
    struct sockaddr_un a;
    int fd;
    /* try abstract socket first */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path + 1, "/tmp/.X11-unix/X0");
    if (connect(fd, (struct sockaddr*)&a, 2 + 1 + strlen("/tmp/.X11-unix/X0")) == 0)
        return fd;
    close(fd);
    /* fallback: regular Unix path */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "/tmp/.X11-unix/X0");
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0)
        return fd;
    close(fd);
    return -1;
}

static void xsend(const void* d, int n) { write(xfd, d, n); }

static int xread_exact(void* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = read(xfd, (char*)buf + got, n - got);
        if (r <= 0) return -1;
        got += r;
    }
    return got;
}

int main(void) {
    /* retry connection for up to 30 seconds */
    for (int i = 0; i < 30; i++) {
        xfd = xconnect();
        if (xfd >= 0) break;
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
    }
    if (xfd < 0) { fprintf(stderr, "xtest: connect failed after 30s\n"); return 1; }
    printf("xtest: connected to X server\n"); fflush(stdout);

    setup_req_t req = {'l',0,11,0,0,0,0};
    xsend(&req, sizeof(req));

    setup_hdr_t hdr;
    if (xread_exact(&hdr, sizeof(hdr)) < 0 || hdr.ok != 1) {
        fprintf(stderr, "xtest: setup failed\n"); return 1;
    }
    printf("xtest: X11 ok v%d.%d\n", hdr.mj, hdr.mn); fflush(stdout);

    static char buf[8192];
    int n = hdr.len * 4;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    if (xread_exact(buf, n) < 0) { fprintf(stderr, "xtest: read setup failed\n"); return 1; }

    setup_body_t* body = (setup_body_t*)buf;
    char* ptr = buf + sizeof(setup_body_t);
    ptr += (body->vendor_len + 3) & ~3;
    ptr += body->nf * 8;
    screen_t* scr = (screen_t*)ptr;
    printf("xtest: root=0x%x  %dx%d\n", scr->root, scr->wp, scr->hp); fflush(stdout);

    uint32_t wid = body->rid_base | 1;

    /* CreateWindow */
    struct __attribute__((packed)) {
        uint8_t op,depth; uint16_t len;
        uint32_t wid,parent; int16_t x,y; uint16_t w,h,bw,cls;
        uint32_t vis,vmask,bg,ev;
    } cw = {1,0,10, wid,scr->root, 0,0, scr->wp, scr->hp, 0,1,
            0, 0x00000802, 0x0000CC00 /*green*/, 1 /*ExposureMask*/};
    xsend(&cw, sizeof(cw));

    /* MapWindow */
    struct { uint8_t op,p; uint16_t len; uint32_t wid; } mw = {8,0,2,wid};
    xsend(&mw, sizeof(mw));

    /* PolyFillRectangle to paint the window background */
    /* connection open — Xorg will auto-fill background on MapWindow */

    printf("xtest: window 0x%x mapped (green). display!\n", wid); fflush(stdout);

    /* keep connection open for 15 seconds */
    struct timespec ts = {15, 0};
    nanosleep(&ts, NULL);
    return 0;
}
