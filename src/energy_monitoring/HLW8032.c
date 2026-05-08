//
// Created by Kirill Shypachov on 30.10.2025.
//

#include "HLW8032.h"

#include <stddef.h>
#include <string.h>

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

static float		VF = 1;
static float		CF = 1;

uint32_t    VolParam = 0;
uint32_t    VolData = 0;
uint32_t    PowerParam = 0;
uint32_t    PowerData = 0;
uint32_t    CurrParam = 0;
uint32_t    CurrData = 0;

uint16_t 	PF_reg, PF_reg_old = 0;
uint32_t    PF_data = 0;


/* HLW8032 message structure
 *
 * Byte 0 (1 byte) data[0] - indication of data state
 * Byte 1 (1 byte) data[1] - Default value	0x5A
 * Byte 2		  data[2]
 * byte 3		  data[3]
 * byte 4 (3 byte) data[4] - Valtage parameter reg (default)
 * byte 5		  data[5]
 * byte 6		  data[6]
 * byte 7 (3 byte) data[7] - Voltage register
 * byte 8		  data[8]
 * byte 9		  data[9]
 * byte 10(3 byte) data[10]- Current parameter register (default)
 * byte 11		  data[11]
 * byte 12		  data[12]
 * byte 13(3 byte) data[13]- Current register
 * byte 14		  data[14]
 * byte 15		  data[15]
 * byte 16(3 byte) data[16] - Power parameter register
 * byte 17		  data[17]
 * byte 18		  data[18]
 * byte 19(3 byte) data[19] - Power register
 * byte 20(1 byte) data[20] - Bits 4 - 7 indicate that Voltage Power Current data is updated
 * byte 21		  data[21]
 * byte 22(2 byte) data[22] - PF pulse numbers, used in conjunction with state register, not saved after power-fail
 * byte 23(1 byte) data[23] - Data check sum, used to verify whether data package is complete in communication
 */

void InitHLW8032(uint32_t VolR1, uint32_t VolR2, double CurrentShuntR) {

    //VolR1 - Resistanse in ohms for first resistor
    //VolR2 - Resistanse in ohms for second resistor
    //ShuntR - Resistanse in ohms for current shunt resistor

    VF = VolR1 / (VolR2 * 1000.0);
    CF = 1.0 / (CurrentShuntR * 1000.0);
}

static int Checksum(uint8_t * data, size_t size){

    uint8_t byte = 0;

    if (size < 24) {
        return -1;
    }

    for (uint8_t i = 2; i <= 22; i++){
        byte = byte + data[i];
    }
    if (byte == data[23])
    {
        return 0;
    }
    // Checksum error - invalid data
    return -2;
}

int RawStringHLW8032(const unsigned char * string, size_t string_size, energy_data_t * data) {

    int err = 0;

    // Checksum verification
    err = Checksum((uint8_t*)string, string_size);
    if (err != 0) {
        return err;
    }

    // Check the first byte of a message
    if (!strcmp((const char*)string +1, (const char*)0x5A)) {
        return -1; // Invalid data. The first byte is not 0x5A. Every message HLW8032 must start with 0x5A
    }

    // Parse voltage parameter
    VolParam = ((uint32_t)string[2] << 16) | ((uint32_t)string[3] << 8) | ((uint32_t)string[4]);
    // Check if voltage data is updated
    if(bitRead(string[20],6) == 1) {
        // Parse voltage data
        VolData = ((uint32_t)string[5] << 16) | ((uint32_t)string[6] << 8) | ((uint32_t)string[7]);
    }

    // Parse current parameter
    CurrParam = ((uint32_t)string[8] << 16) | ((uint32_t)string[9] << 8) | ((uint32_t)string[10]);
    // Check if current data is updated
    if(bitRead(string[20],5) == 1) {
        // Parse current data
        CurrData = ((uint32_t)string[11] << 16) | ((uint32_t)string[12] << 8) | ((uint32_t)string[13]);
    }

    // Parse power parameter
    PowerParam = ((uint32_t)string[14] << 16) | ((uint32_t)string[15] << 8) | ((uint32_t)string[16]);
    // Check if power data is updated
    if(bitRead(string[20],4) == 1) {
        // Parse power data
        PowerData = ((uint32_t)string[17] << 16) | ((uint32_t)string[18] << 8) | ((uint32_t)string[19]);
    }

    //PF pulse numbers register parsing. Used for energy calculation
    PF_reg = ((uint16_t)string[21] << 8) | ((uint16_t)string[22]);

    // Detect overflow of PF register.
    if((bitRead(string[20],7) == 1) || (PF_reg_old > PF_reg)) {
        PF_data ++;
    }

    PF_reg_old = PF_reg;

    return err;
}

//Calculate Raw Voltage
static float CalculateVoltageRAW(void) {
    if (VolData == 0 ) {
        return 0;
    }

    return VolParam / (float)VolData;
}

//Calculate Real voltage
float HLW8032_Voltage(void) {
    float voltage;

    voltage = CalculateVoltageRAW() * VF;
    if (voltage < 0) {
        voltage = 0.00001;
    }
    return voltage;
}

//Calculate Raw Current
static float CalculateCurrentRAW(void) {
    if (CurrData == 0 ) {
        return 0;
    }
    return CurrParam / (float)CurrData;
}