#include "http_wall.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "cJSON.h"
#include <string>
#define MAX_PATH_LEN 64
static const char *TAG = "AccessTokenFetcher";

// 全局API数据结构
typedef struct {
    char *buffer;          // 用于存储HTTP响应的缓冲区
    int buffer_len;        // 缓冲区长度
    const char *url;       // 请求URL
    AccessTokenResult *result; // 结果指针
} ApiData;

static ApiData api_data = {
    .buffer = NULL,
    .buffer_len = 0,
    .url = NULL,
    .result = NULL
};

// HTTP事件处理函数
static esp_err_t http_auth_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            if (api_data.result) {
                api_data.result->error_msg = "HTTP_EVENT_ERROR";
            }
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (api_data.buffer == NULL) {
                    int content_length = esp_http_client_get_content_length(evt->client);
                    if (content_length <= 0) {
                        content_length = 4096; // 默认缓冲区大小
                    }
                    
                    api_data.buffer = (char *)malloc(content_length + 1);
                    if (api_data.buffer == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate memory for response buffer");
                        if (api_data.result) {
                            api_data.result->error_msg = "malloc failed";
                        }
                        return ESP_FAIL;
                    }
                    api_data.buffer_len = 0;
                }
                
                // 检查缓冲区是否足够，不足则扩容
                if (api_data.buffer_len + evt->data_len > esp_http_client_get_content_length(evt->client)) {
                    char* new_buf = (char*)realloc(api_data.buffer, api_data.buffer_len + evt->data_len + 1024);
                    if (new_buf == NULL) {
                        ESP_LOGE(TAG, "Buffer realloc failed");
                        free(api_data.buffer);
                        api_data.buffer = NULL;
                        api_data.buffer_len = 0;
                        if (api_data.result) {
                            api_data.result->error_msg = "realloc failed";
                        }
                        return ESP_FAIL;
                    }
                    api_data.buffer = new_buf;
                }
                
                // 复制数据到缓冲区
                memcpy(api_data.buffer + api_data.buffer_len, evt->data, evt->data_len);
                api_data.buffer_len += evt->data_len;
            } else {
                // 处理分块传输
                if (api_data.buffer == NULL) {
                    api_data.buffer = (char *)malloc(4096);
                    if (api_data.buffer == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate memory for chunked buffer");
                        return ESP_FAIL;
                    }
                    api_data.buffer_len = 0;
                }
                
                // 检查并扩容缓冲区
                if (api_data.buffer_len + evt->data_len >= 4096) {
                    char* new_buf = (char*)realloc(api_data.buffer, api_data.buffer_len + evt->data_len + 1024);
                    if (new_buf == NULL) {
                        ESP_LOGE(TAG, "Chunked buffer realloc failed");
                        free(api_data.buffer);
                        api_data.buffer = NULL;
                        api_data.buffer_len = 0;
                        return ESP_FAIL;
                    }
                    api_data.buffer = new_buf;
                }
                
                memcpy(api_data.buffer + api_data.buffer_len, evt->data, evt->data_len);
                api_data.buffer_len += evt->data_len;
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH, total received: %d bytes", api_data.buffer_len);
            
            // 解析JSON响应
            if (api_data.buffer && api_data.buffer_len > 0 && api_data.result) {
                api_data.buffer[api_data.buffer_len] = '\0';
                ESP_LOGI(TAG, "Response: %s", api_data.buffer);
                
                // cJSON *root = cJSON_Parse(api_data.buffer);
                // if (root) {
                //     // 解析基本字段
                //     cJSON *success = cJSON_GetObjectItem(root, "success");
                //     cJSON *code = cJSON_GetObjectItem(root, "code");
                //     cJSON *message = cJSON_GetObjectItem(root, "message");
                //     cJSON *data = cJSON_GetObjectItem(root, "data");
                    
                //     if (success && cJSON_IsBool(success)) {
                //         api_data.result->success = success->valueint;
                //     }
                //     if (code && cJSON_IsNumber(code)) {
                //         api_data.result->code = code->valueint;
                //     }
                //     if (message && cJSON_IsString(message) && message->valuestring) {
                //         api_data.result->message = message->valuestring;
                //     }
                    
                //     // 解析data字段
                //     if (data && cJSON_IsObject(data)) {
                //         cJSON *accessToken = cJSON_GetObjectItem(data, "accessToken");
                //         cJSON *tokenValidity = cJSON_GetObjectItem(data, "tokenValidity");
                        
                //         if (accessToken && cJSON_IsString(accessToken) && accessToken->valuestring) {
                //             api_data.result->accessToken = accessToken->valuestring;
                //         }
                //         if (tokenValidity && cJSON_IsNumber(tokenValidity)) {
                //             api_data.result->tokenValidity = tokenValidity->valueint;
                //         }
                //     }
                //     cJSON_Delete(root);
                // } else {
                //     ESP_LOGE(TAG, "Failed to parse JSON response");
                //     api_data.result->error_msg = "JSON parse failed";
                // }
                
                free(api_data.buffer);
                api_data.buffer = NULL;
                api_data.buffer_len = 0;
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            // 释放缓冲区
            if (api_data.buffer) {
                free(api_data.buffer);
            }
            api_data.buffer = NULL;
            api_data.buffer_len = 0;
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

/**
 * 获取accessToken
 * @param base_url 天枢平台基础URL（例如：https://api.tianshu.com）
 * @param appKey 应用Key
 * @param appSecret 应用密钥
 * @return AccessTokenResult 包含获取结果的结构体
 */
AccessTokenResult get_access_token(const std::string& base_url, 
                                  const std::string& appKey, 
                                  const std::string& appSecret) {
    AccessTokenResult res = {
        .success = false,
        .code = -1,
        .message = "",
        .accessToken = "",
        .tokenValidity = 0,
        .error_msg = "initial state"
    };
    
    // 参数校验
    if (base_url.empty() || appKey.empty() || appSecret.empty()) {
        res.error_msg = "invalid parameters: base_url, appKey and appSecret are required";
        return res;
    }
    
    // 构建完整请求URL
    std::string api_url = base_url;
    // std::string api_url = base_url + "/openapi/license/app/v1/getAccessToken";
    // ESP_LOGI(TAG, "Request URL: %s", api_url.c_str());
    
    // 构建JSON请求体
    cJSON *post_data = cJSON_CreateObject();
    if (!post_data) {
        res.error_msg = "failed to create JSON object";
        return res;
    }
    
    cJSON_AddStringToObject(post_data, "appKey", appKey.c_str());
    cJSON_AddStringToObject(post_data, "appSecret", appSecret.c_str());
    
    char *post_data_str = cJSON_PrintUnformatted(post_data);
    if (!post_data_str) {
        res.error_msg = "failed to convert JSON to string";
        cJSON_Delete(post_data);
        return res;
    }
    ESP_LOGI(TAG, "POST data: %s", post_data_str);
    
    // 初始化全局变量
    api_data.buffer = NULL;
    api_data.buffer_len = 0;
    api_data.url = api_url.c_str();
    api_data.result = &res;
    
    // 配置HTTP客户端
    esp_http_client_config_t config = {
        .url = api_url.c_str(),
        .method = HTTP_METHOD_POST,
        .event_handler = http_auth_event_handler,
        .buffer_size = 4 * 1024
    };
    
    // 初始化HTTP客户端
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        res.error_msg = "http client init failed";
        cJSON_Delete(post_data);
        free(post_data_str);
        return res;
    }
    
    // 设置HTTP头部
    esp_http_client_set_header(client, "Content-Type", "application/json");
    // esp_http_client_set_header(client, "User-Agent", "ESP32 HTTP Client");
    // esp_http_client_set_header(client, "Connection", "close");
    
    // 设置POST请求体
    esp_http_client_set_post_field(client, post_data_str, strlen(post_data_str));
    
    // 执行HTTP请求
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP POST Status = %d", status_code);
        
        if (status_code != 200) {
            res.error_msg = "http status error: " + std::to_string(status_code);
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        res.error_msg = "http request failed: " + std::string(esp_err_to_name(err));
    }
    
    // 清理资源
    esp_http_client_cleanup(client);
    cJSON_Delete(post_data);
    free(post_data_str);
    
    // 确保全局变量重置
    if (api_data.buffer) {
        free(api_data.buffer);
    }
    api_data.buffer = NULL;
    api_data.buffer_len = 0;
    api_data.url = NULL;
    api_data.result = NULL;
    
    return res;
}