/* console_api: see console_api.h. */
#include "console_api.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- writing ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} out_t;

static void out_init(out_t *o, char *buf, size_t cap)
{
    o->buf = buf;
    o->cap = cap;
    o->len = 0;
    o->overflow = cap == 0;
    if (cap) buf[0] = '\0';
}

static void out_f(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void out_f(out_t *o, const char *fmt, ...)
{
    if (o->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len) {
        o->overflow = true;
        return;
    }
    o->len += (size_t)n;
}

/* A JSON string, quoted and escaped. */
static void out_str(out_t *o, const char *s)
{
    out_f(o, "\"");
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            out_f(o, "\\%c", ch);
        } else if (ch < 0x20) {
            out_f(o, "\\u%04x", ch);
        } else {
            out_f(o, "%c", ch);
        }
    }
    out_f(o, "\"");
}

/* value / scale as a decimal number. scale is 1, 10 or 100, or their
 * negatives where the wire counts down from full scale; 10 prints one
 * decimal and 100 two. No
 * floating point is printed anywhere, so the text is the same on every C
 * library. */
static void out_scaled(out_t *o, int32_t value, int scale)
{
    int32_t v = scale < 0 ? -value : value;
    int32_t div = scale < 0 ? -scale : scale;
    if (div == 1) {
        out_f(o, "%ld", (long)v);
        return;
    }
    int32_t mag = v < 0 ? -v : v;
    if (div == 100) {
        out_f(o, "%s%ld.%02ld", v < 0 ? "-" : "", (long)(mag / div), (long)(mag % div));
        return;
    }
    out_f(o, "%s%ld.%ld", v < 0 ? "-" : "", (long)(mag / div), (long)(mag % div));
}

/* Hundredths of a dB, printed in tenths. */
static void out_cdb(out_t *o, int32_t cdb)
{
    out_scaled(o, (cdb + (cdb >= 0 ? 5 : -5)) / 10, 10);
}

static void out_bool(out_t *o, bool b)
{
    out_f(o, "%s", b ? "true" : "false");
}

static size_t out_done(out_t *o)
{
    if (o->overflow) {
        if (o->cap) o->buf[0] = '\0';
        return 0;
    }
    return o->len;
}

/* ---- the settings, by name ------------------------------------------------------ */

typedef struct {
    const char *name; /* as in host/tools/mockctl.py */
    uint8_t cmd;      /* the mock_ctl command that carries it */
    int8_t scale;     /* wire units per unit; 0 for a choice */
    const char *label;
    const char *group;
    const char *unit;
    const char *hint;
    const char *const *words; /* a choice's values, in wire order */
    uint8_t n_words;
} cset_t;

static const char *const w_route[] = {"monitor", "radio", "parrot", "link"};
static const char *const w_onoff[] = {"off", "on"};
static const char *const w_line[] = {"off", "rts", "dtr"};
static const char *const w_fade[] = {"off",    "mpg",      "mpp",     "mpd",     "mps",
                                     "nvis-d", "lowlat-d", "hilat-m", "hilat-d", "custom"};

#define G_RX "Receive"
#define G_PARROT "Parrot"
/* The fading conditions by what they are; the words are Data2G's names. */
static const char *const l_fade[] = {
    "Off",
    "Good: mpg (0.1 Hz, 0.5 ms)",
    "Poor: mpp (1 Hz, 2 ms)",
    "Disturbed: mpd (2 Hz, 4 ms)",
    "Slow, selective: mps (0.15 Hz, 2 ms)",
    "ITU mid-latitude disturbed NVIS: nvis-d (1 Hz, 7 ms)",
    "ITU low latitude disturbed: lowlat-d (10 Hz, 6 ms)",
    "ITU high latitude moderate: hilat-m (10 Hz, 3 ms)",
    "ITU high latitude disturbed: hilat-d (30 Hz, 7 ms)",
    "Custom: the two numbers below",
};

#define G_PATH "Path"
#define G_LINK "Link"
#define G_PTT "PTT"

static const cset_t csets[] = {
    {"route", MOCK_CTL_ROUTE, 0, "Receive route", G_RX, NULL,
     "monitor: loopback, PTT ignored. radio: mute while keyed. parrot: record while keyed, replay after. "
     "link: hear the other board.",
     w_route, 4},
    {"noise", MOCK_CTL_NOISE, 0, "Noise", G_RX, NULL, "Add noise and hold the signal at the SNR.", w_onoff, 2},
    {"snr", MOCK_CTL_SNR, 10, "SNR", G_RX, "dB", NULL, NULL, 0},
    {"bw", MOCK_CTL_REF_BW, 1, "SNR bandwidth", G_RX, "Hz", "2500 for WSJT-X and Data2G, 3000 for VARA.", NULL, 0},
    {"floor", MOCK_CTL_FLOOR, -10, "Noise floor", G_RX, "dBFS", NULL, NULL, 0},
    {"atten", MOCK_CTL_ATTEN, 10, "Attenuation", G_RX, "dB", NULL, NULL, 0},
    {"turnaround", MOCK_CTL_TURNAROUND, 1, "Turnaround", G_RX, "ms", "Receive mute after unkeying; not in monitor.",
     NULL, 0},
    {"fade", MOCK_CTL_FADE, 0, "Fading", G_PATH, NULL,
     "An HF path: two paths that fade apart, by Doppler spread and the delay between them. The names are the "
     "Data2G simulator's; the ITU ones are severe conditions from ITU-R F.1487.",
     w_fade, 10},
    {"doppler", MOCK_CTL_DOPPLER, 100, "Doppler spread", G_PATH, "Hz", "For custom fading: how fast it fades.", NULL,
     0},
    {"fadedelay", MOCK_CTL_FADE_DELAY, 10, "Path delay", G_PATH, "ms",
     "For custom fading: how much later the second path arrives.", NULL, 0},
    {"offset", MOCK_CTL_FREQ_OFFSET, 10, "Frequency offset", G_PATH, "Hz", "Moves the receive audio up or down.",
     NULL, 0},
    {"delay", MOCK_CTL_PARROT_DELAY, 10, "Replay delay", G_PARROT, "s", "From unkeying to the replay.", NULL, 0},
    {"period", MOCK_CTL_PARROT_PERIOD, 10, "Slot period", G_PARROT, "s", "15 for FT8, 7.5 for FT4, 0 for off.", NULL,
     0},
    {"linkbuffer", MOCK_CTL_LINK_BUFFER, 1, "Buffer", G_LINK, "ms",
     "Audio held before it is played: more rides out uneven arrival, less answers sooner.", NULL, 0},
    {"match", MOCK_CTL_LINK_MATCH, 0, "Frequency match", G_LINK, NULL,
     "On: the other board is heard only when it transmits on this radio's receive frequency.", w_onoff, 2},
    {"share", MOCK_CTL_LINK_COORD, 0, "Shared channel", G_LINK, NULL,
     "On: the two boards keep one set of noise and fading settings, changed from either, and fade alike.", w_onoff,
     2},
    {"line", MOCK_CTL_PTT_LINE, 0, "Control-line PTT", G_PTT, NULL, "CAT keying always works; these add to it.",
     w_line, 3},
    {"vox", MOCK_CTL_VOX, 0, "VOX", G_PTT, NULL, "Transmit audio keys the radio.", w_onoff, 2},
    {"voxlevel", MOCK_CTL_VOX_THRESHOLD, -10, "VOX threshold", G_PTT, "dBFS", NULL, NULL, 0},
    {"voxhang", MOCK_CTL_VOX_HANG, 1, "VOX hang", G_PTT, "ms", NULL, NULL, 0},
};

/* What to call each value of a choice on the page, where its word alone does
 * not say enough; NULL where it does. */
static const char *const *cset_labels(const cset_t *s)
{
    return s->cmd == MOCK_CTL_FADE ? l_fade : NULL;
}

_Static_assert(sizeof l_fade / sizeof l_fade[0] == AE_FADE_COUNT, "a label for every fading condition");

/* A setting that is in force only while another, a choice, has one value:
 * the custom fading's two numbers, which the named conditions do not use.
 * The setting keeps its value meanwhile and can still be set by the other
 * control paths; the page shows it as not in force. NULL for the rest. */
static const cset_t *cset_when(const cset_t *s, const char **value)
{
    if (s->cmd != MOCK_CTL_DOPPLER && s->cmd != MOCK_CTL_FADE_DELAY) return NULL;
    for (size_t i = 0; i < ARRAY_LEN(csets); i++) {
        if (csets[i].cmd == MOCK_CTL_FADE) {
            *value = w_fade[AE_FADE_CUSTOM];
            return &csets[i];
        }
    }
    return NULL;
}

static const cset_t *find_cset(const char *name, size_t len)
{
    for (size_t i = 0; i < ARRAY_LEN(csets); i++) {
        if (strlen(csets[i].name) == len && memcmp(csets[i].name, name, len) == 0) return &csets[i];
    }
    return NULL;
}

/* The ends of a setting's range, as wire values in the order the person
 * sees them: a scale below zero turns the range around. */
static void cset_range(const cset_t *s, int32_t *min_wire, int32_t *max_wire)
{
    int32_t lo = 0, hi = 0;
    (void)mock_ctl_setting_range(s->cmd, &lo, &hi);
    *min_wire = s->scale < 0 ? hi : lo;
    *max_wire = s->scale < 0 ? lo : hi;
}

size_t console_info_json(mock_ctl_t *m, const console_info_t *info, char *buf, size_t cap)
{
    out_t o;
    ae_parrot_status_t par;
    ae_get_parrot_status(m->engine, &par);

    const civ_rig_t *rig = mock_ctl_rig(m);
    out_init(&o, buf, cap);
    out_f(&o, "{\"api\":%d,\"name\":", CONSOLE_API_VERSION);
    out_str(&o, info->name ? info->name : "");
    out_f(&o, ",\"rig\":");
    out_str(&o, rig ? rig->name : info->rig ? info->rig : "");
    out_f(&o, ",\"hamlib\":%d,\"rigs\":[", rig ? rig->hamlib_model : info->hamlib_model);
    for (size_t i = 0; rig && i < m->n_rigs; i++) {
        out_f(&o, "%s{\"name\":", i ? "," : "");
        out_str(&o, m->rigs[i]->name);
        out_f(&o, ",\"hamlib\":%d}", m->rigs[i]->hamlib_model);
    }
    out_f(&o, "],\"parrot_capacity\":");
    out_scaled(&o, (int32_t)((par.capacity_ms + 50) / 100), 10);
    out_f(&o, ",\"scope\":{\"fs\":%d,\"n\":%d,\"bins\":%d,\"rows\":%d}", DSP_SPEC_FS_HZ, DSP_SPEC_N, DSP_SPEC_BINS,
          CONSOLE_SCOPE_ROWS_PER_S);
    out_f(&o, ",\"settings\":[");
    for (size_t i = 0; i < ARRAY_LEN(csets); i++) {
        const cset_t *s = &csets[i];
        out_f(&o, "%s{\"name\":\"%s\",\"label\":", i ? "," : "", s->name);
        out_str(&o, s->label);
        out_f(&o, ",\"group\":");
        out_str(&o, s->group);
        if (s->hint) {
            out_f(&o, ",\"hint\":");
            out_str(&o, s->hint);
        }
        const char *when_value = NULL;
        const cset_t *when = cset_when(s, &when_value);
        if (when) out_f(&o, ",\"when\":{\"%s\":\"%s\"}", when->name, when_value);
        if (s->scale == 0) {
            out_f(&o, ",\"choices\":[");
            for (size_t w = 0; w < s->n_words; w++) out_f(&o, "%s\"%s\"", w ? "," : "", s->words[w]);
            out_f(&o, "]");
            const char *const *labels = cset_labels(s);
            if (labels) {
                out_f(&o, ",\"labels\":[");
                for (size_t w = 0; w < s->n_words; w++) {
                    out_f(&o, "%s", w ? "," : "");
                    out_str(&o, labels[w]);
                }
                out_f(&o, "]");
            }
        } else {
            int32_t lo, hi;
            cset_range(s, &lo, &hi);
            out_f(&o, ",\"unit\":\"%s\",\"min\":", s->unit);
            out_scaled(&o, lo, s->scale);
            out_f(&o, ",\"max\":");
            out_scaled(&o, hi, s->scale);
            out_f(&o, ",\"step\":%s", s->scale == 1 ? "1" : s->scale == 100 ? "0.01" : "0.1");
        }
        out_f(&o, "}");
    }
    out_f(&o, "]}");
    return out_done(&o);
}

size_t console_settings_json(mock_ctl_t *m, char *buf, size_t cap)
{
    out_t o;
    ae_config_t cfg;
    ae_get_config(m->engine, &cfg);

    out_init(&o, buf, cap);
    out_f(&o, "{");
    for (size_t i = 0; i < ARRAY_LEN(csets); i++) {
        const cset_t *s = &csets[i];
        int32_t v = mock_ctl_setting_get(&cfg, s->cmd);
        out_f(&o, "%s\"%s\":", i ? "," : "", s->name);
        if (s->scale == 0) {
            out_f(&o, "\"%s\"", v >= 0 && v < s->n_words ? s->words[v] : "");
        } else {
            out_scaled(&o, v, s->scale);
        }
    }
    out_f(&o, "}");
    return out_done(&o);
}

/* ---- reading a flat JSON object ---------------------------------------------------
 *
 * {"name": value, ...} where a value is a number, a string, true, false or
 * null. No nesting: a setting is never an object or an array. The text must
 * end with a zero byte.
 */
typedef enum { J_NUMBER, J_STRING, J_TRUE, J_FALSE, J_NULL } jtype_t;

typedef struct {
    const char *name; /* between the quotes, still escaped */
    size_t name_len;
    jtype_t type;
    const char *text; /* a string's contents, still escaped */
    size_t text_len;
    double num;
} jmember_t;

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p is just past an opening quote. Returns the closing quote, or NULL for a
 * string that does not end, holds a control character, or uses an escape
 * other than the one-letter ones. */
static const char *string_end(const char *p)
{
    for (;; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"') return p;
        if (ch < 0x20) return NULL; /* the end of the text included */
        if (ch == '\\') {
            p++;
            if (*p == '\0' || !strchr("\"\\/bfnrt", *p)) return NULL;
        }
    }
}

static bool j_open(const char **pp)
{
    const char *p = skip_ws(*pp);
    if (*p != '{') return false;
    *pp = p + 1;
    return true;
}

/* 1: a member, 0: the object ended and nothing follows it, -1: malformed. */
static int j_next(const char **pp, bool *first, jmember_t *m)
{
    const char *p = skip_ws(*pp);
    if (*p == '}') {
        p = skip_ws(p + 1);
        return *p ? -1 : 0;
    }
    if (!*first) {
        if (*p != ',') return -1;
        p = skip_ws(p + 1);
    }
    *first = false;

    if (*p != '"') return -1;
    const char *e = string_end(p + 1);
    if (!e) return -1;
    m->name = p + 1;
    m->name_len = (size_t)(e - (p + 1));
    p = skip_ws(e + 1);
    if (*p != ':') return -1;
    p = skip_ws(p + 1);

    m->text = NULL;
    m->text_len = 0;
    m->num = 0;
    if (*p == '"') {
        e = string_end(p + 1);
        if (!e) return -1;
        m->type = J_STRING;
        m->text = p + 1;
        m->text_len = (size_t)(e - (p + 1));
        p = e + 1;
    } else if (strncmp(p, "true", 4) == 0) {
        m->type = J_TRUE;
        p += 4;
    } else if (strncmp(p, "false", 5) == 0) {
        m->type = J_FALSE;
        p += 5;
    } else if (strncmp(p, "null", 4) == 0) {
        m->type = J_NULL;
        p += 4;
    } else {
        char tmp[32];
        const char *q = p;
        while (*q && strchr("+-0123456789.eE", *q)) q++;
        size_t n = (size_t)(q - p);
        if (n == 0 || n >= sizeof tmp) return -1;
        if (*p != '-' && (*p < '0' || *p > '9')) return -1;
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        char *end;
        double v = strtod(tmp, &end);
        if (*end != '\0' || !isfinite(v)) return -1;
        m->type = J_NUMBER;
        m->num = v;
        p = q;
    }
    /* A value runs straight into the next delimiter. */
    const char *after = skip_ws(p);
    if (*after != ',' && *after != '}') return -1;
    *pp = p;
    return 1;
}

static bool unescape(const char *text, size_t len, char *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        if (ch == '\\' && i + 1 < len) {
            ch = text[++i];
            switch (ch) {
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            default: /* quote, backslash, slash */
                break;
            }
        }
        if (n + 1 >= cap) return false;
        out[n++] = ch;
    }
    if (cap == 0) return false;
    out[n] = '\0';
    return true;
}

bool console_json_string(const char *json, const char *name, char *out, size_t cap)
{
    const char *p = json;
    bool first = true;
    bool found = false;
    jmember_t mem;
    int r;

    if (!j_open(&p)) return false;
    while ((r = j_next(&p, &first, &mem)) == 1) {
        if (mem.name_len == strlen(name) && memcmp(mem.name, name, mem.name_len) == 0) {
            if (mem.type != J_STRING || !unescape(mem.text, mem.text_len, out, cap)) return false;
            found = true;
        }
    }
    return r == 0 && found;
}

/* ---- changing settings ------------------------------------------------------------ */

static console_result_t refuse(console_result_t why, char *err, size_t err_cap, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static console_result_t refuse(console_result_t why, char *err, size_t err_cap, const char *fmt, ...)
{
    if (err && err_cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_cap, fmt, ap);
        va_end(ap);
    }
    return why;
}

static console_result_t refuse_range(const cset_t *s, char *err, size_t err_cap)
{
    out_t o;
    int32_t lo, hi;
    cset_range(s, &lo, &hi);
    out_init(&o, err, err_cap);
    out_f(&o, "%s: must be between ", s->name);
    out_scaled(&o, lo, s->scale);
    out_f(&o, " and ");
    out_scaled(&o, hi, s->scale);
    out_f(&o, " %s", s->unit);
    (void)out_done(&o);
    return CONSOLE_BAD_REQUEST;
}

static console_result_t refuse_choice(const cset_t *s, char *err, size_t err_cap)
{
    out_t o;
    out_init(&o, err, err_cap);
    out_f(&o, "%s: must be one of", s->name);
    for (size_t w = 0; w < s->n_words; w++) out_f(&o, "%s \"%s\"", w ? "," : "", s->words[w]);
    (void)out_done(&o);
    return CONSOLE_BAD_REQUEST;
}

/* A name from the request is only repeated back if it is plain. */
static bool plain_name(const char *name, size_t len)
{
    if (len == 0 || len > 24) return false;
    for (size_t i = 0; i < len; i++) {
        char ch = name[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                  ch == '-';
        if (!ok) return false;
    }
    return true;
}

console_result_t console_settings_apply(mock_ctl_t *m, const char *json, char *err, size_t err_cap)
{
    static const char malformed[] = "expected a JSON object of settings, like {\"noise\":\"on\",\"snr\":-12.5}";
    ae_config_t cfg;
    const char *p = json;
    bool first = true;
    size_t changed = 0;
    jmember_t mem;
    int r;

    if (err && err_cap) err[0] = '\0';
    ae_get_config(m->engine, &cfg);

    if (!j_open(&p)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);
    while ((r = j_next(&p, &first, &mem)) == 1) {
        const cset_t *s = find_cset(mem.name, mem.name_len);
        if (!s) {
            if (plain_name(mem.name, mem.name_len)) {
                return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%.*s: no such setting", (int)mem.name_len, mem.name);
            }
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "no such setting");
        }

        int32_t wire = -1;
        if (s->scale == 0) {
            if (mem.type != J_STRING) return refuse_choice(s, err, err_cap);
            for (size_t w = 0; w < s->n_words; w++) {
                if (strlen(s->words[w]) == mem.text_len && memcmp(s->words[w], mem.text, mem.text_len) == 0) {
                    wire = (int32_t)w;
                }
            }
            if (wire < 0) return refuse_choice(s, err, err_cap);
        } else {
            if (mem.type != J_NUMBER) {
                return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s: must be a number", s->name);
            }
            double scaled = mem.num * (double)s->scale;
            if (!(fabs(scaled) < 1e9)) return refuse_range(s, err, err_cap);
            wire = (int32_t)lround(scaled);
        }

        switch (mock_ctl_setting_put(m, &cfg, s->cmd, wire)) {
        case MOCK_CTL_SET_OK:
            changed++;
            break;
        case MOCK_CTL_SET_UNAVAILABLE:
            return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "%s: \"%s\" is not available, this board %s", s->name,
                          s->words[wire], wire == AE_ROUTE_LINK ? "was built without the link" : "has no parrot buffer");
        default:
            return s->scale == 0 ? refuse_choice(s, err, err_cap) : refuse_range(s, err, err_cap);
        }
    }
    if (r < 0) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);

    if (changed) ae_configure(m->engine, &cfg);
    return CONSOLE_OK;
}

console_result_t console_rig_apply(mock_ctl_t *m, const char *json, char *err, size_t err_cap)
{
    static const char malformed[] = "expected the radio's Hamlib model number, like {\"hamlib\":3088}";
    const char *p = json;
    bool first = true;
    bool have = false;
    double model = 0;
    jmember_t mem;
    int r;

    if (err && err_cap) err[0] = '\0';
    if (!j_open(&p)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);
    while ((r = j_next(&p, &first, &mem)) == 1) {
        if (mem.name_len != 6 || memcmp(mem.name, "hamlib", 6) != 0 || mem.type != J_NUMBER) {
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);
        }
        model = mem.num;
        have = true;
    }
    if (r < 0 || !have || !(model >= 1.0 && model <= 9999.0) || model != floor(model)) {
        return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);
    }
    if (!mock_ctl_select_rig(m, (int)model)) {
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "the mock has no personality for Hamlib model %d", (int)model);
    }
    return CONSOLE_OK;
}

console_result_t console_clip_apply(mock_ctl_t *m, const char *json, char *err, size_t err_cap)
{
    static const char malformed[] = "expected a clip operation, like {\"op\":\"save\",\"clip\":1}";
    static const char *const ops[] = {"save", "play", "erase"}; /* in the order of mock_ctl_clip_op_t */
    const char *p = json;
    bool first = true;
    int op = -1;
    double clip = 0;
    jmember_t mem;
    int r;

    if (err && err_cap) err[0] = '\0';
    if (!j_open(&p)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);
    while ((r = j_next(&p, &first, &mem)) == 1) {
        if (mem.name_len == 2 && memcmp(mem.name, "op", 2) == 0 && mem.type == J_STRING) {
            op = -1;
            for (int i = 0; i < 3; i++) {
                if (strlen(ops[i]) == mem.text_len && memcmp(ops[i], mem.text, mem.text_len) == 0) op = i;
            }
            if (op < 0) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "op: must be one of \"save\", \"play\", \"erase\"");
        } else if (mem.name_len == 4 && memcmp(mem.name, "clip", 4) == 0 && mem.type == J_NUMBER) {
            clip = mem.num;
        } else {
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);
        }
    }
    if (r < 0 || op < 0 || !(clip >= 1.0 && clip <= 99.0) || clip != floor(clip)) {
        return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);
    }

    switch (mock_ctl_clip(m, (mock_ctl_clip_op_t)op, (unsigned)clip)) {
    case MOCK_CTL_CLIP_OK:
        return CONSOLE_OK;
    case MOCK_CTL_CLIP_NONE:
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "there is no clip %d on this board", (int)clip);
    case MOCK_CTL_CLIP_BUSY:
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "another clip operation is still under way");
    case MOCK_CTL_CLIP_NOTHING:
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "%s",
                      op == MOCK_CTL_CLIP_SAVE ? "there is no recording to keep: transmit in the parrot route first"
                                               : "that clip is empty");
    default:
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "%s",
                      op == MOCK_CTL_CLIP_SAVE ? "the parrot is still recording"
                                               : "a clip plays only in the parrot route, with PTT off");
    }
}

/* ---- the front panel ---------------------------------------------------------------- */

static const char *const w_duplex[] = {"simplex", "dup-", "dup+"};

static bool rig_has(const civ_t *c, uint32_t cap)
{
    return (c->rig->caps & cap) != 0;
}

/* The n-th name in "off|fast|mid|slow". NULL past the end. */
static const char *choice_at(const char *choices, size_t n, size_t *len)
{
    const char *p = choices;
    for (;;) {
        const char *e = strchr(p, '|');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (n == 0) {
            *len = l;
            return p;
        }
        if (!e) return NULL;
        p = e + 1;
        n--;
    }
}

/* The highest value the panel offers for a function. */
static unsigned func_max(const civ_item_t *it)
{
    unsigned n = 0;
    size_t len;
    if (!it->choices) return 1;
    while (choice_at(it->choices, n, &len)) n++;
    return n - 1;
}

static bool func_offers(const civ_item_t *it, unsigned value)
{
    size_t len;
    if (!it->choices) return value <= 1;
    return choice_at(it->choices, value, &len) && len > 0;
}

static void out_vfo(out_t *o, const civ_vfo_t *v)
{
    char mode[16];
    console_mode_name(v->mode, false, mode, sizeof mode);
    out_f(o, "{\"freq\":%llu,\"mode\":\"%s\",\"data\":", (unsigned long long)v->freq, mode);
    out_bool(o, v->data);
    out_f(o, ",\"filter\":%u}", (unsigned)v->filter);
}

static void out_ranges(out_t *o, const civ_range_t *r, size_t n)
{
    out_f(o, "[");
    for (size_t i = 0; i < n; i++) {
        out_f(o, "%s[%llu,%llu]", i ? "," : "", (unsigned long long)r[i].lo_hz, (unsigned long long)r[i].hi_hz);
    }
    out_f(o, "]");
}

/* The items of one table that have a name. */
static void out_items(out_t *o, uint8_t cmd, const civ_item_t *items, size_t n, size_t cap_n, const uint16_t *values)
{
    bool first = true;
    out_f(o, "[");
    for (size_t i = 0; i < n && i < cap_n; i++) {
        const civ_item_t *it = &items[i];
        if (!it->name) continue;
        out_f(o, "%s{\"id\":\"%02X.%02X\",\"name\":", first ? "" : ",", cmd, it->sub);
        first = false;
        out_str(o, it->name);
        out_f(o, ",\"value\":%u", (unsigned)values[i]);
        if (cmd == 0x15) {
            out_f(o, ",\"max\":%d", civ_meter_is_status(it->sub) ? 1 : 255);
        } else if (cmd == 0x16 && it->choices) {
            /* A name per value; an empty one is a value the panel does not offer. */
            size_t len;
            const char *ch;
            out_f(o, ",\"choices\":[");
            for (size_t k = 0; (ch = choice_at(it->choices, k, &len)) != NULL; k++) {
                out_f(o, "%s\"%.*s\"", k ? "," : "", (int)len, ch);
            }
            out_f(o, "]");
        }
        out_f(o, "}");
    }
    out_f(o, "]");
}

size_t console_radio_json(const civ_t *c, char *buf, size_t cap)
{
    out_t o;
    const civ_rig_t *rig = c->rig;

    out_init(&o, buf, cap);
    out_f(&o, "{\"rig\":");
    out_str(&o, rig->name);
    out_f(&o, ",\"hamlib\":%d,\"gen\":%lu,\"vfo\":\"%c\",\"split\":", rig->hamlib_model,
          (unsigned long)civ_state_hash(c), c->sel ? 'B' : 'A');
    out_bool(&o, c->split);
    out_f(&o, ",\"a\":");
    out_vfo(&o, &c->vfo[0]);
    out_f(&o, ",\"b\":");
    out_vfo(&o, &c->vfo[1]);

    out_f(&o, ",\"modes\":[");
    for (size_t i = 0; i < rig->n_modes; i++) {
        char mode[16];
        console_mode_name(rig->modes[i], false, mode, sizeof mode);
        out_f(&o, "%s\"%s\"", i ? "," : "", mode);
    }
    out_f(&o, "],\"filters\":%u,\"can\":{\"data\":", (unsigned)rig->max_filter);
    out_bool(&o, rig_has(c, CIV_CAP_DATA_MODE));
    out_f(&o, ",\"equal\":");
    out_bool(&o, rig_has(c, CIV_CAP_VFO_EQUAL));

    out_f(&o, "},\"duplex\":");
    if (rig_has(c, CIV_CAP_DUPLEX) && c->duplex >= 0x10 && c->duplex <= 0x12) {
        out_f(&o, "\"%s\"", w_duplex[c->duplex - 0x10]);
    } else {
        out_f(&o, "null");
    }
    out_f(&o, ",\"rit\":");
    if (rig_has(c, CIV_CAP_RIT)) {
        out_f(&o, "{\"on\":");
        out_bool(&o, c->rit_on);
        out_f(&o, ",\"hz\":%d}", (int)c->rit_hz);
    } else {
        out_f(&o, "null");
    }
    out_f(&o, ",\"tuner\":");
    if (rig_has(c, CIV_CAP_TUNER)) {
        out_bool(&o, c->tuner != 0);
    } else {
        out_f(&o, "null");
    }
    out_f(&o, ",\"att\":");
    if (rig_has(c, CIV_CAP_ATT)) {
        out_f(&o, "{\"on\":");
        out_bool(&o, c->att != 0);
        out_f(&o, ",\"byte\":\"%02X\",\"set\":", c->att);
        out_bool(&o, rig->att_on != 0);
        out_f(&o, "}");
    } else {
        out_f(&o, "null");
    }
    out_f(&o, ",\"step\":");
    if (rig_has(c, CIV_CAP_TS)) {
        out_f(&o, "\"%02X\"", c->ts);
    } else {
        out_f(&o, "null");
    }
    out_f(&o, ",\"width\":");
    if (rig_has(c, CIV_CAP_FILTER_BW)) {
        out_f(&o, "\"%02X\"", c->filter_bw);
    } else {
        out_f(&o, "null");
    }
    out_f(&o, ",\"power\":");
    if (rig_has(c, CIV_CAP_POWER)) {
        out_bool(&o, c->power);
    } else {
        out_f(&o, "null");
    }

    out_f(&o, ",\"rx\":");
    out_ranges(&o, rig->rx, rig->n_rx);
    out_f(&o, ",\"tx\":");
    out_ranges(&o, rig->tx, rig->n_tx);
    out_f(&o, ",\"levels\":");
    out_items(&o, 0x14, rig->levels, rig->n_levels, CIV_MAX_LEVELS, c->level);
    out_f(&o, ",\"funcs\":");
    out_items(&o, 0x16, rig->funcs, rig->n_funcs, CIV_MAX_FUNCS, c->func);
    out_f(&o, ",\"meters\":");
    out_items(&o, 0x15, rig->meters, rig->n_meters, CIV_MAX_METERS, c->meter);
    out_f(&o, "}");
    return out_done(&o);
}

static bool name_is(const jmember_t *mem, const char *name)
{
    return mem->name_len == strlen(name) && memcmp(mem->name, name, mem->name_len) == 0;
}

static bool text_is(const jmember_t *mem, const char *text)
{
    return mem->type == J_STRING && mem->text_len == strlen(text) && memcmp(mem->text, text, mem->text_len) == 0;
}

/* A whole number in lo..hi. */
static bool whole(const jmember_t *mem, double lo, double hi, long long *out)
{
    if (mem->type != J_NUMBER || !(mem->num >= lo && mem->num <= hi) || mem->num != floor(mem->num)) return false;
    *out = (long long)mem->num;
    return true;
}

static bool truth(const jmember_t *mem, bool *out)
{
    if (mem->type != J_TRUE && mem->type != J_FALSE) return false;
    *out = mem->type == J_TRUE;
    return true;
}

/* Shaped like an item's id, "14.0A": safe to repeat back in a message. */
static bool id_shaped(const char *name, size_t len)
{
    static const char hex[] = "0123456789ABCDEFabcdef";
    return len == 5 && name[2] == '.' && strchr(hex, name[0]) && strchr(hex, name[1]) && strchr(hex, name[3]) &&
           strchr(hex, name[4]) && name[0] && name[1] && name[3] && name[4];
}

/* "14.0A": the command and the item's place in the personality's table. */
static const civ_item_t *item_by_id(const civ_t *c, const jmember_t *mem, uint8_t *cmd)
{
    const civ_item_t *items;
    size_t n;
    if (!id_shaped(mem->name, mem->name_len) || mem->name[0] != '1') return NULL;
    const char hex[3] = {mem->name[3], mem->name[4], '\0'};
    unsigned long sub = strtoul(hex, NULL, 16);
    switch (mem->name[1]) {
    case '4':
        items = c->rig->levels;
        n = c->rig->n_levels < CIV_MAX_LEVELS ? c->rig->n_levels : CIV_MAX_LEVELS;
        break;
    case '5':
        items = c->rig->meters;
        n = c->rig->n_meters < CIV_MAX_METERS ? c->rig->n_meters : CIV_MAX_METERS;
        break;
    case '6':
        items = c->rig->funcs;
        n = c->rig->n_funcs < CIV_MAX_FUNCS ? c->rig->n_funcs : CIV_MAX_FUNCS;
        break;
    default:
        return NULL;
    }
    *cmd = (uint8_t)(0x10 + (mem->name[1] - '0'));
    for (size_t i = 0; i < n; i++) {
        if (items[i].sub == sub && items[i].name) return &items[i];
    }
    return NULL;
}

/* One member of a change. The radio has the last word on everything CAT can
 * set: a refusal from civ_local() is the radio's FA. */
static console_result_t radio_put(mock_ctl_t *m, const jmember_t *mem, bool *key, bool *key_on, char *err,
                                  size_t err_cap)
{
    civ_t *c = m->civ;
    const civ_rig_t *rig = c->rig;
    const civ_vfo_t *v = civ_selected_vfo(c);
    long long n;
    bool on;
    uint8_t d[4];
    uint8_t cmd;

    if (name_is(mem, "freq")) {
        if (!whole(mem, 0, 9999999999.0, &n)) {
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "freq: must be a whole number of hertz");
        }
        uint8_t bcd[5];
        civ_u64_to_bcd_le((uint64_t)n, bcd, 5);
        if (civ_local(c, 0x05, bcd, 5)) return CONSOLE_OK;
        return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "freq: the %s does not tune to %lld Hz", rig->name, n);
    }
    if (name_is(mem, "vfo")) {
        if (!text_is(mem, "A") && !text_is(mem, "B")) {
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "vfo: must be \"A\" or \"B\"");
        }
        d[0] = text_is(mem, "B") ? 0x01 : 0x00;
        if (civ_local(c, 0x07, d, 1)) return CONSOLE_OK;
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "vfo: the radio refused");
    }
    if (name_is(mem, "op")) {
        if (!text_is(mem, "equal") && !text_is(mem, "swap")) {
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "op: must be \"equal\" or \"swap\"");
        }
        d[0] = text_is(mem, "equal") ? 0xA0 : 0xB0;
        if (civ_local(c, 0x07, d, 1)) return CONSOLE_OK;
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "op: the %s cannot do that", rig->name);
    }
    if (name_is(mem, "mode")) {
        for (size_t i = 0; mem->type == J_STRING && i < rig->n_modes; i++) {
            char mode[16];
            console_mode_name(rig->modes[i], false, mode, sizeof mode);
            if (!text_is(mem, mode)) continue;
            d[0] = rig->modes[i];
            if (civ_local(c, 0x06, d, 1)) return CONSOLE_OK;
        }
        return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "mode: not a mode of the %s", rig->name);
    }
    if (name_is(mem, "data")) {
        if (!truth(mem, &on)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "data: must be true or false");
        d[0] = 0x06;
        d[1] = on ? 0x01 : 0x00;
        if (civ_local(c, 0x1A, d, 2)) return CONSOLE_OK;
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "data: the %s has no data mode switch", rig->name);
    }
    if (name_is(mem, "filter")) {
        if (!whole(mem, 1, rig->max_filter, &n)) {
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "filter: must be 1 to %u", (unsigned)rig->max_filter);
        }
        d[0] = v->mode;
        d[1] = (uint8_t)n;
        if (civ_local(c, 0x06, d, 2)) return CONSOLE_OK;
        return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "filter: the radio refused");
    }
    if (name_is(mem, "split")) {
        if (!truth(mem, &on)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "split: must be true or false");
        d[0] = on ? 0x01 : 0x00;
        if (civ_local(c, 0x0F, d, 1)) return CONSOLE_OK;
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "split: the radio refused");
    }
    if (name_is(mem, "duplex")) {
        for (size_t i = 0; i < ARRAY_LEN(w_duplex); i++) {
            if (!text_is(mem, w_duplex[i])) continue;
            d[0] = (uint8_t)(0x10 + i);
            if (civ_local(c, 0x0F, d, 1)) return CONSOLE_OK;
            return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "duplex: the %s has no repeater shift", rig->name);
        }
        return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "duplex: must be \"simplex\", \"dup-\" or \"dup+\"");
    }
    if (name_is(mem, "rit")) {
        if (!truth(mem, &on)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "rit: must be true or false");
        d[0] = 0x01;
        d[1] = on ? 0x01 : 0x00;
        if (civ_local(c, 0x21, d, 2)) return CONSOLE_OK;
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "rit: the %s has no RIT", rig->name);
    }
    if (name_is(mem, "rit_hz")) {
        if (!whole(mem, -9999, 9999, &n)) {
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "rit_hz: must be between -9999 and 9999 Hz");
        }
        d[0] = 0x00;
        civ_u64_to_bcd_le((uint64_t)(n < 0 ? -n : n), &d[1], 2);
        d[3] = n < 0 ? 0x01 : 0x00;
        if (civ_local(c, 0x21, d, 4)) return CONSOLE_OK;
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "rit_hz: the %s has no RIT", rig->name);
    }
    if (name_is(mem, "tuner")) {
        if (!truth(mem, &on)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "tuner: must be true or false");
        d[0] = 0x01;
        d[1] = on ? 0x01 : 0x00;
        if (civ_local(c, 0x1C, d, 2)) return CONSOLE_OK;
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "tuner: the %s has no tuner over CAT", rig->name);
    }
    if (name_is(mem, "att")) {
        if (!truth(mem, &on)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "att: must be true or false");
        if (!rig_has(c, CIV_CAP_ATT) || !rig->att_on) {
            return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "att: the panel cannot switch the %s's attenuator; CAT can",
                          rig->name);
        }
        d[0] = on ? rig->att_on : 0x00;
        if (civ_local(c, 0x11, d, 1)) return CONSOLE_OK;
        return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "att: the radio refused");
    }
    if (name_is(mem, "ptt")) {
        if (!truth(mem, &on)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "ptt: must be true or false");
        *key = true; /* applied last, once nothing else in the change has been refused */
        *key_on = on;
        return CONSOLE_OK;
    }

    const civ_item_t *it = item_by_id(c, mem, &cmd);
    if (!it) {
        if (plain_name(mem->name, mem->name_len) || id_shaped(mem->name, mem->name_len)) {
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%.*s: the %s's panel has no such control",
                          (int)mem->name_len, mem->name, rig->name);
        }
        return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "the panel has no such control");
    }
    if (cmd == 0x14) {
        if (!whole(mem, 0, 255, &n)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s: must be 0 to 255", it->name);
        d[0] = it->sub;
        civ_u16_to_bcd_be((uint16_t)n, &d[1], 2);
        if (civ_local(c, 0x14, d, 3)) return CONSOLE_OK;
    } else if (cmd == 0x16) {
        if (!whole(mem, 0, func_max(it), &n) || !func_offers(it, (unsigned)n)) {
            return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s: the panel offers 0 to %u", it->name, func_max(it));
        }
        d[0] = it->sub;
        d[1] = (uint8_t)n;
        if (civ_local(c, 0x16, d, 2)) return CONSOLE_OK;
    } else {
        /* A meter: CAT only reads it, so the panel sets it directly. */
        int max = civ_meter_is_status(it->sub) ? 1 : 255;
        if (!whole(mem, 0, max, &n)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s: must be 0 to %d", it->name, max);
        if (civ_set_meter(c, it->sub, (uint16_t)n)) return CONSOLE_OK;
    }
    return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s: the radio refused", it->name);
}

console_result_t console_radio_apply(mock_ctl_t *m, const char *json, char *err, size_t err_cap)
{
    static const char malformed[] = "expected a JSON object of panel controls, like {\"freq\":7074000,\"mode\":\"LSB\"}";
    const char *p = json;
    bool first = true;
    bool key = false, key_on = false;
    jmember_t mem;
    int r;

    if (err && err_cap) err[0] = '\0';
    if (!m->civ) return refuse(CONSOLE_UNAVAILABLE, err, err_cap, "this mock has no radio to show");
    if (!j_open(&p)) return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);

    const civ_t before = *m->civ; /* all of the change, or none of it */
    while ((r = j_next(&p, &first, &mem)) == 1) {
        console_result_t res = radio_put(m, &mem, &key, &key_on, err, err_cap);
        if (res != CONSOLE_OK) {
            *m->civ = before;
            return res;
        }
    }
    if (r < 0) {
        *m->civ = before;
        return refuse(CONSOLE_BAD_REQUEST, err, err_cap, "%s", malformed);
    }
    if (key) ae_set_panel_ptt(m->engine, key_on);
    return CONSOLE_OK;
}

size_t console_error_json(const char *message, char *buf, size_t cap)
{
    out_t o;
    out_init(&o, buf, cap);
    out_f(&o, "{\"error\":");
    out_str(&o, message ? message : "");
    out_f(&o, "}");
    return out_done(&o);
}

size_t console_strings_json(const char *const *keys, const char *const *values, size_t n, char *buf, size_t cap)
{
    out_t o;
    out_init(&o, buf, cap);
    out_f(&o, "{");
    for (size_t i = 0; i < n; i++) {
        out_f(&o, "%s\"%s\":", i ? "," : "", keys[i]);
        out_str(&o, values[i] ? values[i] : "");
    }
    out_f(&o, "}");
    return out_done(&o);
}

/* ---- level meters ------------------------------------------------------------------- */

static int32_t to_cdb(float db)
{
    return (int32_t)lroundf(db * 100.0f);
}

void console_meter_init(console_meter_t *m)
{
    dsp_meter_reset(&m->acc);
    atomic_store(&m->rms_cdb, to_cdb(DSP_SILENCE_DBFS));
    atomic_store(&m->peak_cdb, to_cdb(DSP_SILENCE_DBFS));
    atomic_store(&m->seq, 0u);
}

void console_meter_add(console_meter_t *m, const int16_t *samples, size_t n)
{
    while (n > 0) {
        size_t room = CONSOLE_METER_WINDOW - m->acc.count;
        size_t take = n < room ? n : room;
        if (samples) {
            dsp_meter_add(&m->acc, samples, take);
            samples += take;
        } else {
            m->acc.count += (uint32_t)take;
        }
        n -= take;
        if (m->acc.count >= CONSOLE_METER_WINDOW) {
            atomic_store(&m->rms_cdb, to_cdb(dsp_meter_rms_dbfs(&m->acc)));
            atomic_store(&m->peak_cdb, to_cdb(dsp_meter_peak_dbfs(&m->acc)));
            atomic_fetch_add(&m->seq, 1u);
            dsp_meter_reset(&m->acc);
        }
    }
}

void console_meter_read(console_meter_t *m, uint32_t *seen, console_level_t *out)
{
    uint32_t seq = atomic_load(&m->seq);
    out->active = seq != *seen;
    *seen = seq;
    out->rms_cdb = atomic_load(&m->rms_cdb);
    out->peak_cdb = atomic_load(&m->peak_cdb);
}

/* ---- the waterfall ------------------------------------------------------------------- */

void console_scope_init(console_scope_t *s)
{
    memset(s->ring, 0, sizeof s->ring);
    atomic_store(&s->written, 0u);
}

void console_scope_add(console_scope_t *s, const int16_t *samples, size_t n)
{
    uint32_t w = atomic_load(&s->written);
    if (n > CONSOLE_SCOPE_RING) { /* only the newest can stay */
        if (samples) samples += n - CONSOLE_SCOPE_RING;
        w += (uint32_t)(n - CONSOLE_SCOPE_RING);
        n = CONSOLE_SCOPE_RING;
    }
    size_t at = w % CONSOLE_SCOPE_RING;
    size_t first = n < CONSOLE_SCOPE_RING - at ? n : CONSOLE_SCOPE_RING - at;
    if (samples) {
        memcpy(&s->ring[at], samples, first * sizeof(int16_t));
        memcpy(&s->ring[0], samples + first, (n - first) * sizeof(int16_t));
    } else {
        memset(&s->ring[at], 0, first * sizeof(int16_t));
        memset(&s->ring[0], 0, (n - first) * sizeof(int16_t));
    }
    atomic_store(&s->written, w + (uint32_t)n);
}

bool console_scope_take(console_scope_t *s, uint32_t *seen, int16_t *out)
{
    uint32_t w = atomic_load(&s->written);
    bool fresh = w != *seen;
    *seen = w;
    if (!fresh || w < DSP_SPEC_INPUT) return false; /* a count that wrapped after a day reads short once */

    size_t at = (w - DSP_SPEC_INPUT) % CONSOLE_SCOPE_RING;
    size_t first = DSP_SPEC_INPUT < CONSOLE_SCOPE_RING - at ? DSP_SPEC_INPUT : CONSOLE_SCOPE_RING - at;
    memcpy(out, &s->ring[at], first * sizeof(int16_t));
    memcpy(out + first, &s->ring[0], (DSP_SPEC_INPUT - first) * sizeof(int16_t));

    /* The audio task may have gone on writing meanwhile. Only if it has come
     * right round to what was being copied is the copy no good. */
    return atomic_load(&s->written) - w <= CONSOLE_SCOPE_RING - DSP_SPEC_INPUT;
}

void console_scope_work_init(console_scope_work_t *w)
{
    dsp_spec_init(&w->spec);
    w->rx_seen = w->tx_seen = 0;
}

static bool scope_lane(console_scope_work_t *w, console_scope_t *s, uint32_t *seen, uint8_t *out)
{
    if (!s || !console_scope_take(s, seen, w->pcm)) return false;
    dsp_spec_run(&w->spec, w->pcm, w->db);
    for (int k = 0; k < DSP_SPEC_BINS; k++) {
        float half_db = -2.0f * w->db[k];
        out[k] = half_db <= 0.0f ? 0 : half_db >= 255.0f ? 255 : (uint8_t)lroundf(half_db);
    }
    return true;
}

size_t console_scope_row(console_scope_work_t *w, console_scope_t *rx, console_scope_t *tx, bool keyed, uint8_t *out,
                         size_t cap)
{
    if (cap < CONSOLE_SCOPE_ROW_MAX) return 0;
    size_t len = 4;
    uint8_t flags = keyed ? CONSOLE_SCOPE_KEYED : 0;
    if (scope_lane(w, rx, &w->rx_seen, &out[len])) {
        flags |= CONSOLE_SCOPE_RX;
        len += DSP_SPEC_BINS;
    }
    if (scope_lane(w, tx, &w->tx_seen, &out[len])) {
        flags |= CONSOLE_SCOPE_TX;
        len += DSP_SPEC_BINS;
    }
    out[0] = CONSOLE_SCOPE_FORMAT;
    out[1] = flags;
    out[2] = DSP_SPEC_BINS;
    out[3] = 0;
    return len;
}

/* ---- telemetry ---------------------------------------------------------------------- */

void console_mode_name(uint8_t mode, bool data, char *out, size_t cap)
{
    static const struct {
        uint8_t mode;
        const char *name;
    } modes[] = {
        {0x00, "LSB"}, {0x01, "USB"}, {0x02, "AM"},   {0x03, "CW"},     {0x04, "RTTY"},
        {0x05, "FM"},  {0x06, "WFM"}, {0x07, "CW-R"}, {0x08, "RTTY-R"}, {0x17, "DV"},
    };
    for (size_t i = 0; i < ARRAY_LEN(modes); i++) {
        if (modes[i].mode == mode) {
            snprintf(out, cap, "%s%s", modes[i].name, data ? "-D" : "");
            return;
        }
    }
    snprintf(out, cap, "%02X%s", mode, data ? "-D" : "");
}

static void out_level(out_t *o, const console_level_t *lvl)
{
    if (!lvl->active) {
        out_f(o, "null");
        return;
    }
    out_f(o, "{\"rms\":");
    out_cdb(o, lvl->rms_cdb);
    out_f(o, ",\"peak\":");
    out_cdb(o, lvl->peak_cdb);
    out_f(o, "}");
}

size_t console_telemetry_json(mock_ctl_t *m, const console_sample_t *s, char *buf, size_t cap)
{
    static const char *const parrot_states[] = {"idle", "recording", "waiting", "playing"};
    out_t o;
    ae_ptt_t ptt;
    ae_status_t st;
    ae_parrot_status_t par;
    char mode[16];

    ae_get_ptt(m->engine, &ptt);
    ae_get_status(m->engine, &st);
    ae_get_parrot_status(m->engine, &par);
    console_mode_name(s->mode, s->data, mode, sizeof mode);

    out_init(&o, buf, cap);
    const civ_rig_t *rig = mock_ctl_rig(m);
    out_f(&o, "{\"t\":%lu,\"gen\":%lu,\"rgen\":%lu,\"hamlib\":%d,\"kept\":", (unsigned long)s->uptime_ms,
          (unsigned long)ae_config_gen(m->engine), (unsigned long)s->radio_gen, rig ? rig->hamlib_model : 0);
    out_bool(&o, mock_ctl_settings_kept(m));
    out_f(&o, ",\"host\":");
    out_bool(&o, s->host);

    out_f(&o, ",\"ptt\":{\"on\":");
    out_bool(&o, ae_ptt_keyed(&ptt));
    out_f(&o, ",\"cat\":");
    out_bool(&o, ptt.cat);
    out_f(&o, ",\"line\":");
    out_bool(&o, ptt.line);
    out_f(&o, ",\"vox\":");
    out_bool(&o, ptt.vox);
    out_f(&o, ",\"panel\":");
    out_bool(&o, ptt.panel);

    out_f(&o, "},\"cat\":{\"freq\":%llu,\"txfreq\":%llu,\"mode\":\"%s\",\"split\":", (unsigned long long)s->freq_hz,
          (unsigned long long)s->tx_freq_hz, mode);
    out_bool(&o, s->split);
    out_f(&o, ",\"ok\":%lu,\"ng\":%lu,\"dropped\":%lu", (unsigned long)s->frames_ok, (unsigned long)s->frames_ng,
          (unsigned long)s->frames_dropped);

    out_f(&o, "},\"audio\":{\"tx\":");
    out_level(&o, &s->tx);
    out_f(&o, ",\"rx\":");
    out_level(&o, &s->rx);
    out_f(&o, ",\"fifo\":{\"level\":%lu,\"target\":%lu,\"over\":%lu,\"under\":%lu,\"dropped\":%lu,\"repeated\":%lu}",
          (unsigned long)s->fifo_level, (unsigned long)s->fifo_target, (unsigned long)s->fifo_overruns,
          (unsigned long)s->fifo_underruns, (unsigned long)s->fifo_dropped, (unsigned long)s->fifo_repeated);

    /* The receive channel: the level it measured on its source and the gain
     * it applies to reach the SNR. */
    out_f(&o, "},\"channel\":{\"muted\":");
    out_bool(&o, st.rx_muted);
    out_f(&o, ",\"level\":");
    if (st.signal_dbfs > DSP_SILENCE_DBFS) {
        out_cdb(&o, to_cdb(st.signal_dbfs));
    } else {
        out_f(&o, "null");
    }
    out_f(&o, ",\"gain\":");
    out_cdb(&o, to_cdb(st.gain_db));
    out_f(&o, ",\"limited\":");
    out_bool(&o, st.gain_limited);

    out_f(&o, "},\"parrot\":{\"state\":\"%s\",\"recorded\":",
          (size_t)par.state < ARRAY_LEN(parrot_states) ? parrot_states[par.state] : "idle");
    out_scaled(&o, (int32_t)((par.recorded_ms + 50) / 100), 10);
    out_f(&o, ",\"capacity\":");
    out_scaled(&o, (int32_t)((par.capacity_ms + 50) / 100), 10);
    out_f(&o, ",\"truncated\":");
    out_bool(&o, par.truncated);

    out_f(&o, "},\"clips\":");
    mock_ctl_clips_status_t clips;
    if (mock_ctl_clips_status(m, &clips)) {
        out_f(&o, "{\"busy\":");
        out_bool(&o, clips.busy);
        out_f(&o, ",\"failed\":");
        out_bool(&o, clips.failed);
        out_f(&o, ",\"seconds\":[");
        for (unsigned i = 0; i < clips.slots; i++) {
            out_f(&o, "%s", i ? "," : "");
            out_scaled(&o, (int32_t)((clips.length_ms[i] + 50) / 100), 10);
        }
        out_f(&o, "]}");
    } else {
        out_f(&o, "null");
    }

    out_f(&o, ",\"link\":");
    if (s->has_link) {
        static const char *const link_states[] = {"off", "no peer", "idle", "sending", "receiving", "off frequency"};
        const link_status_t *k = &s->link;
        out_f(&o, "{\"state\":\"%s\",\"peer\":", (size_t)k->state < ARRAY_LEN(link_states) ? link_states[k->state] : "off");
        if (k->peer) {
            out_str(&o, k->peer_name);
            out_f(&o, ",\"peer_rx\":%lu,\"peer_tx\":%lu,\"peer_keyed\":", (unsigned long)k->peer_rx_hz,
                  (unsigned long)k->peer_tx_hz);
            out_bool(&o, k->peer_keyed);
        } else {
            out_f(&o, "null,\"peer_rx\":null,\"peer_tx\":null,\"peer_keyed\":false");
        }
        out_f(&o, ",\"rssi\":");
        if (k->peer && k->has_rssi) {
            out_f(&o, "%d", k->rssi_dbm);
        } else {
            out_f(&o, "null");
        }
        out_f(&o,
              ",\"buffer\":%lu,\"target\":%lu,\"sent\":%lu,\"send_failed\":%lu,\"received\":%lu,\"lost\":%lu,"
              "\"late\":%lu,\"off_frequency\":%lu,\"underruns\":%lu,\"overruns\":%lu,\"dropped\":%lu,\"repeated\":%lu",
              (unsigned long)k->buffer_ms, (unsigned long)k->target_ms, (unsigned long)k->sent,
              (unsigned long)k->send_failed, (unsigned long)k->received, (unsigned long)k->lost, (unsigned long)k->late,
              (unsigned long)k->off_frequency, (unsigned long)k->underruns, (unsigned long)k->overruns,
              (unsigned long)k->dropped, (unsigned long)k->repeated);
        out_f(&o, ",\"shared\":");
        out_bool(&o, s->link_shared);
        out_f(&o, ",\"leads\":");
        out_bool(&o, s->link_leads);
        out_f(&o, "}");
    } else {
        out_f(&o, "null");
    }

    out_f(&o, ",\"rssi\":");
    if (s->has_rssi) {
        out_f(&o, "%d", s->rssi_dbm);
    } else {
        out_f(&o, "null");
    }
    out_f(&o, "}");
    return out_done(&o);
}

/* ---- the board's health ---------------------------------------------------------------- */

void console_cost_init(console_cost_t *c)
{
    c->sum_us = c->max_us = c->samples = c->blocks = 0;
    atomic_store(&c->avg_us, 0u);
    atomic_store(&c->peak_us, 0u);
    atomic_store(&c->load_permille, 0u);
    atomic_store(&c->block_samples, 0u);
    atomic_store(&c->worst_us, 0u);
    atomic_store(&c->total_blocks, 0u);
    atomic_store(&c->windows, 0u);
}

void console_cost_add(console_cost_t *c, uint32_t us, size_t n)
{
    if (n == 0) return;
    c->sum_us = us > UINT32_MAX - c->sum_us ? UINT32_MAX : c->sum_us + us;
    if (us > c->max_us) c->max_us = us;
    c->samples += (uint32_t)n;
    c->blocks++;
    if (us > atomic_load(&c->worst_us)) atomic_store(&c->worst_us, us);
    atomic_store(&c->block_samples, (uint32_t)n);
    atomic_fetch_add(&c->total_blocks, 1u);

    if (c->samples >= CONSOLE_COST_WINDOW) {
        /* The samples last samples / 48 ms: the load is the time taken over that. */
        uint64_t load = (uint64_t)c->sum_us * 48u / c->samples;
        atomic_store(&c->avg_us, c->sum_us / c->blocks);
        atomic_store(&c->peak_us, c->max_us);
        atomic_store(&c->load_permille, load > 9999u ? 9999u : (uint32_t)load);
        atomic_fetch_add(&c->windows, 1u);
        c->sum_us = c->max_us = c->samples = c->blocks = 0;
    }
}

void console_cost_read(console_cost_t *c, console_health_t *out)
{
    out->cost_blocks = atomic_load(&c->total_blocks);
    out->cost_block_samples = atomic_load(&c->block_samples);
    out->cost_avg_us = atomic_load(&c->avg_us);
    out->cost_peak_us = atomic_load(&c->peak_us);
    out->cost_worst_us = atomic_load(&c->worst_us);
    out->cost_load_permille = atomic_load(&c->load_permille);
    out->has_cost = atomic_load(&c->windows) > 0;
}

size_t console_health_json(const console_health_t *h, char *buf, size_t cap)
{
    out_t o;
    out_init(&o, buf, cap);
    out_f(&o, "{\"t\":%lu,\"reset\":", (unsigned long)h->uptime_ms);
    if (h->reset) {
        out_str(&o, h->reset);
    } else {
        out_f(&o, "null");
    }
    out_f(&o, ",\"heap\":{\"free\":%lu,\"min\":%lu,\"largest\":%lu},\"psram\":", (unsigned long)h->heap_free,
          (unsigned long)h->heap_min, (unsigned long)h->heap_largest);
    if (h->has_psram) {
        out_f(&o, "%lu", (unsigned long)h->psram_free);
    } else {
        out_f(&o, "null");
    }

    out_f(&o, ",\"audio\":");
    if (h->has_cost) {
        out_f(&o, "{\"blocks\":%lu,\"block\":%lu,\"avg\":%lu,\"max\":%lu,\"worst\":%lu,\"load\":",
              (unsigned long)h->cost_blocks, (unsigned long)h->cost_block_samples, (unsigned long)h->cost_avg_us,
              (unsigned long)h->cost_peak_us, (unsigned long)h->cost_worst_us);
        out_scaled(&o, (int32_t)h->cost_load_permille, 10);
        out_f(&o, "}");
    } else {
        out_f(&o, "null");
    }

    out_f(&o, ",\"wifi\":");
    if (h->has_wifi) {
        out_f(&o, "{\"drops\":%lu}", (unsigned long)h->wifi_drops);
    } else {
        out_f(&o, "null");
    }

    out_f(&o, ",\"cpu\":");
    if (h->n_cores) {
        unsigned cores = h->n_cores < CONSOLE_HEALTH_CORES ? h->n_cores : CONSOLE_HEALTH_CORES;
        for (unsigned i = 0; i < cores; i++) {
            out_f(&o, "%s", i ? "," : "[");
            out_scaled(&o, (int32_t)h->busy_permille[i], 10);
        }
        out_f(&o, "]");
    } else {
        out_f(&o, "null");
    }

    out_f(&o, ",\"running\":%lu,\"tasks\":", (unsigned long)h->running);
    if (h->has_tasks) {
        out_f(&o, "{\"age\":%lu,\"took\":%lu,\"missed\":%lu,\"list\":[", (unsigned long)h->tasks_age_ms,
              (unsigned long)h->tasks_took_us, (unsigned long)h->tasks_missed);
        size_t n = h->n_tasks < CONSOLE_HEALTH_TASKS ? h->n_tasks : CONSOLE_HEALTH_TASKS;
        for (size_t i = 0; i < n; i++) {
            const console_task_t *t = &h->tasks[i];
            char name[CONSOLE_TASK_NAME_MAX + 1];
            memcpy(name, t->name, CONSOLE_TASK_NAME_MAX); /* may fill its field, with no zero after it */
            name[CONSOLE_TASK_NAME_MAX] = '\0';
            out_f(&o, "%s{\"name\":", i ? "," : "");
            out_str(&o, name);
            out_f(&o, ",\"stack\":%lu,\"cpu\":", (unsigned long)t->stack_free);
            if (t->has_cpu) {
                out_scaled(&o, (int32_t)t->cpu_permille, 10);
            } else {
                out_f(&o, "null");
            }
            out_f(&o, "}");
        }
        out_f(&o, "]}");
    } else {
        out_f(&o, "null");
    }
    out_f(&o, "}");
    return out_done(&o);
}
