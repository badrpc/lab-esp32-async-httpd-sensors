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
#include <esp_timer.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"

#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>

#include <libesphttpd/httpd.h>
#include <libesphttpd/route.h>
#include <libesphttpd/httpd-freertos.h>

#include <esp_modbus_master.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include "sas8.h"

#define BME680_BSEC

#ifdef BME680_OPENSOURCE
#include <bme680.h>
#endif
#ifdef BME680_BSEC
#include <bsec_integration.h>
#include <bsec_serialized_configurations_iaq.h>
#endif

// Note on terminology
//
// Request is full request to gatekeeper task including technical header.
//
// Arguments are parameters of operation - part of the request without
// technical header.
//
// Response is a full response from gatekeeper task.
//
// Result is a result of an operation (with an exception that error code goes
// into response).

// Supported response allocation modes.
typedef enum {
    // gkResponseAllocStack - allocate full size of response structure on the
    // stack.
    gkResponseAllocStack,
    // TODO
    gkResponseAllocHeap,
} GKResponseAM;

// Generic gatekeeper request. Must be included as a first field of any request.
typedef struct {
    SemaphoreHandle_t sync;
    QueueHandle_t response_q;

    // Operation to execute in the context of gatekeeper task.
    void (*op)(void *args, void *result, size_t result_size);

    // Full size of request.
    size_t request_size;

    // Relative offset (in bytes) of operation arguments data structure inside
    // gatekeeper request. Use offsetof() to find it.
    size_t args_offset;

    // Response allocation mode. See comment for GKResponseAM and code of
    // gatekeeper task for details.
    GKResponseAM response_mode;

    // What size response_q expects. This should always be small enough to fit
    // on stack. But must be sufficient to fit GKResponse.
    size_t response_size;
    size_t result_size;

    // Relative offset (in bytes) of operation result data structure inside
    // gatekeeper response. Use offsetof() to find it.
    size_t result_offset;
} GKRequest;

typedef struct {
    esp_err_t err;
} GKResponseError;

typedef struct {
    esp_err_t err;
    GKResponseAM response_mode;
} GKResponseStack;

typedef struct {
    esp_err_t err;
    GKResponseAM response_mode;
    void* result_ptr;
} GKResponseHeap;

// Gatekeeper task arguments.
typedef struct {
    QueueHandle_t q;
    size_t request_size;
} GKTaskArgs;

bool gkExecResultStack(GKRequest *req, void *op_args);
bool gkExecResultHeap(GKRequest *req, void *op_args);
bool gkExecDone(GKRequest *req, void *result);
bool gkError(GKRequest *req, esp_err_t err);
static void gkTimeoutFunc(TimerHandle_t xTimer);
static esp_err_t GKSendRequest(QueueHandle_t request_q, GKRequest *request, TickType_t timeoutTicks, TimerHandle_t *timeoutTimer);
static esp_err_t GKSendRequestQ(QueueHandle_t request_q, GKRequest *request, TickType_t timeoutTicks, QueueHandle_t *response_q, TimerHandle_t *timeoutTimer);

static void GatekeeperTask(void *arg) {
    static const char* log_tag = __func__;
    GKTaskArgs *a = (GKTaskArgs*) arg;
    if (a->q == NULL) {
        ESP_LOGE(log_tag, "a->q == NULL");
        abort();
    }
    if (a->request_size < sizeof(GKRequest)) {
        ESP_LOGE(log_tag, "a->req_size < sizeof(GKRequest)");
        abort();
    }
    char reqData[a->request_size];
    GKRequest *req = (GKRequest *)reqData;
    for (;;) {
        xQueueReceive(a->q, req, portMAX_DELAY);

        ESP_LOGI(log_tag, "Request received");

        // If req->sync reached 0 then this request has already timed out.
        // Presumably by sitting in the queue for too long. There's no need to
        // execute operation and task should proceed directly to resource
        // cleanup.
        if (uxSemaphoreGetCount(req->sync) < 1) {
            ESP_LOGE(log_tag, "request timed out early");
            ESP_LOGI("semaphore", "- %p", req->sync);
            vSemaphoreDelete(req->sync);
            continue;
        }

        if (req->response_size < sizeof(GKResponseError)) {
            ESP_LOGE(log_tag, "req->response_size < sizeof(GKResponseError)");
            abort();
        }

        if (req->request_size > a->request_size) {
            ESP_LOGE(log_tag, "req->request_size > a->req_size");
            gkError(req, ESP_ERR_INVALID_ARG);
            continue;
        }

        if (req->args_offset != 0 && req->args_offset < sizeof(GKRequest)) {
            ESP_LOGE(log_tag, "req->args_offset < sizeof(GKRequest)");
            gkError(req, ESP_ERR_INVALID_ARG);
            continue;
        }

        if (req->args_offset >= req->request_size) {
            ESP_LOGE(log_tag, "req->args_offset >= req->request_size");
            gkError(req, ESP_ERR_INVALID_ARG);
            continue;
        }

        void *op_args = NULL;
        if (req->args_offset != 0) {
            op_args = ((char *)req) + req->args_offset;
        }

        switch (req->response_mode) {
        case gkResponseAllocStack:
            gkExecResultStack(req, op_args);
            break;
        case gkResponseAllocHeap:
            gkExecResultHeap(req, op_args);
            break;
        default:
            ESP_LOGE(log_tag, "Unsupported req->response_mode %d", req->response_mode);
            gkError(req, ESP_ERR_INVALID_ARG);
            continue;
        }
    }
}

bool gkExecResultStack(GKRequest *request, void* op_args) {
    static const char* log_tag = __func__;

    if (request->response_size < sizeof(GKResponseStack)) {
        ESP_LOGE(log_tag, "req->response_size < sizeof(GKResponseStack)");
        return gkError(request, ESP_ERR_INVALID_ARG);
    }

    if (request->result_offset != 0 && request->result_offset < sizeof(GKResponseStack)) {
        ESP_LOGE(log_tag, "request->result_offset < sizeof(GKResponseStack)");
        return gkError(request, ESP_ERR_INVALID_ARG);
    }

    if (request->result_offset >= request->response_size) {
        ESP_LOGE(log_tag, "result_offset >= req->response_size");
        return gkError(request, ESP_ERR_INVALID_ARG);
    }

    char response_data[request->response_size];
    GKResponseStack *response = (GKResponseStack*)response_data;
    response->err = ESP_OK;
    response->response_mode = gkResponseAllocStack;

    void *result = NULL;
    size_t result_size = 0;
    if (request->result_offset != 0) {
        result = ((char *)response) + request->result_offset;
        result_size = request->response_size - request->result_offset;
    }
    ESP_LOGI(log_tag, "+ op");
    request->op(op_args, result, result_size);
    ESP_LOGI(log_tag, "- op");
    return gkExecDone(request, response);
}

bool gkExecResultHeap(GKRequest *request, void *op_args) {
    static const char* log_tag = __func__;

    if (request->response_size < sizeof(GKResponseHeap)) {
        ESP_LOGE(log_tag, "req->response_size < sizeof(GKResponseHeap)");
        return gkError(request, ESP_ERR_INVALID_ARG);
    }

    char response_data[request->response_size];
    GKResponseHeap *response = (GKResponseHeap*)response_data;
    response->err = ESP_OK;
    response->response_mode = gkResponseAllocHeap;
    response->result_ptr = NULL;
    size_t result_size = 0;
    if (request->result_size>0) {
        response->result_ptr = malloc(request->result_size);
        ESP_LOGI("heap", "+ %p", response->result_ptr);
        if (response->result_ptr == NULL) {
          ESP_LOGI(log_tag, "Failed to allocate response.result_ptr");
          return gkError(request, ESP_ERR_NO_MEM);
        }
        result_size = request->result_size;
    }

    ESP_LOGI(log_tag, "+ op");
    request->op(op_args, response->result_ptr, result_size);
    ESP_LOGI(log_tag, "- op");

    bool ret = gkExecDone(request, response);
    if (!ret) {
        if (response->result_ptr != NULL) {
            ESP_LOGI("heap", "- %p", response->result_ptr);
            free(response->result_ptr);
        }
    }
    return ret;
}

// gkError prepares and sends gkResponseError with specified error code. Caller
// must ensure that request has at least sizeof(GKRequestError) and response
// has at least sizeof(GKResponseError) bytes.
bool gkError(GKRequest *request, esp_err_t err) {
    char response_data[request->response_size];
    GKResponseError *response = (GKResponseError*)response_data;
    response->err = err;
    return gkExecDone(request, response);
}

bool gkExecDone(GKRequest *req, void *result) {
    static const char* log_tag = __func__;
    if (xSemaphoreTake(req->sync, 0) != pdTRUE) {
        // This task was too late and request timed out. This side is
        // responsible for deleting the sync semaphore.
        ESP_LOGE(log_tag, "request timed out");
        ESP_LOGI("semaphore", "- %p", req->sync);
        vSemaphoreDelete(req->sync);
        return false;
    }

    // Gatekeeper task was first to take the sync semaphore meaning
    // that timeout timer hasn't fired yet. In this case the gatekeeper
    // task is responsible to write the result and signal the done
    // channel. Timeout timer is responsible for deleting the sync
    // semaphore.
    if (xQueueSend(req->response_q, result, 0) != pdPASS) {
        ESP_LOGE(log_tag, "xQueueSend(req->result) != pdPASS");
        return false;
    }
    return true;
}

esp_err_t gkTimeout(const TickType_t timeoutTicks, GKRequest *request, TimerHandle_t *timeoutTimer) {
    static const char* log_tag = __func__;
    *timeoutTimer = xTimerCreate("gkTimeout", timeoutTicks, pdFALSE /* uxAutoReload */, request, gkTimeoutFunc);
    if (*timeoutTimer == NULL) {
        ESP_LOGE(log_tag, "xTimerCreate(): NULL");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI("timer", "+ %p", *timeoutTimer);
    if (xTimerStart(*timeoutTimer, 0) != pdPASS) {
        ESP_LOGE(log_tag, "xTimerStart(timeoutTimer): !pdPASS");

        ESP_LOGI("timer", "- %p", *timeoutTimer);
        if (xTimerDelete(*timeoutTimer, 0) != pdPASS) {
            ESP_LOGE(log_tag, "xTimerDelete(timeoutTimer) != pdPASS");
        }

        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void gkTimeoutFunc(TimerHandle_t xTimer) {
    static const char* log_tag = __func__;
    GKRequest *req = (GKRequest *)pvTimerGetTimerID(xTimer);
    if (req == NULL) {
        ESP_LOGE(log_tag, "req == NULL");
        return;
    }
    if (req->response_size < sizeof(GKResponseError)) {
        ESP_LOGE(log_tag, "req->response_size < sizeof(GKResponseError)");
        abort();
    }
    if (xSemaphoreTake(req->sync, 0) != pdTRUE) {
        // Sync semaphore has already been taken meaning that the gatekeeper
        // task was faster. In this case the gatekeeper task is responsible for
        // notifying the done semaphore. All that's left to be done here is to
        // delete the sync semaphore and free memory allocated to timer
        // request.
        ESP_LOGI("semaphore", "- %p", req->sync);
        vSemaphoreDelete(req->sync);
    } else {
        // Timer task came first to taking the sync semaphore meaning that the
        // gatekeeper task was too slow and we're dealing with a timeout. We
        // should reflect timeout state in the response and notify the done
        // channel. The gatekeeper task will be responsible for deleting the
        // sync semaphore.

        gkError(req, ESP_ERR_TIMEOUT);
    }
    // Timer gets a dedicated copy of request which it should delete when done.
    ESP_LOGI("heap", "- %p", req);
    free(req);
    // Setting Timer ID to NULL is used to signal that timer has already fired
    // and associated request and sync semaphore has been handled. This is
    // later used by gkTimerCancelFunc to correctly identify resources to
    // release.
    vTimerSetTimerID(xTimer, NULL);
}

// gkTimerCancelFunc deletes the gatekeeper timeout timer. It must execute in
// the context of FreeRTOS timer task to ensure serialised execution of
// gkTimeoutFunc and gkTimerCancelFunc.
static void gkTimerCancelFunc(void *timerParam, uint32_t param2) {
    static const char* log_tag = __func__;
    TimerHandle_t timer = timerParam;
    if (timer == NULL) {
        ESP_LOGE(log_tag, "timer == NULL");
        abort();
    }

    GKRequest *req = (GKRequest *)pvTimerGetTimerID(timer);

    // Timer removal is a deferred operation - xTimerDelete only queues request
    // to delete the timer but does not delete it immediately. So even though
    // we call xTimerDelete here it is still possible for timer to expire and
    // execute gkTimeoutFunc. Setting TimerID to NULL here ensures that
    // gkTimeoutFunc won't attempt to free memory and delete sync semaphore
    // second time.
    vTimerSetTimerID(timer, NULL);

    ESP_LOGI("timer", "- %p", timer);
    if (xTimerDelete(timer, 0) != pdPASS) {
        ESP_LOGE(log_tag, "xTimerDelete() != pdPASS");
    }

    if (req == NULL) {
        // Timer has already fired and handled sync semaphore and request. All
        // that's left is to delete timer itself.
        return;
    }

    // The following requires that the task doing the "real work" always
    // finishes correctly and handle sync semaphore deletion if needed.
    if (xSemaphoreTake(req->sync, 0) != pdTRUE) {
        ESP_LOGI("semaphore", "- %p", req->sync);
        vSemaphoreDelete(req->sync);
    }
    ESP_LOGI("heap", "- %p", req);
    free(req);
}

static BaseType_t GKTimerCancel(TimerHandle_t timer) {
    return xTimerPendFunctionCall(gkTimerCancelFunc, timer, 0, 0);
}

static esp_err_t GKSendRequestQ(QueueHandle_t request_q, GKRequest *request, TickType_t timeoutTicks, QueueHandle_t *response_q, TimerHandle_t *timeoutTimer) {
    static const char* log_tag = __func__;
    *response_q = xQueueCreate(1, request->response_size);
    if (response_q == NULL) {
        ESP_LOGE(log_tag, "xQueueCreate() == NULL");
        return ESP_ERR_INVALID_STATE;
    }
    request->response_q = *response_q;
    ESP_LOGI("queue", "+ %p", *response_q);
    esp_err_t err = GKSendRequest(request_q, request, timeoutTicks, timeoutTimer);
    if (err != ESP_OK) {
        vQueueDelete(*response_q);
        ESP_LOGI("queue", "- %p", *response_q);
    }
    return err;
}

static esp_err_t GKSendRequest(QueueHandle_t request_q, GKRequest *request, TickType_t timeoutTicks, TimerHandle_t *timeoutTimer) {
    static const char* log_tag = __func__;
    request->sync = xSemaphoreCreateBinary();
    if (request->sync == NULL) {
        ESP_LOGE(log_tag, "xSemaphoreCreateBinary() == NULL");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI("semaphore", "+ %p", request->sync);
    if (xSemaphoreGive(request->sync) != pdPASS) {
        ESP_LOGE(log_tag, "xSemaphoreGive(request->sync) != pdPASS");
        ESP_LOGI("semaphore", "- %p", request->sync);
        vSemaphoreDelete(request->sync);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gkTimeout(timeoutTicks, (GKRequest *) request, timeoutTimer);
    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "gkTimeout(...) err: 0x%x (%s).",
                 (int)err, (char*)esp_err_to_name(err));
        ESP_LOGI("semaphore", "- %p", request->sync);
        vSemaphoreDelete(request->sync);
        return err;
    }

    if (xQueueSend(request_q, request, 0) != pdPASS) {
        ESP_LOGE(log_tag, "xQueueSend(): !pdPASS");
        ESP_LOGI("semaphore", "- %p", request->sync);
        vSemaphoreDelete(request->sync);
        GKTimerCancel(timeoutTimer);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static volatile GKTaskArgs sas8GKArgs;

typedef struct {
    uint8_t sensor_addr;
} SAS8StatusCO2Args;

typedef struct {
    GKRequest gk_req;
    SAS8StatusCO2Args args;
} SAS8GKStatusCO2Request;

typedef struct {
    esp_err_t err;
    uint16_t status;
    uint16_t co2;
} SAS8StatusCO2Result;

typedef struct {
    GKResponseStack gk_resp;
    SAS8StatusCO2Result result;
} SAS8GKStatusCO2Response;


void dummy_op(void *args, void *result, size_t result_size) {
    static const char* log_tag = __func__;
    ESP_LOGI(log_tag, "args: %p; result: %p; result_size: %d", args, result, result_size);
}

void sas8ReadStatusCO2Op(void *args, void *result, size_t result_size) {
    static const char* log_tag = __func__;
    ESP_LOGI(log_tag, "args: %p; result: %p; result_size: %d", args, result, result_size);
    SAS8StatusCO2Args *a = (SAS8StatusCO2Args*) args;
    ESP_LOGI(log_tag, "sensor_addr: %d", a->sensor_addr);
    SAS8StatusCO2Result *r = (SAS8StatusCO2Result*) result;
    r->err = sas8ReadStatusCO2(a->sensor_addr, &r->status, &r->co2);
    ESP_LOGI(log_tag, "Status: 0x%04x; CO2: % 4d", r->status, r->co2);
}

static esp_err_t SAS8ReadStatusCO2Async(TickType_t timeoutTicks, QueueHandle_t *response_q, TimerHandle_t *timeoutTimer) {
    static const char* log_tag = __func__;

    SAS8GKStatusCO2Request *request = malloc(sizeof(SAS8GKStatusCO2Request));
    if (request == NULL) {
        ESP_LOGE(log_tag, "malloc(sizeof(SAS8GKStatusCO2Request)) == NULL");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI("heap", "+ %p", request);
    request->gk_req.op = sas8ReadStatusCO2Op;
    request->gk_req.request_size = sizeof(SAS8GKStatusCO2Request);
    request->gk_req.args_offset = offsetof(SAS8GKStatusCO2Request, args);
    request->gk_req.response_mode = gkResponseAllocStack;
    request->gk_req.response_size = sizeof(SAS8GKStatusCO2Response);
    request->gk_req.result_size = 0; // Only used when .response_mode = gkResponseAllocHeap
    request->gk_req.result_offset = offsetof(SAS8GKStatusCO2Response, result);
    request->args.sensor_addr = 104;

    esp_err_t err = GKSendRequestQ(sas8GKArgs.q, (GKRequest *)request, timeoutTicks, response_q, timeoutTimer);
    if (err != ESP_OK) {
        ESP_LOGI("heap", "- %p", request);
        free(request);
    }
    return err;
}

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
    ESP_LOGI(__func__, "disconnect_handler");
}

static void connect_handler(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data)
{
    ESP_LOGI(__func__, "connect_handler");
}

CgiStatus ICACHE_FLASH_ATTR handleMetrics(HttpdConnData *connData) {
    static const char* log_tag = __func__;
    ESP_LOGI(log_tag, "+");

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

    ESP_LOGI(log_tag, "sending request");

    // // // // // // // // //
    QueueHandle_t done;
    TimerHandle_t tTimeout;
    esp_err_t err = SAS8ReadStatusCO2Async(pdMS_TO_TICKS(150), &done, &tTimeout);
    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "sas8ReadStatusCO2(...) err: 0x%x (%s).",
                 (int)err, (char*)esp_err_to_name(err));
        httpdStartResponse(connData, 503);
        httpdEndHeaders(connData);
        return HTTPD_CGI_DONE;
    }
    SAS8GKStatusCO2Response response;
    if (xQueueReceive(done, &response, portMAX_DELAY) != pdPASS) {
        ESP_LOGE(log_tag, "xSemaphoreTake(done): !pdPASS");
        abort();
    }
    GKTimerCancel(tTimeout);
    vQueueDelete(done);
    ESP_LOGI("queue", "- %p", done);
    if (response.gk_resp.err != ESP_OK) {
        ESP_LOGE(log_tag, "response.err: 0x%x (%s).",
                 (int)err, (char*)esp_err_to_name(err));
        httpdStartResponse(connData, 503);
        httpdEndHeaders(connData);
        return HTTPD_CGI_DONE;
    }
    ESP_LOGI(log_tag, "Status: 0x%04x; CO2: % 4d", response.result.status, response.result.co2);

    httpdStartResponse(connData, 200);
    httpdHeader(connData, "Content-Type", "text/plain");
    httpdEndHeaders(connData);

    char output[256];
    int len=sprintf(output, "# HELP concentration_ppm CO2 concentration (ppm).\n# TYPE concentration_ppm gauge\nconcentration_ppm{substance=\"co2\"} %d\n", response.result.co2);
    httpdSend(connData, output, len);

    ESP_LOGI(log_tag, "-");

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

static void updateConsole(void *unused_arg) {
    static const char* log_tag = __func__;

    for(;;vTaskDelay(pdMS_TO_TICKS(14000))) {
        QueueHandle_t done;
        TimerHandle_t tTimeout;
        esp_err_t err = SAS8ReadStatusCO2Async(pdMS_TO_TICKS(150), &done, &tTimeout);
        if (err != ESP_OK) {
            ESP_LOGE(log_tag, "SAS8ReadStatusCO2Async(...) err: 0x%x (%s).",
                     (int)err, (char*)esp_err_to_name(err));
            continue;
        }
        SAS8GKStatusCO2Response response;
        if (xQueueReceive(done, &response, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(log_tag, "xQueueReceive(done): !pdPASS");
            // TODO: release resources.
            continue;
        }
        GKTimerCancel(tTimeout);
        vQueueDelete(done);
        ESP_LOGI("queue", "- %p", done);
        if (response.gk_resp.err != ESP_OK) {
            ESP_LOGE(log_tag, "response.gk_resp.err err: 0x%x (%s).",
                     (int)response.gk_resp.err, (char*)esp_err_to_name(response.gk_resp.err));
            continue;
        }
        ESP_LOGI(log_tag, "Status: 0x%04x; CO2: % 4d", response.result.status, response.result.co2);
    }
}


#define LISTEN_PORT     80u
#define MAX_CONNECTIONS 32u

static char connectionMemory[sizeof(RtosConnType) * MAX_CONNECTIONS];
static HttpdFreertosInstance httpdFreertosInstance;

const HttpdBuiltInUrl builtInUrls[]={
	ROUTE_CGI("/metrics", handleMetrics),
	ROUTE_END(),
};

#define MASTER_TAG "sas8"

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
    err = uart_set_pin(UART_NUM_1, GPIO_NUM_21, GPIO_NUM_25,
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

// user task stack depth for ESP32
#define TASK_STACK_DEPTH 2048

// I2C interface defintions for ESP32 and ESP8266
#define I2C_BUS       0
#define I2C_SCL_PIN   22
#define I2C_SDA_PIN   19
#define I2C_FREQ      I2C_FREQ_100K

/* -- user tasks --------------------------------------------------- */

#ifdef BME680_OPENSOURCE
static bme680_sensor_t* sensor = 0;
#endif

#ifdef BME680_OPENSOURCE
/*
 * User task that triggers measurements of sensor every seconds. It uses
 * function *vTaskDelay* to wait for measurement results. Busy wating
 * alternative is shown in comments
 */
void user_task(void *pvParameters)
{
    TickType_t last_wakeup = xTaskGetTickCount();

    bme680_values_float_t values;

    // as long as sensor configuration isn't changed, duration is constant
    uint32_t duration = bme680_get_measurement_duration(sensor);

    while (1) {
        // trigger the sensor to start one TPHG measurement cycle
        if (bme680_force_measurement (sensor))
        {
            // passive waiting until measurement results are available
            vTaskDelay (duration);

            // alternatively: busy waiting until measurement results are available
            // while (bme680_is_measuring (sensor)) ;

            // get the results and do something with them
            if (bme680_get_results_float (sensor, &values))
                printf("%.3f BME680 Sensor: %.2f °C, %.2f %%, %.2f hPa, %.2f Ohm\n",
                       (double)sdk_system_get_time()*1e-3,
                       values.temperature, values.humidity,
                       values.pressure, values.gas_resistance);
        }
        // passive waiting until 1 second is over
        vTaskDelayUntil(&last_wakeup, 10000 / portTICK_PERIOD_MS);
    }
}
#endif

#ifdef BME680_BSEC

#define I2C_ACK_VAL  0x0
#define I2C_NACK_VAL 0x1

/*!
 * @brief           Write operation in either I2C or SPI
 *
 * param[in]        dev_addr        I2C or SPI device address
 * param[in]        reg_addr        register address
 * param[in]        reg_data_ptr    pointer to the data to be written
 * param[in]        data_len        number of bytes to be written
 *
 * @return          result of the bus communication function
 */
int8_t bus_write(uint8_t dev_addr, uint8_t reg_addr, uint8_t *reg_data_ptr, uint16_t data_len)
{
    // ...
    // Please insert system specific function to write to the bus where BME680 is connected
    // ...
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, dev_addr << 1 | I2C_MASTER_WRITE, true);

    i2c_master_write_byte(cmd, reg_addr, true);

    if (reg_data_ptr) {
        i2c_master_write(cmd, reg_data_ptr, data_len, true);
    }
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(/* bus */ 0, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    return err;
}

/*!
 * @brief           Read operation in either I2C or SPI
 *
 * param[in]        dev_addr        I2C or SPI device address
 * param[in]        reg_addr        register address
 * param[out]       reg_data_ptr    pointer to the memory to be used to store the read data
 * param[in]        data_len        number of bytes to be read
 *
 * @return          result of the bus communication function
 */
int8_t bus_read(uint8_t dev_addr, uint8_t reg_addr, uint8_t *reg_data_ptr, uint16_t data_len)
{
    // ...
    // Please insert system specific function to read from bus where BME680 is connected
    // ...
    if (data_len == 0) return true;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, ( dev_addr << 1 ) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    if (!reg_data_ptr) {
        i2c_master_stop(cmd);
    }

    if (reg_data_ptr)
    {
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, ( dev_addr << 1 ) | I2C_MASTER_READ, true);
        if (data_len > 1) {
            i2c_master_read(cmd, reg_data_ptr, data_len-1, I2C_ACK_VAL);
        }
        i2c_master_read_byte(cmd, reg_data_ptr + data_len-1, I2C_NACK_VAL);
        i2c_master_stop(cmd);
    }
    esp_err_t err = i2c_master_cmd_begin(/* bus */ 0, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    return err;
}

/*!
 * @brief           System specific implementation of sleep function
 *
 * @param[in]       t_ms    time in milliseconds
 *
 * @return          none
 */
void local_sleep(uint32_t t_ms)
{
    // ...
    // Please insert system specific function sleep or delay for t_ms milliseconds
    // ...
    vTaskDelay(pdMS_TO_TICKS(t_ms));
}

/*!
 * @brief           Capture the system time in microseconds
 *
 * @return          system_current_time    current system timestamp in microseconds
 */
int64_t get_timestamp_us()
{
    // int64_t system_current_time = 0;
    // ...
    // Please insert system specific function to retrieve a timestamp (in microseconds)
    // ...
    return esp_timer_get_time();
    // return system_current_time;
}

/*!
 * @brief           Handling of the ready outputs
 *
 * @param[in]       timestamp       time in nanoseconds
 * @param[in]       iaq             IAQ signal
 * @param[in]       iaq_accuracy    accuracy of IAQ signal
 * @param[in]       temperature     temperature signal
 * @param[in]       humidity        humidity signal
 * @param[in]       pressure        pressure signal
 * @param[in]       raw_temperature raw temperature signal
 * @param[in]       raw_humidity    raw humidity signal
 * @param[in]       gas             raw gas sensor signal
 * @param[in]       bsec_status     value returned by the bsec_do_steps() call
 *
 * @return          none
 */
void output_ready(int64_t timestamp, float iaq, uint8_t iaq_accuracy, float temperature, float humidity,
     float pressure, float raw_temperature, float raw_humidity, float gas, bsec_library_return_t bsec_status,
     float static_iaq, float co2_equivalent, float breath_voc_equivalent)
{
    // const char *log_tag = "BSEC output";
    // ...
    // Please insert system specific code to further process or display the BSEC outputs
    // ...
    // ESP_LOGI(log_tag, "timestamp: %lld, iaq: %f, iaq_accuracy: %d, temperature: %f, humidity: %f, pressure: %f, raw_temperature: %f, raw_humidity: %f, gas: %f, static_iaq: %f, co2: %f, voc: %f", timestamp, iaq, iaq_accuracy, temperature, humidity, pressure, raw_temperature, raw_humidity, gas, static_iaq, co2_equivalent, breath_voc_equivalent);
    printf("timestamp: %lld, iaq: %f, iaq_accuracy: %d, temperature: %f, humidity: %f, pressure: %f, raw_temperature: %f, raw_humidity: %f, gas: %f, static_iaq: %f, co2: %f, voc: %f\n", timestamp, iaq, iaq_accuracy, temperature, humidity, pressure, raw_temperature, raw_humidity, gas, static_iaq, co2_equivalent, breath_voc_equivalent);
}

/*!
 * @brief           Load previous library state from non-volatile memory
 *
 * @param[in,out]   state_buffer    buffer to hold the loaded state string
 * @param[in]       n_buffer        size of the allocated state buffer
 *
 * @return          number of bytes copied to state_buffer
 */
uint32_t state_load(uint8_t *state_buffer, uint32_t n_buffer)
{
    // ...
    // Load a previous library state from non-volatile memory, if available.
    //
    // Return zero if loading was unsuccessful or no state was available, 
    // otherwise return length of loaded state string.
    // ...
    return 0;
}

/*!
 * @brief           Save library state to non-volatile memory
 *
 * @param[in]       state_buffer    buffer holding the state to be stored
 * @param[in]       length          length of the state string to be stored
 *
 * @return          none
 */
void state_save(const uint8_t *state_buffer, uint32_t length)
{
    // ...
    // Save the string some form of non-volatile memory, if possible.
    // ...
}
 
/*!
 * @brief           Load library config from non-volatile memory
 *
 * @param[in,out]   config_buffer    buffer to hold the loaded state string
 * @param[in]       n_buffer        size of the allocated state buffer
 *
 * @return          number of bytes copied to config_buffer
 */
uint32_t config_load(uint8_t *config_buffer, uint32_t n_buffer)
{
    // ...
    // Load a library config from non-volatile memory, if available.
    //
    // Return zero if loading was unsuccessful or no config was available, 
    // otherwise return length of loaded config string.
    // ...
    if (sizeof(bsec_config_iaq) < n_buffer) {
        printf("config_load: buffsizeof(bsec_config_iaq)r too small: need %d, provided %d\n", sizeof(bsec_config_iaq), n_buffer);
        return 0;
    }
    printf("config_load: %d bytes\n", sizeof(bsec_config_iaq));
    memcpy(config_buffer, bsec_config_iaq, sizeof(bsec_config_iaq));
    return sizeof(bsec_config_iaq);
}

void user_task(void *pvParameters)
{
    i2c_config_t conf;
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_SDA_PIN;
    conf.scl_io_num = I2C_SCL_PIN;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 1000000;
    i2c_param_config(/* bus */ 0, &conf);
    i2c_driver_install(/* bus */ 0, I2C_MODE_MASTER, 0, 0, 0);

    // BME680 via proprietary Bosch library
    // https://github.com/BoschSensortec/BME68x-Sensor-API
    // https://www.bosch-sensortec.com/software-tools/software/bsec/
    return_values_init ret;

    /* Call to the function which initializes the BSEC library 
     * Switch on low-power mode and provide no temperature offset */
    ret = bsec_iot_init(BSEC_SAMPLE_RATE_LP, 4.0f, bus_write, bus_read, local_sleep, state_load, config_load);
    if (ret.bme680_status)
    {
        /* Could not intialize BME680 */
        printf("(BSEC) Could not initialize BME680 sensor\n");
    }
    else if (ret.bsec_status)
    {
        /* Could not intialize BSEC library */
        printf("(BSEC) Could not initialize BSEC library\n");
    }

    /* Call to endless loop function which reads and processes data based on sensor settings */
    /* State is saved every 10.000 samples, which means every 10.000 * 3 secs = 500 minutes  */
    bsec_iot_loop(local_sleep, get_timestamp_us, output_ready, state_save, 10000);
}
#endif

void app_main(void)
{
    static const char* log_tag = __func__;

#ifdef BME680_OPENSOURCE
    // BME680 via open-source library https://github.com/gschorcht/bme680-esp-idf

    /** -- MANDATORY PART -- */

    // Init all I2C bus interfaces at which BME680 sensors are connected
    i2c_init(I2C_BUS, I2C_SCL_PIN, I2C_SDA_PIN, I2C_FREQ);

    // init the sensor with slave address BME680_I2C_ADDRESS_2 connected to I2C_BUS.
    sensor = bme680_init_sensor (I2C_BUS, BME680_I2C_ADDRESS_2, 0);

    if (sensor) {
        /** -- SENSOR CONFIGURATION PART (optional) --- */

        // Changes the oversampling rates to 4x oversampling for temperature
        // and 2x oversampling for humidity. Pressure measurement is skipped.
        // bme680_set_oversampling_rates(sensor, osr_4x, osr_none, osr_2x);
        bme680_set_oversampling_rates(sensor, osr_4x, osr_4x, osr_4x);

        // Change the IIR filter size for temperature and pressure to 7.
        bme680_set_filter_size(sensor, iir_size_7);

        // Change the heater profile 0 to 200 degree Celcius for 100 ms.
        //bme680_set_heater_profile (sensor, 0, 200, 100);
        //bme680_use_heater_profile (sensor, 0);
        bme680_use_heater_profile(sensor, BME680_HEATER_NOT_USED);

        // Set ambient temperature to 10 degree Celsius
        bme680_set_ambient_temperature (sensor, 22);

        /** -- TASK CREATION PART --- */

        // must be done last to avoid concurrency situations with the sensor 
        // configuration part

        // Create a task that uses the sensor
        xTaskCreate(user_task, "user_task", TASK_STACK_DEPTH, NULL, 2, NULL);
    } else {
        printf("Could not initialize BME680 sensor\n");
    }
#endif

#ifdef BME680_BSEC
    xTaskCreate(user_task, "user_task", 8192, NULL, 2, NULL);
#endif

    ESP_ERROR_CHECK(master_init());

    sas8GKArgs.request_size = sizeof(SAS8GKStatusCO2Request);
    sas8GKArgs.q = xQueueCreate(16, sas8GKArgs.request_size);
    if (sas8GKArgs.q == NULL) {
        ESP_LOGE(log_tag, "sas8GKArgs.q == NULL");
        abort();
    }
    xTaskCreate(GatekeeperTask, "sas8Gatekeeper", 2048, (void *)&sas8GKArgs, 1, NULL);
    xTaskCreate(updateConsole, "updateConsole", 2048, NULL, 1, NULL);

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
