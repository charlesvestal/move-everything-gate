/*
 * GATE Audio FX Plugin - Noise Gate & Downward Expander
 *
 * Two modes:
 *   GATE   - Hard noise gate with configurable range (floor attenuation)
 *   EXPAND - Downward expander with configurable ratio
 *
 * Uses envelope follower + state machine with hysteresis for clean gating.
 *
 * Parameters:
 *   threshold  - Detection threshold in dB (-80 to 0)
 *   attack     - Gate open time in ms (0.1 to 50)
 *   hold       - Hold time before release in ms (0 to 500)
 *   release    - Gate close time in ms (10 to 1000)
 *   range      - Max attenuation in dB for gate mode (0 to 80)
 *   ratio      - Expansion ratio for expander mode (1 to 20)
 *   hysteresis - Gap between open/close thresholds in dB (0 to 12)
 *   mode       - GATE or EXPAND
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "audio_fx_api_v1.h"

#define SAMPLE_RATE 44100.0f

/* Gate states */
enum {
    GATE_CLOSED = 0,
    GATE_OPEN,
    GATE_HOLD,
    GATE_CLOSING
};

/* Mode */
enum {
    MODE_GATE = 0,
    MODE_EXPAND
};

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

static inline float linear_to_db(float lin) {
    if (lin < 1e-10f) return -200.0f;
    return 20.0f * log10f(lin);
}

/* ---- JSON helper ---- */
static int json_get_number(const char *json, const char *key, float *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (!*p) return -1;
    *out = strtof(p, NULL);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p && *p != '"') p++;
    if (*p != '"') return -1;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    int len = (int)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* ---- Instance state ---- */
typedef struct {
    char module_dir[256];

    /* Parameters */
    float threshold_db;   /* -80 to 0 */
    float attack_ms;      /* 0.1 to 50 */
    float hold_ms;        /* 0 to 500 */
    float release_ms;     /* 10 to 1000 */
    float range_db;       /* 0 to 80 */
    float ratio;          /* 1 to 20 */
    float hysteresis_db;  /* 0 to 12 */
    int   mode;           /* MODE_GATE or MODE_EXPAND */

    /* Gate state */
    int   gate_state;
    float gate_gain;      /* Current gain multiplier (0..1) */
    int   hold_counter;

    /* Envelope follower */
    float envelope;

} gate_instance_t;

/* ---- Globals ---- */
static const host_api_v1_t *g_host = NULL;

static void gate_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[GATE] %s", msg);
        g_host->log(buf);
    }
}

/* ---- Instance lifecycle ---- */

static void* gate_create_instance(const char *module_dir, const char *config_json) {
    gate_log("Creating instance");

    gate_instance_t *inst = (gate_instance_t*)calloc(1, sizeof(gate_instance_t));
    if (!inst) {
        gate_log("Failed to allocate instance");
        return NULL;
    }

    if (module_dir) {
        strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    }

    /* Defaults */
    inst->threshold_db  = -40.0f;
    inst->attack_ms     = 3.0f;
    inst->hold_ms       = 50.0f;
    inst->release_ms    = 200.0f;
    inst->range_db      = 80.0f;
    inst->ratio         = 4.0f;
    inst->hysteresis_db = 4.0f;
    inst->mode          = MODE_GATE;

    inst->gate_state    = GATE_CLOSED;
    inst->gate_gain     = 0.0f;
    inst->hold_counter  = 0;
    inst->envelope      = 0.0f;

    gate_log("Instance created");
    return inst;
}

static void gate_destroy_instance(void *instance) {
    gate_instance_t *inst = (gate_instance_t*)instance;
    if (!inst) return;
    gate_log("Destroying instance");
    free(inst);
}

/* ---- Audio processing ---- */

static void gate_process_block(void *instance, int16_t *audio_inout, int frames) {
    gate_instance_t *inst = (gate_instance_t*)instance;
    if (!inst) return;

    /* Pre-compute coefficients */
    float open_thresh  = db_to_linear(inst->threshold_db);
    float close_thresh = db_to_linear(inst->threshold_db - inst->hysteresis_db);

    /* Envelope follower: fast attack (1ms), moderate release (50ms) */
    float env_attack  = 1.0f - expf(-1.0f / (1.0f * 0.001f * SAMPLE_RATE));
    float env_release = 1.0f - expf(-1.0f / (50.0f * 0.001f * SAMPLE_RATE));

    /* Gate attack/release as linear ramp steps per sample */
    float attack_step = (inst->attack_ms > 0.0f) ?
        1.0f / (inst->attack_ms * 0.001f * SAMPLE_RATE) : 1.0f;
    float release_step = (inst->release_ms > 0.0f) ?
        1.0f / (inst->release_ms * 0.001f * SAMPLE_RATE) : 1.0f;

    int hold_samples = (int)(inst->hold_ms * 0.001f * SAMPLE_RATE);

    /* Gate mode floor */
    float gate_floor = (inst->range_db >= 79.0f) ? 0.0f : db_to_linear(-inst->range_db);

    for (int i = 0; i < frames; i++) {
        float L = (float)audio_inout[i * 2];
        float R = (float)audio_inout[i * 2 + 1];

        /* Peak detection (stereo max) */
        float peak = fabsf(L);
        float peak_r = fabsf(R);
        if (peak_r > peak) peak = peak_r;
        float peak_norm = peak / 32768.0f;

        /* Envelope follower */
        if (peak_norm > inst->envelope) {
            inst->envelope += env_attack * (peak_norm - inst->envelope);
        } else {
            inst->envelope += env_release * (peak_norm - inst->envelope);
        }

        /* State machine */
        switch (inst->gate_state) {
        case GATE_OPEN:
            inst->gate_gain += attack_step;
            if (inst->gate_gain > 1.0f) inst->gate_gain = 1.0f;
            if (inst->envelope < close_thresh) {
                inst->gate_state = GATE_HOLD;
                inst->hold_counter = hold_samples;
            }
            break;

        case GATE_HOLD:
            inst->hold_counter--;
            if (inst->envelope > open_thresh) {
                inst->gate_state = GATE_OPEN;
            } else if (inst->hold_counter <= 0) {
                inst->gate_state = GATE_CLOSING;
            }
            break;

        case GATE_CLOSING:
            if (inst->mode == MODE_GATE) {
                inst->gate_gain -= release_step;
                if (inst->gate_gain <= gate_floor) {
                    inst->gate_gain = gate_floor;
                    inst->gate_state = GATE_CLOSED;
                }
            } else {
                /* Expander mode: ratio-based gain reduction */
                inst->gate_gain -= release_step;
                float min_gain = 0.0f;
                if (inst->envelope > 1e-10f && open_thresh > 1e-10f) {
                    float below_db = linear_to_db(inst->envelope) - linear_to_db(open_thresh);
                    if (below_db < 0.0f) {
                        float reduced_db = below_db * (1.0f - 1.0f / inst->ratio);
                        min_gain = db_to_linear(reduced_db);
                        if (min_gain > 1.0f) min_gain = 1.0f;
                    } else {
                        min_gain = 1.0f;
                    }
                }
                if (inst->gate_gain < min_gain) inst->gate_gain = min_gain;
                if (inst->gate_gain <= min_gain && min_gain < gate_floor) {
                    inst->gate_state = GATE_CLOSED;
                }
            }
            if (inst->envelope > open_thresh) {
                inst->gate_state = GATE_OPEN;
            }
            break;

        case GATE_CLOSED:
            if (inst->mode == MODE_GATE) {
                inst->gate_gain = gate_floor;
            } else {
                /* Expander: continuously compute ratio-based gain */
                if (inst->envelope > 1e-10f && open_thresh > 1e-10f) {
                    float below_db = linear_to_db(inst->envelope) - linear_to_db(open_thresh);
                    if (below_db < 0.0f) {
                        float reduced_db = below_db * (1.0f - 1.0f / inst->ratio);
                        float target = db_to_linear(reduced_db);
                        if (target > 1.0f) target = 1.0f;
                        /* Smooth toward target */
                        float smooth = 1.0f - expf(-1.0f / (10.0f * 0.001f * SAMPLE_RATE));
                        inst->gate_gain += smooth * (target - inst->gate_gain);
                    }
                }
            }
            if (inst->envelope > open_thresh) {
                inst->gate_state = GATE_OPEN;
            }
            break;
        }

        /* Apply gain */
        float gain = inst->gate_gain;
        audio_inout[i * 2]     = (int16_t)(L * gain);
        audio_inout[i * 2 + 1] = (int16_t)(R * gain);
    }
}

/* ---- Parameter handling ---- */

static void gate_set_param(void *instance, const char *key, const char *val) {
    gate_instance_t *inst = (gate_instance_t*)instance;
    if (!inst) return;

    float v = atof(val);

    if (strcmp(key, "threshold") == 0) {
        inst->threshold_db = clampf(v, -80.0f, 0.0f);
    } else if (strcmp(key, "attack") == 0) {
        inst->attack_ms = clampf(v, 0.1f, 50.0f);
    } else if (strcmp(key, "hold") == 0) {
        inst->hold_ms = clampf(v, 0.0f, 500.0f);
    } else if (strcmp(key, "release") == 0) {
        inst->release_ms = clampf(v, 10.0f, 1000.0f);
    } else if (strcmp(key, "range") == 0) {
        inst->range_db = clampf(v, 0.0f, 80.0f);
    } else if (strcmp(key, "ratio") == 0) {
        inst->ratio = clampf(v, 1.0f, 20.0f);
    } else if (strcmp(key, "hysteresis") == 0) {
        inst->hysteresis_db = clampf(v, 0.0f, 12.0f);
    } else if (strcmp(key, "mode") == 0) {
        if (strcmp(val, "GATE") == 0) inst->mode = MODE_GATE;
        else if (strcmp(val, "EXPAND") == 0) inst->mode = MODE_EXPAND;
    } else if (strcmp(key, "state") == 0) {
        /* Restore all parameters from JSON state */
        float fval;
        char sval[32];
        if (json_get_number(val, "threshold", &fval) == 0)
            inst->threshold_db = clampf(fval, -80.0f, 0.0f);
        if (json_get_number(val, "attack", &fval) == 0)
            inst->attack_ms = clampf(fval, 0.1f, 50.0f);
        if (json_get_number(val, "hold", &fval) == 0)
            inst->hold_ms = clampf(fval, 0.0f, 500.0f);
        if (json_get_number(val, "release", &fval) == 0)
            inst->release_ms = clampf(fval, 10.0f, 1000.0f);
        if (json_get_number(val, "range", &fval) == 0)
            inst->range_db = clampf(fval, 0.0f, 80.0f);
        if (json_get_number(val, "ratio", &fval) == 0)
            inst->ratio = clampf(fval, 1.0f, 20.0f);
        if (json_get_number(val, "hysteresis", &fval) == 0)
            inst->hysteresis_db = clampf(fval, 0.0f, 12.0f);
        if (json_get_string(val, "mode", sval, sizeof(sval)) == 0) {
            if (strcmp(sval, "EXPAND") == 0) inst->mode = MODE_EXPAND;
            else inst->mode = MODE_GATE;
        }
    }
}

static int gate_get_param(void *instance, const char *key, char *buf, int buf_len) {
    gate_instance_t *inst = (gate_instance_t*)instance;
    if (!inst) return -1;

    if (strcmp(key, "threshold") == 0)
        return snprintf(buf, buf_len, "%.0f", inst->threshold_db);
    if (strcmp(key, "attack") == 0)
        return snprintf(buf, buf_len, "%.1f", inst->attack_ms);
    if (strcmp(key, "hold") == 0)
        return snprintf(buf, buf_len, "%.0f", inst->hold_ms);
    if (strcmp(key, "release") == 0)
        return snprintf(buf, buf_len, "%.0f", inst->release_ms);
    if (strcmp(key, "range") == 0)
        return snprintf(buf, buf_len, "%.0f", inst->range_db);
    if (strcmp(key, "ratio") == 0)
        return snprintf(buf, buf_len, "%.1f", inst->ratio);
    if (strcmp(key, "hysteresis") == 0)
        return snprintf(buf, buf_len, "%.1f", inst->hysteresis_db);
    if (strcmp(key, "mode") == 0)
        return snprintf(buf, buf_len, "%s", inst->mode == MODE_EXPAND ? "EXPAND" : "GATE");
    if (strcmp(key, "name") == 0)
        return snprintf(buf, buf_len, "GATE");

    /* State save */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"threshold\":%.1f,\"attack\":%.1f,\"hold\":%.1f,"
            "\"release\":%.1f,\"range\":%.1f,\"ratio\":%.1f,"
            "\"hysteresis\":%.1f,\"mode\":\"%s\"}",
            inst->threshold_db, inst->attack_ms, inst->hold_ms,
            inst->release_ms, inst->range_db, inst->ratio,
            inst->hysteresis_db,
            inst->mode == MODE_EXPAND ? "EXPAND" : "GATE");
    }

    /* UI hierarchy for shadow parameter editor */
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"threshold\",\"attack\",\"hold\",\"release\",\"range\",\"ratio\",\"hysteresis\",\"mode\"],"
                    "\"params\":[\"threshold\",\"attack\",\"hold\",\"release\",\"range\",\"ratio\",\"hysteresis\",\"mode\"]"
                "}"
            "}"
        "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    /* Chain params metadata */
    if (strcmp(key, "chain_params") == 0) {
        const char *params_json = "["
            "{\"key\":\"threshold\",\"name\":\"Threshold\",\"type\":\"float\",\"min\":-80,\"max\":0,\"default\":-40,\"step\":1,\"unit\":\"dB\"},"
            "{\"key\":\"attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0.1,\"max\":50,\"default\":3,\"step\":0.1,\"unit\":\"ms\"},"
            "{\"key\":\"hold\",\"name\":\"Hold\",\"type\":\"float\",\"min\":0,\"max\":500,\"default\":50,\"step\":1,\"unit\":\"ms\"},"
            "{\"key\":\"release\",\"name\":\"Release\",\"type\":\"float\",\"min\":10,\"max\":1000,\"default\":200,\"step\":1,\"unit\":\"ms\"},"
            "{\"key\":\"range\",\"name\":\"Range\",\"type\":\"float\",\"min\":0,\"max\":80,\"default\":80,\"step\":1,\"unit\":\"dB\"},"
            "{\"key\":\"ratio\",\"name\":\"Ratio\",\"type\":\"float\",\"min\":1,\"max\":20,\"default\":4,\"step\":0.5,\"unit\":\":1\"},"
            "{\"key\":\"hysteresis\",\"name\":\"Hysteresis\",\"type\":\"float\",\"min\":0,\"max\":12,\"default\":4,\"step\":0.5,\"unit\":\"dB\"},"
            "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"GATE\",\"EXPAND\"],\"default\":\"GATE\"}"
        "]";
        int len = strlen(params_json);
        if (len < buf_len) {
            strcpy(buf, params_json);
            return len;
        }
        return -1;
    }

    return -1;
}

/* ---- Plugin entry point ---- */

static audio_fx_api_v2_t g_fx_api_v2;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version      = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = gate_create_instance;
    g_fx_api_v2.destroy_instance = gate_destroy_instance;
    g_fx_api_v2.process_block    = gate_process_block;
    g_fx_api_v2.set_param        = gate_set_param;
    g_fx_api_v2.get_param        = gate_get_param;

    gate_log("GATE v2 plugin initialized");

    return &g_fx_api_v2;
}
