#include "reDataSend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" 
#include "freertos/semphr.h" 
#include "esp_http_client.h"
#include "mbedtls/ssl.h"
#include "sys/queue.h"
#include <stdlib.h>

#if CONFIG_DATASEND_ENABLE

typedef struct dataChannel_t {
  ext_data_service_t kind;
  uint32_t uid;
  const char* key;
  TickType_t interval_min;
  TickType_t interval_err;
  TickType_t send_next;
  char* data;
  STAILQ_ENTRY(dataChannel_t) next;
} dataChannel_t;
typedef struct dataChannel_t *dataChannelHandle_t;

STAILQ_HEAD(dataChannelsHead_t, dataChannel_t);

typedef struct {
  ext_data_service_t kind;
  uint32_t uid;
  char* data;
} dataSendQueueItem_t;  

#define DATASEND_QUEUE_ITEM_SIZE sizeof(dataSendQueueItem_t)

TaskHandle_t _dataSendTask;
QueueHandle_t _dataSendQueue = nullptr;
dataChannelsHead_t *_dataSendChannels = nullptr;
#if defined(CONFIG_DATASEND_SEND_BUFFER_SIZE) && (CONFIG_DATASEND_SEND_BUFFER_SIZE > 0)
  #define CONFIG_DATASEND_USE_STATIC_BUFFER 1
  static char _dsSendBuf[CONFIG_DATASEND_SEND_BUFFER_SIZE];
#else
  #define CONFIG_DATASEND_STATIC_BUFFER 0
#endif // CONFIG_DATASEND_SEND_BUFFER_SIZE

static const char* logTAG = "SEND";
static const char* dataSendTaskName = "http_send";
static const char* dataSendUserAgent = "ESP32";

#if CONFIG_DATASEND_STATIC_ALLOCATION
StaticQueue_t _dataSendQueueBuffer;
StaticTask_t _dataSendTaskBuffer;
StackType_t _dataSendTaskStack[CONFIG_DATASEND_STACK_SIZE];
uint8_t _dataSendQueueStorage[CONFIG_DATASEND_QUEUE_SIZE * DATASEND_QUEUE_ITEM_SIZE];
#endif // CONFIG_DATASEND_STATIC_ALLOCATION

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------- Routines ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_OPENMON_ENABLE
  #ifndef CONFIG_OPENMON_TLS_PEM_STORAGE
    #define CONFIG_OPENMON_TLS_PEM_STORAGE TLS_CERT_BUFFER
  #endif // CONFIG_OPENMON_TLS_PEM_STORAGE
  static esp_err_t last_queue_error_openmon = ESP_OK;
  static esp_err_t last_api_error_openmon = ESP_OK;
#endif // CONFIG_OPENMON_ENABLE

#if CONFIG_OPENMON_ENABLE && (CONFIG_OPENMON_TLS_PEM_STORAGE == TLS_CERT_BUFFER)
  extern const char api_openmon_pem_start[] asm(CONFIG_OPENMON_TLS_PEM_START);
#endif // CONFIG_OPENMON_TLS_PEM_START

#if CONFIG_NARODMON_ENABLE
  #ifndef CONFIG_NARODMON_TLS_PEM_STORAGE
    #define CONFIG_NARODMON_TLS_PEM_STORAGE TLS_CERT_BUFFER
  #endif // CONFIG_NARODMON_TLS_PEM_STORAGE
  static esp_err_t last_queue_error_narodmon = ESP_OK;
  static esp_err_t last_api_error_narodmon = ESP_OK;
#endif // CONFIG_NARODMON_ENABLE

#if CONFIG_NARODMON_ENABLE && (CONFIG_NARODMON_TLS_PEM_STORAGE == TLS_CERT_BUFFER)
  extern const char api_narodmon_pem_start[] asm(CONFIG_NARODMON_TLS_PEM_START);
#endif // CONFIG_NARODMON_TLS_PEM_START

#if CONFIG_THINGSPEAK_ENABLE
  #ifndef CONFIG_THINGSPEAK_TLS_PEM_STORAGE
    #define CONFIG_THINGSPEAK_TLS_PEM_STORAGE TLS_CERT_BUFFER
  #endif // CONFIG_THINGSPEAK_TLS_PEM_STORAGE
  static esp_err_t last_queue_error_thingspeak = ESP_OK;
  static esp_err_t last_api_error_thingspeak = ESP_OK;
#endif // CONFIG_THINGSPEAK_ENABLE

#if CONFIG_THINGSPEAK_ENABLE && (CONFIG_THINGSPEAK_TLS_PEM_STORAGE == TLS_CERT_BUFFER)
  extern const char api_thingspeak_pem_start[] asm(CONFIG_THINGSPEAK_TLS_PEM_START);
#endif // CONFIG_THINGSPEAK_TLS_PEM_START

const char* kind2tag(ext_data_service_t kind)
{
  switch (kind) {
    #if CONFIG_OPENMON_ENABLE
      case EDS_OPENMON:    
        return CONFIG_OPENMON_API_LOGTAG;
    #endif // CONFIG_OPENMON_ENABLE

    #if CONFIG_NARODMON_ENABLE
      case EDS_NARODMON:   
        return CONFIG_NARODMON_API_LOGTAG;
    #endif // CONFIG_NARODMON_ENABLE

    #if CONFIG_THINGSPEAK_ENABLE
      case EDS_THINGSPEAK: 
        return CONFIG_THINGSPEAK_API_LOGTAG;
    #endif // CONFIG_THINGSPEAK_ENABLE
    
    default:             
      return logTAG;
  };
}

void fixErrorQueue(ext_data_service_t kind, esp_err_t state)
{
  switch (kind) {
    #if CONFIG_OPENMON_ENABLE
      case EDS_OPENMON:    
        if (last_queue_error_openmon != state) {
          last_queue_error_openmon = state;
          eventLoopPostError(RE_SYS_OPENMON_ERROR, state);
        };
        break;
    #endif // CONFIG_OPENMON_ENABLE

    #if CONFIG_NARODMON_ENABLE
      case EDS_NARODMON:   
        if (last_queue_error_narodmon != state) {
          last_queue_error_narodmon = state;
          eventLoopPostError(RE_SYS_NARODMON_ERROR, state);
        };
        break;
    #endif // CONFIG_NARODMON_ENABLE

    #if CONFIG_THINGSPEAK_ENABLE
      case EDS_THINGSPEAK: 
        if (last_queue_error_thingspeak != state) {
          last_queue_error_thingspeak = state;
          eventLoopPostError(RE_SYS_THINGSPEAK_ERROR, state);
        };
        break;
    #endif // CONFIG_THINGSPEAK_ENABLE

    default:
      break;
  };
}

void fixErrorApi(ext_data_service_t kind, esp_err_t state)
{
  switch (kind) {
    #if CONFIG_OPENMON_ENABLE
      case EDS_OPENMON:    
        if (last_api_error_openmon != state) {
          last_api_error_openmon = state;
          eventLoopPostError(RE_SYS_OPENMON_ERROR, state);
        };
        break;
    #endif // CONFIG_OPENMON_ENABLE

    #if CONFIG_NARODMON_ENABLE
      case EDS_NARODMON:   
        if (last_api_error_narodmon != state) {
          last_api_error_narodmon = state;
          eventLoopPostError(RE_SYS_NARODMON_ERROR, state);
        };
        break;
    #endif // CONFIG_NARODMON_ENABLE

    #if CONFIG_THINGSPEAK_ENABLE
      case EDS_THINGSPEAK: 
        if (last_api_error_thingspeak != state) {
          last_api_error_thingspeak = state;
          eventLoopPostError(RE_SYS_THINGSPEAK_ERROR, state);
        };
        break;
    #endif // CONFIG_THINGSPEAK_ENABLE

    default:
      break;
  };
}

void dsHttpConfig(ext_data_service_t kind, esp_http_client_config_t *config)
{
  memset(config, 0, sizeof(esp_http_client_config_t));
  
  config->method = HTTP_METHOD_GET;
  switch (kind) {
    #if CONFIG_OPENMON_ENABLE
      case EDS_OPENMON:
        config->host = CONFIG_OPENMON_API_HOST;
        config->port = CONFIG_OPENMON_API_PORT;
        config->path = CONFIG_OPENMON_API_SEND_PATH;
        config->timeout_ms = CONFIG_OPENMON_API_TIMEOUT_MS;
        #if CONFIG_OPENMON_TLS_PEM_STORAGE == TLS_CERT_BUFFER
          config->cert_pem = api_openmon_pem_start;
          config->use_global_ca_store = false;
        #elif CONFIG_OPENMON_TLS_PEM_STORAGE == TLS_CERT_GLOBAL
          config->use_global_ca_store = true;
        #elif CONFIG_OPENMON_TLS_PEM_STORAGE == TLS_CERT_BUNDLE
          config->crt_bundle_attach = esp_crt_bundle_attach;
          config->use_global_ca_store = false;
        #endif // CONFIG_OPENMON_TLS_PEM_STORAGE
        break;
    #endif // CONFIG_OPENMON_ENABLE
    #if CONFIG_NARODMON_ENABLE
      case EDS_NARODMON:
        config->host = CONFIG_NARODMON_API_HOST;
        config->port = CONFIG_NARODMON_API_PORT;
        config->path = CONFIG_NARODMON_API_SEND_PATH;
        config->timeout_ms = CONFIG_NARODMON_API_TIMEOUT_MS;
        #if CONFIG_NARODMON_TLS_PEM_STORAGE == TLS_CERT_BUFFER
          config->cert_pem = api_narodmon_pem_start;
          config->use_global_ca_store = false;
        #elif CONFIG_NARODMON_TLS_PEM_STORAGE == TLS_CERT_GLOBAL
          config->use_global_ca_store = true;
        #elif CONFIG_NARODMON_TLS_PEM_STORAGE == TLS_CERT_BUNDLE
          config->crt_bundle_attach = esp_crt_bundle_attach;
          config->use_global_ca_store = false;
        #endif // CONFIG_NARODMON_TLS_PEM_STORAGE
        break;
    #endif // CONFIG_NARODMON_ENABLE
    #if CONFIG_THINGSPEAK_ENABLE
      case EDS_THINGSPEAK:
        config->host = CONFIG_THINGSPEAK_API_HOST;
        config->port = CONFIG_THINGSPEAK_API_PORT;
        config->path = CONFIG_THINGSPEAK_API_SEND_PATH;
        config->timeout_ms = CONFIG_THINGSPEAK_API_TIMEOUT_MS;
        #if CONFIG_THINGSPEAK_TLS_PEM_STORAGE == TLS_CERT_BUFFER
          config->cert_pem = api_thingspeak_pem_start;
          config->use_global_ca_store = false;
        #elif CONFIG_THINGSPEAK_TLS_PEM_STORAGE == TLS_CERT_GLOBAL
          config->use_global_ca_store = true;
        #elif CONFIG_THINGSPEAK_TLS_PEM_STORAGE == TLS_CERT_BUNDLE
          config->crt_bundle_attach = esp_crt_bundle_attach;
          config->use_global_ca_store = false;
        #endif // CONFIG_THINGSPEAK_TLS_PEM_STORAGE
        break;
    #endif // CONFIG_THINGSPEAK_ENABLE
    default: break;
  };

  config->user_agent = dataSendUserAgent;
  config->transport_type = HTTP_TRANSPORT_OVER_SSL;
  config->skip_cert_common_name_check = false;
  config->is_async = false;
}

#if CONFIG_DATASEND_USE_STATIC_BUFFER
  bool dsCreateApiRequest(dataChannelHandle_t channel) {
    switch (channel->kind) {
      #if CONFIG_OPENMON_ENABLE
        case EDS_OPENMON:    
          return format_string(_dsSendBuf, CONFIG_DATASEND_SEND_BUFFER_SIZE,
            CONFIG_OPENMON_API_SEND_VALUES, channel->uid, channel->key, channel->data) > 0;
      #endif // CONFIG_OPENMON_ENABLE
      #if CONFIG_NARODMON_ENABLE
        case EDS_NARODMON:   
          return format_string(_dsSendBuf, CONFIG_DATASEND_SEND_BUFFER_SIZE,
            CONFIG_NARODMON_API_SEND_VALUES, channel->key, channel->data) > 0;
      #endif // CONFIG_NARODMON_ENABLE
      #if CONFIG_THINGSPEAK_ENABLE
        case EDS_THINGSPEAK: 
          return format_string(_dsSendBuf, CONFIG_DATASEND_SEND_BUFFER_SIZE,
            CONFIG_THINGSPEAK_API_SEND_VALUES, channel->key, channel->data) > 0;
      #endif // CONFIG_THINGSPEAK_ENABLE
      default: return false;
    };
  };
#else
  char* dsCreateApiRequest(dataChannelHandle_t channel) {
    switch (channel->kind) {
      #if CONFIG_OPENMON_ENABLE
        case EDS_OPENMON:    
          return malloc_stringf(CONFIG_OPENMON_API_SEND_VALUES, channel->uid, channel->key, channel->data);
      #endif // CONFIG_OPENMON_ENABLE
      #if CONFIG_NARODMON_ENABLE
        case EDS_NARODMON:
          return malloc_stringf(CONFIG_NARODMON_API_SEND_VALUES, channel->key, channel->data);
      #endif // CONFIG_NARODMON_ENABLE
      #if CONFIG_THINGSPEAK_ENABLE
        case EDS_THINGSPEAK: 
          return malloc_stringf(CONFIG_THINGSPEAK_API_SEND_VALUES, channel->key, channel->data);
      #endif // CONFIG_THINGSPEAK_ENABLE
      default: return nullptr;
    };
  };
#endif // CONFIG_DATASEND_USE_STATIC_BUFFER

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Call API --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

esp_err_t dsCallDataApi(dataChannelHandle_t channel)
{
  if (channel->data == nullptr) return ESP_OK;

  // Create the text of the GET request
  #if CONFIG_DATASEND_USE_STATIC_BUFFER
    if (!dsCreateApiRequest(channel)) return ESP_ERR_INVALID_SIZE;
  #else
    char* _dsSendBuf = dsCreateApiRequest(channel);
    if (_dsSendBuf == nullptr) return ESP_ERR_NO_MEM;
  #endif // CONFIG_DATASEND_USE_STATIC_BUFFER
  
  // Make a request to the API
  esp_err_t ret = ESP_FAIL;
  esp_http_client_config_t cfgHttp;
  dsHttpConfig(channel->kind, &cfgHttp);
  cfgHttp.query = _dsSendBuf;
  esp_http_client_handle_t client = esp_http_client_init(&cfgHttp);
  if (client != nullptr) {
    ret = esp_http_client_perform(client);
    if (ret == ESP_OK) {
      int api_code = esp_http_client_get_status_code(client);
      if (api_code == HttpStatus_Ok) {
        ret = ESP_OK;
        rlog_i(kind2tag(channel->kind), "Data sent to [%d]: %s", channel->uid, _dsSendBuf);
      } else {
        ret = ESP_ERR_HTTP_BASE + api_code;
        rlog_e(kind2tag(channel->kind), "Failed to send message, API error code: #%d!", api_code);
      };
      // Flashing system LED
      #if CONFIG_SYSLED_SEND_ACTIVITY
      ledSysActivity();
      #endif // CONFIG_SYSLED_SEND_ACTIVITY
    }
    else {
      rlog_e(kind2tag(channel->kind), "Failed to complete request to [%d]: %d %s", channel->uid, ret, esp_err_to_name(ret));
    };
    esp_http_client_cleanup(client);
  } else {
    ret = ESP_ERR_INVALID_STATE;
    rlog_e(kind2tag(channel->kind), "Failed to complete request to [%d]", channel->uid);
  };

  // Remove the request from memory
  #if !CONFIG_DATASEND_USE_STATIC_BUFFER
    if (_dsSendBuf) free(_dsSendBuf);
  #endif // CONFIG_DATASEND_USE_STATIC_BUFFER

  return ret;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------- Channels ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool dsChannelsInit()
{
  _dataSendChannels = (dataChannelsHead_t*)esp_calloc(1, sizeof(dataChannelsHead_t));
  if (_dataSendChannels == nullptr) {
    rlog_e(logTAG, "Failed to create list of channels");
  };
  STAILQ_INIT(_dataSendChannels);
  return (_dataSendChannels != nullptr);
}

bool dsChannelInit(ext_data_service_t kind, uint32_t uid, const char *key, uint32_t min_interval, uint32_t err_interval)
{
  if (_dataSendChannels == nullptr) {
    if (!dsChannelsInit()) return false;
  };

  dataChannelHandle_t channel = (dataChannelHandle_t)esp_calloc(1, sizeof(dataChannel_t));
  if (channel == nullptr) {
    rlog_e(logTAG, "Failed to allocate memory for external channel data");
    return false;
  };

  channel->kind = kind;
  channel->uid = uid;
  channel->key = key;
  channel->interval_min = pdMS_TO_TICKS(min_interval);
  channel->interval_err = pdMS_TO_TICKS(err_interval);
  channel->send_next = 0;
  channel->data = nullptr;
  STAILQ_INSERT_TAIL(_dataSendChannels, channel, next);

  return true;
}

dataChannelHandle_t dsChannelFind(ext_data_service_t kind, uint32_t uid)
{
  dataChannelHandle_t item;
  STAILQ_FOREACH(item, _dataSendChannels, next) {
    if ((item->kind == kind) && (item->uid == uid)) {
      return item;
    }
  }
  return nullptr;
} 

void dsChannelsFree()
{
  dataChannelHandle_t item = STAILQ_FIRST(_dataSendChannels);
  while (item != nullptr) {
    STAILQ_REMOVE_HEAD(_dataSendChannels, next);
    if (item->data != nullptr) free(item->data);
    free(item);
  };
  free(_dataSendChannels);
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------ Adding data to the send queue ----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool dsSend(ext_data_service_t kind, const uint32_t uid, char *data, bool free_data)
{
  if (data == nullptr) {
    rlog_w(kind2tag(kind), "No data to send to channel %d", uid);
    return false;
  };

  bool ret = false;
  if (_dataSendQueue != nullptr) {
    dataSendQueueItem_t* item = (dataSendQueueItem_t*)esp_calloc(1, sizeof(dataSendQueueItem_t));
    if (item != nullptr) {
      item->kind = kind;
      item->uid = uid;
      item->data = malloc_string(data);
      if (xQueueSend(_dataSendQueue, &item, pdMS_TO_TICKS(CONFIG_DATASEND_QUEUE_WAIT)) == pdPASS) {
        ret = true;
        fixErrorQueue(kind, ESP_OK);
      } else {
        rlog_e(kind2tag(kind), "Failed to append message to queue [ %s ]!", dataSendTaskName);
        fixErrorQueue(kind, ESP_ERR_NOT_FINISHED);
        if (item->data) free(item->data);
        free(item);
      };
    } else {
      rlog_e(kind2tag(kind), "Failed to create message to queue [ %s ]!", dataSendTaskName);
      fixErrorQueue(kind, ESP_FAIL);
    };
  };
  if (free_data) free(data);
  return ret;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Queue processing --------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void dsTaskExec(void *pvParameters)
{
  dataSendQueueItem_t* item = nullptr;
  dataChannelHandle_t channel = nullptr;
  TickType_t wait_queue = portMAX_DELAY;
  
  while (true) {
    // Receiving new data
    while (xQueueReceive(_dataSendQueue, &item, wait_queue) == pdPASS) {
      wait_queue = 1;
      channel = dsChannelFind(item->kind, item->uid);
      if (channel) {
        // Replacing channel data with new ones from the transporter
        if (channel->data != nullptr) free(channel->data);
        channel->data = item->data;
      } else {
        rlog_e(logTAG, "Channel [%d] not found!", item->uid);
        if (item->data != nullptr) free(item->data);
      };
      item->data = nullptr;
      free(item);
    };

    // Check internet availability 
    if (statesInetIsAvailabled()) {
      // Sending data
      channel = nullptr;
      STAILQ_FOREACH(channel, _dataSendChannels, next) {
        if ((channel->data != nullptr) && (channel->send_next < xTaskGetTickCount())) {
          // Attempt to send data
          esp_err_t err = dsCallDataApi(channel);
          if (err == ESP_OK) {
            // Calculate the time of the next dispatch in the given channel
            channel->send_next = xTaskGetTickCount() + channel->interval_min;
            if (channel->data) {
              free(channel->data);
              channel->data = nullptr;
            };
          } else {
            // Calculate the time of the next dispatch in the given channel
            channel->send_next = xTaskGetTickCount() + channel->interval_err;
          };
          // If the return code has changed, send an event to the event loop
          fixErrorApi(channel->kind, err);
          // Reset watchdog (wait 1 tick (~100 ms))
          vTaskDelay(1);
        };
      };

      // Find the minimum delay before the next sending to the channel
      channel = nullptr;
      wait_queue = portMAX_DELAY;
      STAILQ_FOREACH(channel, _dataSendChannels, next) {
        if (channel->data != nullptr) {
          TickType_t send_delay = 1;
          if (channel->send_next > xTaskGetTickCount()) {
            send_delay = channel->send_next - xTaskGetTickCount();
          };
          if (send_delay < wait_queue) {
            wait_queue = send_delay;
          };
        };
      };
    } else {
      // If the Internet is not available, repeat the check every second
      wait_queue = pdMS_TO_TICKS(1000); 
    };
  };
  dsTaskDelete();
}

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- Task routines ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void dsTaskError(esp_err_t state)
{
  #if CONFIG_OPENMON_ENABLE
    eventLoopPostError(RE_SYS_OPENMON_ERROR, state);
  #endif // CONFIG_OPENMON_ENABLE
  #if CONFIG_NARODMON_ENABLE
    eventLoopPostError(RE_SYS_NARODMON_ERROR, state);
  #endif // CONFIG_NARODMON_ENABLE
  #if CONFIG_THINGSPEAK_ENABLE
    eventLoopPostError(RE_SYS_THINGSPEAK_ERROR, state);
  #endif // CONFIG_THINGSPEAK_ENABLE
}

bool dsTaskSuspend()
{
  if ((_dataSendTask) && (eTaskGetState(_dataSendTask) != eSuspended)) {
    vTaskSuspend(_dataSendTask);
    if (eTaskGetState(_dataSendTask) == eSuspended) {
      rloga_d("Task [ %s ] has been suspended", dataSendTaskName);
      return true;
    } else {
      rloga_e("Failed to suspend task [ %s ]!", dataSendTaskName);
    };
  };
  return false;  
}

bool dsTaskResume()
{
  if ((_dataSendTask) && (eTaskGetState(_dataSendTask) == eSuspended)) {
    vTaskResume(_dataSendTask);
    if (eTaskGetState(_dataSendTask) != eSuspended) {
      rloga_i("Task [ %s ] has been successfully resumed", dataSendTaskName);
      return true;
    } else {
      rloga_e("Failed to resume task [ %s ]!", dataSendTaskName);
    };
  };
  return false;  
}

bool dsEventHandlerRegister();
bool dsTaskCreate(bool createSuspended) 
{
  if (!_dataSendTask) {
    if (_dataSendChannels == nullptr) {
      if (!dsChannelsInit()) {
        dsTaskError(ESP_FAIL);
        return false;
      };
    };

    if (!_dataSendQueue) {
      #if CONFIG_DATASEND_STATIC_ALLOCATION
      _dataSendQueue = xQueueCreateStatic(CONFIG_DATASEND_QUEUE_SIZE, DATASEND_QUEUE_ITEM_SIZE, &(_dataSendQueueStorage[0]), &_dataSendQueueBuffer);
      #else
      _dataSendQueue = xQueueCreate(CONFIG_DATASEND_QUEUE_SIZE, DATASEND_QUEUE_ITEM_SIZE);
      #endif // CONFIG_DATASEND_STATIC_ALLOCATION
      if (!_dataSendQueue) {
        dsChannelsFree();
        rloga_e("Failed to create a queue for sending data to external services!");
        dsTaskError(ESP_FAIL);
        return false;
      };
    };
    
    #if CONFIG_DATASEND_STATIC_ALLOCATION
    _dataSendTask = xTaskCreateStaticPinnedToCore(dsTaskExec, dataSendTaskName, CONFIG_DATASEND_STACK_SIZE, nullptr, CONFIG_TASK_PRIORITY_DATASEND, _dataSendTaskStack, &_dataSendTaskBuffer, CONFIG_TASK_CORE_DATASEND); 
    #else
    xTaskCreatePinnedToCore(dsTaskExec, dataSendTaskName, CONFIG_DATASEND_STACK_SIZE, nullptr, CONFIG_TASK_PRIORITY_DATASEND, &_dataSendTask, CONFIG_TASK_CORE_DATASEND); 
    #endif // CONFIG_DATASEND_STATIC_ALLOCATION
    if (_dataSendTask == nullptr) {
      vQueueDelete(_dataSendQueue);
      dsChannelsFree();
      rloga_e("Failed to create task for sending data to external services!");
      dsTaskError(ESP_FAIL);
      return false;
    } else {
      if (createSuspended) {
        rloga_i("Task [ %s ] has been successfully created", dataSendTaskName);
        dsTaskSuspend();
        dsTaskError(ESP_FAIL);
      } else {
        rloga_i("Task [ %s ] has been successfully started", dataSendTaskName);
        dsTaskError(ESP_OK);
      };
      return dsEventHandlerRegister();
    };
  };
  return false;
}

bool dsTaskDelete()
{
  dsChannelsFree();

  if (_dataSendQueue != nullptr) {
    vQueueDelete(_dataSendQueue);
    _dataSendQueue = nullptr;
  };

  if (_dataSendTask != nullptr) {
    vTaskDelete(_dataSendTask);
    _dataSendTask = nullptr;
    rloga_d("Task [ %s ] was deleted", dataSendTaskName);
  };
  
  return true;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Events handlers ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void dsWiFiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_INET_PING_OK) {
    dsTaskResume();
  };
}

static void dsOtaEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if ((event_id == RE_SYS_OTA) && (event_data)) {
    re_system_event_data_t* data = (re_system_event_data_t*)event_data;
    if (data->type == RE_SYS_SET) {
      dsTaskSuspend();
    } else {
      dsTaskResume();
    };
  };
}

bool dsEventHandlerRegister()
{
  return eventHandlerRegister(RE_WIFI_EVENTS, RE_INET_PING_OK, &dsWiFiEventHandler, nullptr)
      && eventHandlerRegister(RE_SYSTEM_EVENTS, RE_SYS_OTA, &dsOtaEventHandler, nullptr);
};

#endif // CONFIG_DATASEND_ENABLE
