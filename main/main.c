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
#include "sas8.h"

#if 0
typedef struct {
    sas8StatusCO2Response *response;
    SemaphoreHandle_t sync;
    SemaphoreHandle_t done;
} sas8StatusCO2Request;

static void sas8Gatekeeper(void *arg) {
    QueueHandle_t queue = (QueueHandle_t)arg;
    if (queue == NULL) {
        ESP_LOGE("sas8Gatekeeper", "queue == NULL");
        abort();
    }
    sas8StatusCO2Request req;
    for (;;) {
        xQueueReceive(queue, &req, portMAX_DELAY);

        ESP_LOGI("sas8Gatekeeper", "new request");

        // If req.sync reached 0 then this request has already timed out.
        // Presumably by sitting in the queue for too long. There's no need to
        // execute operation and task should proceed directly to resource
        // cleanup.
        if (uxSemaphoreGetCount(req.sync) < 1) {
            ESP_LOGE("sas8Gatekeeper", "request timed out early");
#if 0
        } else if (response == NULL) {
            // TODO: in generic gatekeeper pass response directly into the
            // operation and omit this check.
            ESP_LOGE("sas8Gatekeeper", "request->response is NULL");
#endif
        } else {
            ESP_LOGI("sas8Gatekeeper", "+ sas8ReadStatusCO2");
            esp_err_t err;
            uint16_t status=0, co2=0;
            err = sas8ReadStatusCO2(104, &status, &co2);
            ESP_LOGI("sas8Gatekeeper", "- sas8ReadStatusCO2");
            if (err != ESP_OK) {
                ESP_LOGE("sas8Gatekeeper", "sas8ReadStatusCO2(...) err: 0x%x (%s).",
                         (int)err, (char*)esp_err_to_name(err));
            }
            ESP_LOGI("sas8Gatekeeper", "Status: 0x%04x; CO2: % 4d", status, co2);
        }

        if (xSemaphoreTake(req.sync, 0) == pdTRUE) {
            // Gatekeeper task was first to take the sync semaphore meaning
            // that timeout timer hasn't fired yet. In this case the gatekeeper
            // task is responsible to write the result and signal the done
            // channel. Timeout timer is responsible for deleting the sync
            // semaphore.
            // TODO: implement writing request.
            if (xSemaphoreGive(req.done) != pdPASS) {
                ESP_LOGI("sas8Gatekeeper", "xSemaphoreGive(req.wait): !pdPASS");
                abort();
            }
        } else {
            // This task was too late and request timed out. This side is
            // responsible for deleting the sync semaphore.
            ESP_LOGE("sas8Gatekeeper", "request timed out");
            vSemaphoreDelete(req.sync);
        }
    }
}

// TODO: Also need a function to cancel timer.
static void gatekeeperTimeout(TimerHandle_t xTimer) {
    sas8StatusCO2Request *req = (sas8StatusCO2Request*) pvTimerGetTimerID(xTimer);
    if (req == NULL) {
        ESP_LOGE("gatekeeperTimeout", "req: NULL");
        abort();
    }
    if (xSemaphoreTake(req->sync, 0) != pdTRUE) {
        // Sync semaphore has already been taken meaning that the gatekeeper
        // task was faster. In this case the gatekeeper task is responsible for
        // notifying the done semaphore. All that's left to be done here is to
        // delete the sync semaphore and free memory allocated to timer
        // request.
        vSemaphoreDelete(req->sync);
    } else {
        // Timer task came first to taking the sync semaphore meaning that the
        // gatekeeper task was too slow and we're dealing with a timeout. We
        // should reflect timeout state in the response and notify the done
        // channel. The gatekeeper task will be responsible for deleting the
        // sync semaphore.
        // TODO: Implement writing to the response.
        if (xSemaphoreGive(req->done) != pdPASS) {
            ESP_LOGE("gatekeeperTimeout", "xSemaphoreGive(req->done): !pdPASS");
            abort();
        }
    }
    // Timer gets a dedicated copy of request which it should delete when done.
    ESP_LOGI("heap", "- %p", req);
    free(req);
    // TODO: find the way to delete timer.
    if (xTimerDelete(xTimer, 0) != pdPASS) {
        ESP_LOGE("gatekeeperTimeout", "xTimerDelete(): !pdPASS");
        abort();
    }
}

static esp_err_t sas8KeeperReadAsync(QueueHandle_t queue, TickType_t xTicksToWait, SemaphoreHandle_t *done, TimerHandle_t *tTimeout) {
    sas8StatusCO2Request *req = malloc(sizeof(sas8StatusCO2Request));
    if (req == NULL) {
        ESP_LOGE("sas8Read", "malloc(sizeof(sas8StatusCO2Request)): NULL");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI("heap", "+ %p", req);
    req->sync = xSemaphoreCreateBinary();
    req->done = xSemaphoreCreateBinary();
    // TODO: implement response.
    // req->response = malloc(sizeof(sas8StatusCO2Response)),
    // TODO: check all allocated

    if (xSemaphoreGive(req->sync) != pdPASS) {
        // TODO: release resources
        ESP_LOGE("sas8Read", "xSemaphoreGive(req.sync): !pdPASS");
        return ESP_ERR_INVALID_STATE;
    }

    *done = req->done;

    *tTimeout = xTimerCreate("gatekeeper timeout", xTicksToWait, pdFALSE /* uxAutoReload */, req, gatekeeperTimeout);
    if (*tTimeout == NULL) {
        // TODO: release resources
        ESP_LOGE("sas8Read", "xTimerCreate(): NULL");
        return ESP_ERR_NO_MEM;
    }
    if (xTimerStart(*tTimeout, 0) != pdPASS) {
        ESP_LOGE("sas8Read", "xTimerStart(tTimeout): !pdPASS");
        // TODO: release resources
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(queue, req, 0) != pdPASS) {
        ESP_LOGE("sas8Read", "xQueueSend(): !pdPASS");
        // TODO: release resources
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t sas8ReadStatusCO2GK(QueueHandle_t q) {
}

static esp_err_t GatekeeperRequest() {
}

static esp_err_t GatekeeperRequestQ() {
    static const char* log_tag = "GatekeeperRequestQ";
    if (xQueueSend(q, req, 0) != pdPASS) {
        ESP_LOGE(log_tag, "xQueueSend() != pdPASS");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

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

static void GKTimeout(TimerHandle_t xTimer) {
    static const char* log_tag = __func__;
    GKRequest *req = (GKRequest *)pvTimerGetTimerID(xTimer);
    if (req == NULL) {
        ESP_LOGE(log_tag, "req == NULL");
        abort();
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
    // TODO: find the way to delete timer.
    if (xTimerDelete(xTimer, 0) != pdPASS) {
        ESP_LOGE(log_tag, "xTimerDelete(): !pdPASS");
        abort();
    }
}

static volatile GKTaskArgs sas8GKArgs;

typedef struct {
    uint8_t sensor_addr;
} SAS8StatusCO2Request;

typedef struct {
    GKRequest gk_req;
    SAS8StatusCO2Request args;
} SAS8GKStatusCO2Request;

typedef struct {
    esp_err_t err;
    uint16_t status;
    uint16_t co2;
} SAS8StatusCO2Response;

typedef struct {
    GKResponseStack gk_resp;
    SAS8StatusCO2Response result;
} SAS8GKStatusCO2Response;


void dummy_op(void *args, void *result, size_t result_size) {
    static const char* log_tag = __func__;
    ESP_LOGI(log_tag, "args: %p; result: %p; result_size: %d", args, result, result_size);
}

void sas8ReadStatusCO2Op(void *args, void *result, size_t result_size) {
    static const char* log_tag = __func__;
    ESP_LOGI(log_tag, "args: %p; result: %p; result_size: %d", args, result, result_size);
    SAS8StatusCO2Request *a = (SAS8StatusCO2Request*) args;
    ESP_LOGI(log_tag, "sensor_addr: %d", a->sensor_addr);
    SAS8StatusCO2Response *r = (SAS8StatusCO2Response*) result;
    r->err = sas8ReadStatusCO2(a->sensor_addr, &r->status, &r->co2);
}

// TODO: Decide what would be better in relation to timeout.
static esp_err_t SAS8ReadStatusCO2Async(TickType_t xTicksToWait, QueueHandle_t *response_q, TimerHandle_t *tTimeout) {
    static const char* log_tag = __func__;

    SAS8GKStatusCO2Request *request = malloc(sizeof(SAS8GKStatusCO2Request));
    if (request == NULL) {
        // TODO
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

    // // // // // // // // //
    *tTimeout = xTimerCreate("gatekeeper timeout", xTicksToWait, pdFALSE /* uxAutoReload */, request, GKTimeout);
    if (*tTimeout == NULL) {
        // TODO: release resources
        ESP_LOGE(log_tag, "xTimerCreate(): NULL");
        return ESP_ERR_NO_MEM;
    }
    if (xTimerStart(*tTimeout, 0) != pdPASS) {
        ESP_LOGE(log_tag, "xTimerStart(tTimeout): !pdPASS");
        // TODO: release resources
        return ESP_ERR_INVALID_STATE;
    }

    // // // // // // // // //
    *response_q = xQueueCreate(1, sizeof(SAS8GKStatusCO2Response));
    if (response_q == NULL) {
        // TODO
    }
    request->gk_req.response_q = *response_q;

    // // // // // // // // //
    request->gk_req.sync = xSemaphoreCreateBinary();
    if (request->gk_req.sync == NULL) {
        // TODO
    }
    if (xSemaphoreGive(request->gk_req.sync) != pdPASS) {
        // TODO: release resources
        ESP_LOGE(log_tag, "xSemaphoreGive(req.sync): !pdPASS");
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(sas8GKArgs.q, request, 0) != pdPASS) {
        ESP_LOGE(log_tag, "xQueueSend(): !pdPASS");
        // TODO: release resources
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
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

    ESP_LOGI(LOG_TAG, "sending request");
    // uint16_t status=0, co2=0;

    // // // // // // // // //
    QueueHandle_t done;
    TimerHandle_t tTimeout;
    esp_err_t err = SAS8ReadStatusCO2Async(pdMS_TO_TICKS(150), &done, &tTimeout);
    if (err != ESP_OK) {
        ESP_LOGE(LOG_TAG, "sas8ReadStatusCO2(...) err: 0x%x (%s).",
                 (int)err, (char*)esp_err_to_name(err));
        httpdStartResponse(connData, 503);
        httpdEndHeaders(connData);
        return HTTPD_CGI_DONE;
    }
    SAS8GKStatusCO2Response response;
    if (xQueueReceive(done, &response, portMAX_DELAY) != pdPASS) {
        ESP_LOGE(LOG_TAG, "xSemaphoreTake(done): !pdPASS");
        abort();
    }
    if (response.gk_resp.err != ESP_OK) {
        ESP_LOGE(LOG_TAG, "response.err: 0x%x (%s).",
                 (int)err, (char*)esp_err_to_name(err));
        httpdStartResponse(connData, 503);
        httpdEndHeaders(connData);
        return HTTPD_CGI_DONE;
    }
    // TODO: find the way to delete timer.
    // if (xTimerDelete(tTimeout, 0) != pdPASS) {
    //     ESP_LOGE(LOG_TAG, "xTimerDelete(tTimeout): !pdPASS");
    //     abort();
    // }
    ESP_LOGI(LOG_TAG, "Status: 0x%04x; CO2: % 4d", response.result.status, response.result.co2);

    httpdStartResponse(connData, 200);
    httpdHeader(connData, "Content-Type", "text/plain");
    httpdEndHeaders(connData);

    char output[256];
    int len=sprintf(output, "# HELP concentration_ppm CO2 concentration (ppm).\n# TYPE concentration_ppm gauge\nconcentration_ppm{substance=\"co2\"} %d\n", response.result.co2);
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
#if 0
    esp_err_t err = sas8KeeperReadAsync(sas8RequestQueue, pdMS_TO_TICKS(150), &status, &co2);
    SemaphoreHandle_t done;
    TimerHandle_t tTimeout;
    esp_err_t err = sas8KeeperReadAsync(sas8RequestQueue, pdMS_TO_TICKS(150), &done, &tTimeout);
    if (err != ESP_OK) {
        ESP_LOGE(LOG_TAG, "sas8ReadStatusCO2(...) err: 0x%x (%s).",
                 (int)err, (char*)esp_err_to_name(err));
        httpdStartResponse(connData, 503);
        httpdEndHeaders(connData);
        return HTTPD_CGI_DONE;
    }
    if (xSemaphoreTake(done, portMAX_DELAY) != pdPASS) {
        ESP_LOGE(LOG_TAG, "xSemaphoreTake(done): !pdPASS");
        abort();
    }
    // TODO: find the way to delete timer.
    // if (xTimerDelete(tTimeout, 0) != pdPASS) {
    //     ESP_LOGE(LOG_TAG, "xTimerDelete(tTimeout): !pdPASS");
    //     abort();
    // }
    ESP_LOGI(LOG_TAG, "Status: 0x%04x; CO2: % 4d", status, co2);
#endif
}

#if 0
static void updateConsole(void *arg) {
    QueueHandle_t queue = (QueueHandle_t)arg;
    if (queue == NULL) {
        ESP_LOGE("updateConsole", "queue == NULL");
        abort();
    }
    for(;;vTaskDelay(pdMS_TO_TICKS(14000))) {
        uint16_t status, co2;
        esp_err_t err = sas8KeeperRead(sas8RequestQueue, pdMS_TO_TICKS(150), &status, &co2);
        if (err != ESP_OK) {
            ESP_LOGE("updateConsole", "sas8ReadStatusCO2(...) err: 0x%x (%s).",
                     (int)err, (char*)esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(LOG_TAG, "Status: 0x%04x; CO2: % 4d", status, co2);
    }
}
#endif


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
    static const char* log_tag = __func__;

    ESP_ERROR_CHECK(master_init());

    sas8GKArgs.request_size = sizeof(SAS8GKStatusCO2Request);
    sas8GKArgs.q = xQueueCreate(16, sas8GKArgs.request_size);
    if (sas8GKArgs.q == NULL) {
        ESP_LOGE(log_tag, "sas8GKArgs.q == NULL");
        abort();
    }
    xTaskCreate(GatekeeperTask, "sas8Gatekeeper", 2048, (void *)&sas8GKArgs, 1, NULL);
#if 0
    xTaskCreate(updateConsole, "updateConsole", 2048, (void *)sas8RequestQueue, 1, NULL);
#endif

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
