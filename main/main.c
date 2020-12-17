/* Simple HTTP Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
// #include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"

#include <libesphttpd/httpd.h>
#include <libesphttpd/route.h>
#include <libesphttpd/httpd-freertos.h>

#include <esp_modbus_master.h>
#include <driver/gpio.h>
#include "sas8.h"

/* A simple example that demonstrates how to create GET and POST
 * handlers for the web server.
 */

static const char *TAG = "example";

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG, "disconnect_handler");
}

static void connect_handler(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG, "connect_handler");
}

#define LOG_TAG "sas8"

CgiStatus ICACHE_FLASH_ATTR handleMetrics(HttpdConnData *connData) {
    ESP_LOGI("handleMetrics", "+");

    // If the browser unexpectedly closes the connection, the CGI will be
    // called after the isConnectionClosed flag is set. We can use this to
    // clean up any data. It's not used in this simple CGI function.
    if (connData->isConnectionClosed) {
        // Connection aborted. Clean up.
        return HTTPD_CGI_DONE;
    }

    if (connData->requestType!=HTTPD_METHOD_GET) {
        // Sorry, we only accept GET requests.
        httpdStartResponse(connData, 406);  // HTTP error code 'unacceptable'
        httpdEndHeaders(connData);
        return HTTPD_CGI_DONE;
    }

    uint16_t status, co2;
    esp_err_t err = sas8ReadStatusCO2(104, &status, &co2);
    if (err != ESP_OK) {
        ESP_LOGE(LOG_TAG, "sas8ReadStatusCO2(...) err: 0x%x (%s).",
                 (int)err, (char*)esp_err_to_name(err));
        httpdStartResponse(connData, 503);
        httpdEndHeaders(connData);
        return HTTPD_CGI_DONE;
    }
    ESP_LOGI(LOG_TAG, "Status: 0x%04x; CO2: % 4d", status, co2);

    httpdStartResponse(connData, 200);
    httpdHeader(connData, "Content-Type", "text/plain");
    httpdEndHeaders(connData);

    char output[256];
    int len=sprintf(output, "# HELP concentration_ppm CO2 concentration (ppm).\n# TYPE concentration_ppm gauge\nconcentration_ppm{substance=\"co2\"} %d\n", co2);
    httpdSend(connData, output, len);

    ESP_LOGI("handleMetrics", "-");

    // If you need to suspend the HTTP response and resume it asynchronously
    // for some other reason, you may save the HttpdConnData pointer, return
    // HTTPD_CGI_MORE, then later call httpdContinue with the saved connection
    // pointer. For example, if you need to communicate with another device
    // over a different connection, you could send data to that device in the
    // initial CGI call, then return HTTPD_CGI_MORE, then, in the
    // espconn_recv_callback for the response, you can call httpdContinue to
    // resume the HTTP response with data retrieved from the other device.

    // All done.
    return HTTPD_CGI_DONE;
}


#define LISTEN_PORT     80u
#define MAX_CONNECTIONS 32u

static char connectionMemory[sizeof(RtosConnType) * MAX_CONNECTIONS];
static HttpdFreertosInstance httpdFreertosInstance;

const HttpdBuiltInUrl builtInUrls[]={
	ROUTE_CGI("/metrics", handleMetrics),
	ROUTE_END(),
};

#define MASTER_TAG LOG_TAG

#define MASTER_CHECK(a, ret_val, str, ...) \
    if (!(a)) { \
        ESP_LOGE(MASTER_TAG, "%s(%u): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        return (ret_val); \
    }

void* master_handler = NULL;

// MODBUS master initialization
static esp_err_t master_init(void)
{
    // Initialize and start Modbus controller
    mb_communication_info_t comm = {
            .port = UART_NUM_1,
            .mode = MB_MODE_RTU,
            .baudrate = 9600,
            .parity = MB_PARITY_NONE
    };

    esp_err_t err = mbc_master_init(MB_PORT_SERIAL_MASTER, &master_handler);
    MASTER_CHECK((master_handler != NULL), ESP_ERR_INVALID_STATE,
                                "mb controller initialization fail.");
    MASTER_CHECK((err == ESP_OK), ESP_ERR_INVALID_STATE,
                            "mb controller initialization fail, returns(0x%x).",
                            (uint32_t)err);
    err = mbc_master_setup((void*)&comm);
    MASTER_CHECK((err == ESP_OK), ESP_ERR_INVALID_STATE,
                            "mb controller setup fail, returns(0x%x).",
                            (uint32_t)err);

    // Set UART pin numbers
    err = uart_set_pin(UART_NUM_1, GPIO_NUM_33, GPIO_NUM_23,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    err = mbc_master_start();
    MASTER_CHECK((err == ESP_OK), ESP_ERR_INVALID_STATE,
                            "mb controller start fail, returns(0x%x).",
                            (uint32_t)err);

    MASTER_CHECK((err == ESP_OK), ESP_ERR_INVALID_STATE,
            "mb serial set pin failure, uart_set_pin() returned (0x%x).", (uint32_t)err);
    // Set driver mode to Half Duplex
    err = uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX);
    MASTER_CHECK((err == ESP_OK), ESP_ERR_INVALID_STATE,
            "mb serial set mode failure, uart_set_mode() returned (0x%x).", (uint32_t)err);

    vTaskDelay(5);
    ESP_LOGI(MASTER_TAG, "Modbus master stack initialized...");
    return err;
}


void app_main(void)
{
    ESP_ERROR_CHECK(master_init());

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
#ifdef CONFIG_EXAMPLE_CONNECT_WIFI
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, NULL /*&server*/));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, NULL /*&server*/));
#endif // CONFIG_EXAMPLE_CONNECT_WIFI
#ifdef CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_ETHERNET

    httpdFreertosInit(&httpdFreertosInstance, builtInUrls, LISTEN_PORT,
                      connectionMemory, MAX_CONNECTIONS, HTTPD_FLAG_NONE);
    httpdFreertosStart(&httpdFreertosInstance);
}
