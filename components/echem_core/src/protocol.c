#include "echem_core/protocol.h"
#include <stdio.h>
#include <string.h>

int protocol_format_point(char *buf, size_t buf_len,
                           uint8_t electrode, uint16_t idx,
                           float E_mV, float I_uA, float RE_mV)
{
    /* {"t":"point","e":1,"n":0,"v":-0.5000,"i":6.4200,"re":-0.4950} */
    return snprintf(buf, buf_len,
        "{\"t\":\"point\",\"e\":%u,\"n\":%u,\"v\":%.4f,\"i\":%.4f,\"re\":%.4f}\n",
        (unsigned)electrode,
        (unsigned)idx,
        (double)(E_mV  / 1000.0f),
        (double)I_uA,
        (double)(RE_mV / 1000.0f));
}

int protocol_format_event_started(char *buf, size_t buf_len,
                                   const char *technique, uint8_t electrode)
{
    return snprintf(buf, buf_len,
        "{\"t\":\"event\",\"name\":\"scan_started\",\"mode\":\"%s\",\"e\":%u}\n",
        technique, (unsigned)electrode);
}

int protocol_format_event_complete(char *buf, size_t buf_len)
{
    return snprintf(buf, buf_len,
        "{\"t\":\"event\",\"name\":\"scan_complete\"}\n");
}

int protocol_format_event_error(char *buf, size_t buf_len, const char *msg)
{
    return snprintf(buf, buf_len,
        "{\"t\":\"event\",\"name\":\"scan_error\",\"msg\":\"%s\"}\n",
        msg ? msg : "unknown error");
}

int protocol_format_hello(char *buf, size_t buf_len)
{
    return snprintf(buf, buf_len,
        "{\"t\":\"hello\",\"fw\":\"%s\",\"proto\":%d}\n",
        UBIOPOT_FW_VERSION_STR,
        UBIOPOT_PROTOCOL_VERSION);
}
