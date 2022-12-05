/* 
   EN: Module for sending data to external services via HTTP protocol
   RU: Модуль для отправки данных на внешние сервисы через HTTP-протокол
   --------------------------
   (с) 2020-2022 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __RE_DATASEND_H__
#define __RE_DATASEND_H__

#include <sys/types.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include "rLog.h"
#include "rTypes.h"
#include "rStrings.h"
#include "reLed.h"
#include "reWiFi.h"
#include "reEvents.h"
#include "reStates.h"
#include "reEsp32.h"
#include "project_config.h"
#include "def_consts.h"

#if CONFIG_DATASEND_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  EDS_OPENMON = 0,
  EDS_NARODMON,
  EDS_THINGSPEAK
} ext_data_service_t;

/**
 * EN: Task management: create, suspend, resume and delete
 * RU: Управление задачей: создание, приостановка, восстановление и удаление
 **/
bool dsTaskCreate(bool createSuspended);
bool dsTaskSuspend();
bool dsTaskResume();
bool dsTaskDelete();

/**
 * EN: Adding a new channel to the list
 * RU: Добавление нового канала в список
 * 
 * @param kind - Service type / Тип сервиса
 * @param uid  - Channel ID / Идентификатор канала
 * @param key  - Channel token / Токен авторизации
 * @param min_interval - Minimum interval for sending data / Минимальный интервал отправки данных
 * @param err_interval - Retry interval in case of error / Интервал повторной отправки в случае ошибки
 **/
bool dsChannelInit(ext_data_service_t kind, uint32_t uid, const char *key, uint32_t min_interval, uint32_t err_interval);

/**
 * EN: Sending data to the specified channel.
 * If little time has passed since the last data sent to the channel, the data will be queued.
 * If there is already data in the queue for this channel, it will be overwritten with new data.
 * 
 * RU: Отправка данных в заданный канал.
 * Если с момента последней отправки данных в канал прошло мало времени, то данные будут поставлены в очередь.
 * Если в очереди на данный канал уже есть данные, то они будут перезаписаны новыми данными.
 * 
 * @param kind - Service type / Тип сервиса
 * @param uid  - Channel ID / Идентификатор канала
 * @param data - Data in the format f1=VALUE1&f2=VALUE2&f3=VALUE3... / Данные в формате p1=ЗНАЧ1&p2=ЗНАЧ2&p3=ЗНАЧ3...
 * @param free_data -  Delete the original data after the data has been queued / Удалить исходную строку данных после помещения данных в очередь
 **/
bool dsSend(ext_data_service_t kind, uint32_t uid, char *data, bool free_data);

#ifdef __cplusplus
}
#endif

#endif // __RE_DATASEND_H__

#endif // CONFIG_DATASEND_ENABLE
