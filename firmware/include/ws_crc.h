#ifndef WS_CRC_H
#define WS_CRC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Bitwise implementations: a few dozen bytes of code and no lookup tables.
 * At one record per minute the speed of a table-driven CRC is not needed on a Cortex-M3.
 */

/* CRC-8/NRSC-5: poly 0x31, init 0xFF, not reflected, no final XOR. check("123456789") = 0xF7 */
uint8_t ws_crc8(const uint8_t *data, size_t len);

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, not reflected, no final XOR. check = 0x29B1 */
uint16_t ws_crc16(const uint8_t *data, size_t len);

#endif
