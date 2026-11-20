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
    out_f(&o, "{\"t\":%lu,\"gen\":%lu,\"hamlib\":%d,\"kept\":", (unsigned long)s->uptime_ms,
          (unsigned long)ae_config_gen(m->engine), rig ? rig->hamlib_model : 0);
    out_bool(&o, mock_ctl_settings_kept(m));
    out_f(&o, ",\"host\":");
    out_bool(&o, s->host);

    out_f(&o, ",\"ptt\":{\"on\":");
    out_bool(&o, ptt.cat || ptt.line || ptt.vox);
    out_f(&o, ",\"cat\":");
    out_bool(&o, ptt.cat);
    out_f(&o, ",\"line\":");
    out_bool(&o, ptt.line);
    out_f(&o, ",\"vox\":");
    out_bool(&o, ptt.vox);

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
