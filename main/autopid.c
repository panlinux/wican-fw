/*
 * This file is part of the WiCAN project.
 *
 * Copyright (C) 2022  Meatpi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>
#include "driver/twai.h"
#include "esp_timer.h"
#include "esp_system.h" 
#include "lwip/sockets.h"
#include "elm327.h"
#include "autopid.h"
#include "expression_parser.h"
#include "mqtt.h"
#include "cJSON.h"
#include "config_server.h"

#define TAG __func__

#define RANDOM_MIN          5
#define RANDOM_MAX          50
#define ECU_INIT_CMD        "0100\r"

static char auto_pid_buf[BUFFER_SIZE];
static autopid_state_t  autopid_state = CONNECT_CHECK;
static QueueHandle_t autopidQueue;
static pid_req_t *pid_req;
static size_t num_of_pids = 0;
static char* initialisation = NULL;       // Initialisation string

								 															
static void parse_elm327_response(char *buffer, unsigned char *data, uint32_t *data_length) 
{
    int i = 0, k = 0;
    char *frame;
    char temp_buffer[32];

    // Split the frames by '\r' or '\r\n'
    frame = strtok(buffer, "\r\n");
    while (frame != NULL) 
    {
        if (frame[strlen(frame) - 1] == '>') 
        {
            frame[strlen(frame) - 1] = '\0';  // Remove the '>' from the last frame
        }

        // Extract and print the data before the first space (ID)
        sscanf(frame, "%s", temp_buffer);

        // Determine the start index based on ID length (standard ID vs. extended ID)
        int start_index = (strchr(frame, ' ') - frame) + 1;

        // Skip the header and frame length byte
        for (i = start_index + 3; i < strlen(frame); i += 3) 
        {
            if (frame[i] == '\0') break;
            // Convert hex string to byte and store in data array
            char hex_byte[3] = {frame[i], frame[i+1], '\0'};
            data[k++] = (unsigned char) strtol(hex_byte, NULL, 16);
        }

        frame = strtok(NULL, "\r\n");
    }

    *data_length = k;
}

static void append_to_buffer(char *buffer, const char *new_data) 
{
    if (strlen(buffer) + strlen(new_data) < BUFFER_SIZE) 
    {
        strcat(buffer, new_data);
    }
    else
    {
        ESP_LOGE(TAG, "Failed add data to buffer");
    }
}

void autopid_parser(char *str, uint32_t len, QueueHandle_t *q)
{
    static response_t response;
    if (strlen(str) != 0)
    {
        ESP_LOGI(__func__, "%s", str);

        append_to_buffer(auto_pid_buf, str);

        if (strchr(str, '>') != NULL) 
        {
            if(strstr(str, "NO DATA") == NULL && strstr(str, "ERROR") == NULL)
            {
                // Parse the accumulated buffer
                parse_elm327_response(auto_pid_buf,  response.data, &response.length);
                if (xQueueSend(autopidQueue, &response, pdMS_TO_TICKS(1000)) != pdPASS)
                {
                    ESP_LOGE(TAG, "Failed to send to queue");
                }
            }
            else
            {
                ESP_LOGE(__func__, "Error response: %s", auto_pid_buf);
            }
            // Clear the buffer after parsing
            auto_pid_buf[0] = '\0';
        }
    }
}

static void send_commands(char *commands, uint32_t delay_ms)
{
    char *cmd_start = commands;
    char *cmd_end;
    twai_message_t tx_msg;
    
    while ((cmd_end = strchr(cmd_start, '\r')) != NULL) 
    {
        size_t cmd_len = cmd_end - cmd_start + 1; // +1 to include '\r'
        char str_send[cmd_len + 1]; // +1 for null terminator
        strncpy(str_send, cmd_start, cmd_len);
        str_send[cmd_len] = '\0'; // Null-terminate the command string

        elm327_process_cmd((uint8_t *)str_send, cmd_len, &tx_msg, &autopidQueue);

        cmd_start = cmd_end + 1; // Move to the start of the next command
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void autopid_task(void *pvParameters)
{
    static char default_init[] = "ati\ratd\rate0\rath1\ratl0\rats1\ratsp6\r";
    static response_t response;
    twai_message_t tx_msg;

    autopidQueue = xQueueCreate(QUEUE_SIZE, sizeof(response_t));
    if (autopidQueue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create queue");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    send_commands(default_init, 50);
    // if(initialisation != NULL) 
    // {
    //     send_commands(initialisation, 50);
    // }
    
    // vTaskDelay(pdMS_TO_TICKS(1000));

    while ((xQueueReceive(autopidQueue, &response, pdMS_TO_TICKS(1000)) == pdPASS));
    
    for(uint32_t i = 0; i < num_of_pids; i++)
    {
        strcat(pid_req[i].pid_command, "\r");
    }

    while (1)
    {
        if(num_of_pids && mqtt_connected())
        {
            switch(autopid_state)
            {
                case CONNECT_CHECK:
                {
                    if(initialisation != NULL) 
                    {
                        send_commands(initialisation, 100);
                    }
                    while ((xQueueReceive(autopidQueue, &response, pdMS_TO_TICKS(1000)) == pdPASS));
                    send_commands(ECU_INIT_CMD, 1000);
                    if(((xQueueReceive(autopidQueue, &response, pdMS_TO_TICKS(1000)) == pdPASS)))
                    {
                        autopid_state = CONNECT_NOTIFY;
                        ESP_LOGI(TAG, "State change --> CONNECT_NOTIFY");
                    }
                    else
                    {
                        vTaskDelay(pdMS_TO_TICKS(3000));
                    }
                    break;
                }
                case CONNECT_NOTIFY:
                {
                    cJSON *rsp_json = cJSON_CreateObject();
                    if (rsp_json == NULL) 
                    {
                        ESP_LOGI(TAG, "Failed to create cJSON object");
                        break;
                    }

                    cJSON_AddStringToObject( rsp_json, "ecu_status", "online");
                    char *response_str = cJSON_PrintUnformatted(rsp_json);

                    mqtt_publish(config_server_get_mqtt_status_topic(), response_str, 0, 0, 0);
                    free(response_str);
                    cJSON_Delete(rsp_json);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    autopid_state = READ_PID;
                    ESP_LOGI(TAG, "State change --> READ_PID");
                    break;
                }
                case DISCONNECT_NOTIFY:
                {
                    cJSON *rsp_json = cJSON_CreateObject();
                    if (rsp_json == NULL) 
                    {
                        ESP_LOGI(TAG, "Failed to create cJSON object");
                        break;
                    }

                    cJSON_AddStringToObject( rsp_json, "ecu_status", "offline");
                    char *response_str = cJSON_PrintUnformatted(rsp_json);

                    mqtt_publish(config_server_get_mqtt_status_topic(), response_str, 0, 0, 0);
                    free(response_str);
                    cJSON_Delete(rsp_json);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    autopid_state = CONNECT_CHECK;
                    ESP_LOGI(TAG, "State change --> CONNECT_CHECK");
                    break;
                }

                case READ_PID:
                {
                    uint8_t pid_no_response = 0;

                    for(uint32_t i = 0; i < num_of_pids; i++)
                    {
                        if( esp_timer_get_time() > pid_req[i].timer )
                        {
                            pid_req[i].timer = esp_timer_get_time() + pid_req[i].period*1000;
                            pid_req[i].timer += RANDOM_MIN + (esp_random() % (RANDOM_MAX - RANDOM_MIN + 1));

                            elm327_process_cmd((uint8_t*)pid_req[i].pid_command , strlen(pid_req[i].pid_command), &tx_msg, &autopidQueue);
                            ESP_LOGI(TAG, "Sending command: %s", pid_req[i].pid_command);
                            if (xQueueReceive(autopidQueue, &response, pdMS_TO_TICKS(1000)) == pdPASS)
                            {
                                double result;
                                static char hex_rsponse[256];

                                ESP_LOGI(TAG, "Received response for: %s", pid_req[i].pid_command);
                                ESP_LOGI(TAG, "Response length: %lu", response.length);
                                ESP_LOG_BUFFER_HEXDUMP(TAG, response.data, response.length, ESP_LOG_INFO);
                                if(evaluate_expression((uint8_t*)pid_req[i].expression, response.data, 0, &result))
                                {
                                    cJSON *rsp_json = cJSON_CreateObject();
                                    if (rsp_json == NULL) 
                                    {
                                        ESP_LOGI(TAG, "Failed to create cJSON object");
                                        break;
                                    }
                                    
                                    for (size_t j = 0; j < response.length; ++j) 
                                    {
                                        sprintf(hex_rsponse + (j * 2), "%02X", response.data[j]);
                                    }
                                    hex_rsponse[response.length * 2] = '\0'; 

                                    // Add the name and result to the JSON object
                                    cJSON_AddNumberToObject(rsp_json, pid_req[i].name, result);

                                    cJSON_AddStringToObject( rsp_json, "raw", hex_rsponse);
                                    
                                    // Convert the cJSON object to a string
                                    char *response_str = cJSON_PrintUnformatted(rsp_json);
                                    if (response_str == NULL) 
                                    {
                                        ESP_LOGI(TAG, "Failed to print cJSON object");
                                        cJSON_Delete(rsp_json); // Clean up cJSON object
                                        break;
                                    }

                                    ESP_LOGI(TAG, "Expression result, Name: %s: %lf", pid_req[i].name, result);
                                    if(strlen(pid_req[i].destination) != 0)
                                    {
                                        mqtt_publish(pid_req[i].destination, response_str, 0, 0, 0);
                                    }
                                    else
                                    {
                                        //if destination is empty send to default
                                        mqtt_publish(config_server_get_mqtt_rx_topic(), response_str, 0, 0, 0);
                                    }
                                    
                                    // Free the JSON string and cJSON object
                                    free(response_str);
                                    cJSON_Delete(rsp_json);
                                    vTaskDelay(pdMS_TO_TICKS(10));
                                }
                                else
                                {
                                    ESP_LOGE(TAG, "Failed Expression: %s", pid_req[i].expression);
                                }
                            }
                            else
                            {
                                ESP_LOGE(TAG, "Timeout waiting for response for: %s", pid_req[i].pid_command);
                                pid_no_response = 1;
                            }
                        }
                    }

                    if(pid_no_response)
                    {
                        autopid_state = DISCONNECT_NOTIFY;
                        ESP_LOGI(TAG, "State change --> DISCONNECT_NOTIFY");
                    }

                    break;
                }

            }
        }
        else
        {
            autopid_state = CONNECT_CHECK;
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    while (1)
    {
        ESP_LOGE(TAG, "autopid_task ended");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
}

static void autopid_load(char *config_str)
{
    cJSON *json = cJSON_Parse(config_str);
    if (json == NULL) 
    {
        ESP_LOGE(TAG, "Failed to parse config string");
        return;
    }

    cJSON *init = cJSON_GetObjectItem(json, "initialisation");
    if (cJSON_IsString(init) && (init->valuestring != NULL)) 
    {
        size_t len = strlen(init->valuestring) + 1; // +1 for the null terminator
        initialisation = (char*)malloc(len);
        if (initialisation == NULL) 
        {
            ESP_LOGE(TAG, "Failed to allocate memory for initialisation string");
            cJSON_Delete(json);
            return;
        }
        strncpy(initialisation, init->valuestring, len);
        //replace ';' with carriage return
        for (size_t i = 0; i < len; ++i) 
        {
            if (initialisation[i] == ';') 
            {
                initialisation[i] = '\r';
            }
        }
    } 
    else 
    {
        ESP_LOGE(TAG, "Invalid initialisation string in config");
        cJSON_Delete(json);
        return;
    }

    cJSON *pids = cJSON_GetObjectItem(json, "pids");
    if (!cJSON_IsArray(pids)) 
    {
        ESP_LOGE(TAG, "Invalid pids array in config");
        cJSON_Delete(json);
        return;
    }

    num_of_pids = cJSON_GetArraySize(pids);
    if(num_of_pids == 0)
    {
        return;
    }

    pid_req = (pid_req_t *)malloc(sizeof(pid_req_t) * num_of_pids);
    if (pid_req == NULL) 
    {
        ESP_LOGE(TAG, "Failed to allocate memory for pid_req");
        cJSON_Delete(json);
        return;
    }

    for (size_t i = 0; i < num_of_pids; ++i) 
    {
        cJSON *pid_item = cJSON_GetArrayItem(pids, i);
        if (!cJSON_IsObject(pid_item)) 
        {
            ESP_LOGE(TAG, "Invalid PID item in config");
            continue;
        }

        cJSON *name = cJSON_GetObjectItem(pid_item, "Name");
        cJSON *pid_init = cJSON_GetObjectItem(pid_item, "Init");
        cJSON *pid_command = cJSON_GetObjectItem(pid_item, "PID");
        cJSON *expression = cJSON_GetObjectItem(pid_item, "Expression");
        cJSON *period = cJSON_GetObjectItem(pid_item, "Period");
        cJSON *send_to = cJSON_GetObjectItem(pid_item, "Send_to");
        cJSON *type = cJSON_GetObjectItem(pid_item, "Type");

        strncpy(pid_req[i].name, name->valuestring, sizeof(pid_req[i].name) - 1);
        strncpy(pid_req[i].pid_init, pid_init->valuestring, sizeof(pid_req[i].pid_init) - 1);
        strncpy(pid_req[i].pid_command, pid_command->valuestring, sizeof(pid_req[i].pid_command) - 1);
        strncpy(pid_req[i].expression, expression->valuestring, sizeof(pid_req[i].expression) - 1);
        strncpy(pid_req[i].destination, send_to->valuestring, sizeof(pid_req[i].destination) - 1);
        pid_req[i].period = (uint32_t)strtoul(period->valuestring, NULL, 10);
        pid_req[i].type = (strcmp(type->valuestring, "MQTT_Topic") == 0) ? 0 : 1;  // Example: 0 for MQTT, 1 for file
        pid_req[i].timer = 0; 
    }

    cJSON_Delete(json);
}

void autopid_init(char *config_str)
{
    autopid_load(config_str);
    
    xTaskCreate(autopid_task, "autopid_task", 1024 * 5, (void *)AF_INET, 5, NULL);
}
