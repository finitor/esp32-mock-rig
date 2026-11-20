/*
 * civ_pty: run the CI-V core behind a pseudo-terminal on the development
 * machine, so real Hamlib can talk to it with no radio and no ESP32.
 *
 *   civ_pty [-q] [-m <Hamlib model>] <link-path>
 *
 * The personality is the IC-705 (3085) unless -m names another (3088, the
 * G90).
 *
 * Creates <link-path> as a symlink to the pty's device, then serves until
 * killed. Point rigctl at the link:
 *
 *   rigctl -m 3085 -r <link-path> -vvvv
 *
 * Every frame is logged to stderr in hex unless -q is given.
 *
 * The mock's own controls answer at address F0 as in the firmware, so
 * tools/mockctl.py can be tried against it too.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "audio_engine.h"
#include "cat_civ.h"
#include "mock_ctl.h"
#include "rig_g90.h"
#include "rig_ic705.h"

static int g_master = -1;
static int g_quiet;
static const char *g_link;

static void hex(const char *tag, const uint8_t *b, size_t n)
{
    if (g_quiet) return;
    fprintf(stderr, "%s", tag);
    for (size_t i = 0; i < n; i++) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
}

static void tx(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    hex("  ->", b, n);
    while (n > 0) {
        ssize_t w = write(g_master, b, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            return;
        }
        b += w;
        n -= (size_t)w;
    }
}

static void on_ptt(void *ctx, bool on)
{
    (void)ctx;
    if (!g_quiet) fprintf(stderr, "  PTT %s\n", on ? "on" : "off");
}

static void cleanup(int sig)
{
    (void)sig;
    if (g_link) unlink(g_link);
    _exit(0);
}

int main(int argc, char **argv)
{
    int arg = 1;
    if (arg < argc && strcmp(argv[arg], "-q") == 0) {
        g_quiet = 1;
        arg++;
    }
    const civ_rig_t *rig = &rig_ic705;
    if (arg + 1 < argc && strcmp(argv[arg], "-m") == 0) {
        int model = atoi(argv[arg + 1]);
        if (model == rig_g90.hamlib_model) {
            rig = &rig_g90;
        } else if (model != rig_ic705.hamlib_model) {
            fprintf(stderr, "no personality for Hamlib model %s\n", argv[arg + 1]);
            return 2;
        }
        arg += 2;
    }
    if (arg >= argc) {
        fprintf(stderr, "usage: %s [-q] [-m <Hamlib model>] <link-path>\n", argv[0]);
        return 2;
    }
    g_link = argv[arg];

    int slave;
    char name[128];
    if (openpty(&g_master, &slave, name, NULL, NULL) != 0) {
        perror("openpty");
        return 1;
    }
    /* Raw: CI-V is binary. The slave stays open here so the master never
     * sees end-of-file when a client disconnects. */
    struct termios t;
    if (tcgetattr(slave, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(slave, TCSANOW, &t);
    }

    unlink(g_link);
    if (symlink(name, g_link) != 0) {
        perror("symlink");
        return 1;
    }
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    /* The mock's own controls, on an engine that never sees audio: settings
     * can be set and read back, and the status stays at "nothing measured". */
    static ae_t engine;
    static int16_t parrot_store[30 * AE_PARROT_FS_HZ]; /* so the parrot route can be selected */
    static mock_ctl_t ctl;
    ae_config_t power_up;
    ae_init(&engine, 1u);
    ae_set_parrot_buffer(&engine, parrot_store, sizeof parrot_store / sizeof parrot_store[0]);
    ae_config_defaults(&power_up);
    mock_ctl_init(&ctl, &engine, &power_up);

    civ_t civ;
    static const civ_rig_t *const rigs[] = {&rig_ic705, &rig_g90};
    civ_init(&civ, rig, tx, on_ptt, NULL);
    civ_set_private(&civ, MOCK_CTL_ADDR, mock_ctl_command, &ctl);
    mock_ctl_set_rigs(&ctl, &civ, rigs, sizeof rigs / sizeof rigs[0], NULL, NULL);
    fprintf(stderr, "%s on %s (link %s)\n", rig->name, name, g_link);

    uint8_t buf[256];
    for (;;) {
        ssize_t n = read(g_master, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read");
            break;
        }
        if (n == 0) continue;
        hex("<-", buf, (size_t)n);
        civ_feed(&civ, buf, (size_t)n);
    }
    cleanup(0);
    return 0;
}
