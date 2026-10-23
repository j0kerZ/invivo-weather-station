#include "ws_crc.h"

uint8_t ws_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            unsigned shifted = (unsigned)crc << 1; /* unsigned: avoids int promotion */
            crc = (uint8_t)((crc & 0x80u) ? (shifted ^ 0x31u) : shifted);
        }
    }
    return crc;
}

uint16_t ws_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((unsigned)data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            unsigned shifted = (unsigned)crc << 1;
            crc = (uint16_t)((crc & 0x8000u) ? (shifted ^ 0x1021u) : shifted);
        }
    }
    return crc;
}
