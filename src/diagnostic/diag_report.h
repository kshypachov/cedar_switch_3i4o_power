//
// Created by Kirill Shypachov on 28.09.2025.
//

#ifndef CEDAR_SWITCH_3IN3OUT_POWER_DIAG_REPORT_H
#define CEDAR_SWITCH_3IN3OUT_POWER_DIAG_REPORT_H

void net_diag_start_periodic(k_timeout_t interval);
void start_send_data_to_outputs_pin(void);
#endif //CEDAR_SWITCH_3IN3OUT_POWER_DIAG_REPORT_H