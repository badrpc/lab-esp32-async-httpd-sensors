#include "esp_err.h"
#include "esp_log.h"
#include "mbcontroller.h"
#include "sas8.h"

esp_err_t sas8ReadStatusCO2(uint8_t slave_addr, uint16_t *status , uint16_t *co2)
{
    mb_param_request_t req = {
        .slave_addr = slave_addr, // Sensor address.
        .command = 0x04,          // Function code. 0x04 - Read Input Registers.
        .reg_start = 0x0000,      // Address of starting register.
        .reg_size  = 0x0004,      // Number of registers.
    };

    uint32_t data[2];
    esp_err_t err = mbc_master_send_request(&req, (void*)(data));
    if (err != ESP_OK) {
        return err;
    }

    if (status != NULL) {
        *status = (uint16_t)(data[0] & 0xffff);
    }

    if (co2 != NULL) {
        *co2 = (uint16_t)(data[1] >> 16);
    }

    return err;
}
