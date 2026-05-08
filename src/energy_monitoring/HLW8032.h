//
// Created by Kirill Shypachov on 30.10.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_HLW8032_H
#define CEDAR_SWITCH_3IN3OUT_POWER_HLW8032_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    float energy;   //energy in kw_h
    float voltage;  //AC voltage
    float current;  //AC current
    float power_active; //
    float power_apparent; //
    float power_factor; //
}energy_data_t;


void InitHLW8032(uint32_t VolR1, uint32_t VolR2, double CurrentShuntR);
int RawStringHLW8032(const unsigned char * string, size_t string_size, energy_data_t * data);


#endif //CEDAR_SWITCH_3IN3OUT_POWER_HLW8032_H