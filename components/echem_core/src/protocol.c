#include "echem_core/protocol.h"
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helper: escape a string for safe JSON embedding.
 * Escapes '"' → '\"', '\' → '\\', and ASCII control chars → \uXXXX.
 * Writes at most (out_len - 1) chars plus a null terminator.
 * -------------------------------------------------------------------------- */
static void json_escape_into(char *out, size_t out_len, const char *in)
{
    if (!out || out_len == 0) return;
    if (!in) { out[0] = '\0'; return; }

    size_t w = 0;
    while (*in) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\') {
            if (w + 3 > out_len) break;  /* 2 chars + future null */
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c < 0x20) {
            if (w + 7 > out_len) break;  /* \uXXXX (6) + future null */
            int written = snprintf(out + w, out_len - w, "\\u%04x", (unsigned)c);
            if (written > 0) w += (size_t)written;
        } else {
            if (w + 2 > out_len) break;  /* 1 char + future null */
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
}

int protocol_format_point(char *buf, size_t buf_len,
                           uint8_t electrode, uint16_t idx,
                           float E_mV, float I_uA, float RE_mV)
{
    int n = snprintf(buf, buf_len,
        "{\"t\":\"point\",\"e\":%u,\"n\":%u,\"v\":%.4f,\"i\":%.4f,\"re\":%.4f}\n",
        (unsigned)electrode,
        (unsigned)idx,
        (double)(E_mV  / 1000.0f),
        (double)I_uA,
        (double)(RE_mV / 1000.0f));
    if (n < 0 || (size_t)n >= buf_len) return -1;
    return n;
}

int protocol_format_event_started(char *buf, size_t buf_len,
                                   const char *technique, uint8_t electrode)
{
    char esc_tech[32];
    json_escape_into(esc_tech, sizeof(esc_tech), technique ? technique : "");
    int n = snprintf(buf, buf_len,
        "{\"t\":\"event\",\"name\":\"scan_started\",\"mode\":\"%s\",\"e\":%u}\n",
        esc_tech, (unsigned)electrode);
    if (n < 0 || (size_t)n >= buf_len) return -1;
    return n;
}

int protocol_format_event_complete(char *buf, size_t buf_len)
{
    int n = snprintf(buf, buf_len,
        "{\"t\":\"event\",\"name\":\"scan_complete\"}\n");
    if (n < 0 || (size_t)n >= buf_len) return -1;
    return n;
}

int protocol_format_event_error(char *buf, size_t buf_len, const char *msg)
{
    char esc_msg[256];
    json_escape_into(esc_msg, sizeof(esc_msg), msg ? msg : "unknown error");
    int n = snprintf(buf, buf_len,
        "{\"t\":\"event\",\"name\":\"scan_error\",\"msg\":\"%s\"}\n",
        esc_msg);
    if (n < 0 || (size_t)n >= buf_len) return -1;
    return n;
}

int protocol_format_hello(char *buf, size_t buf_len)
{
    int n = snprintf(buf, buf_len,
        "{\"t\":\"hello\",\"fw\":\"%s\",\"proto\":%d}\n",
        UBIOPOT_FW_VERSION_STR,
        UBIOPOT_PROTOCOL_VERSION);
    if (n < 0 || (size_t)n >= buf_len) return -1;
    return n;
}
