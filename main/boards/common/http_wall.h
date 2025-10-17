#ifndef HTTP_WALL_H
#define HTTP_WALL_H

#include "esp_err.h"
#include <string.h>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string>

// 存储accessToken获取结果的数据结构
typedef struct {
    bool success;          // 请求是否成功
    int code;              // 状态码
    std::string message;   // 提示信息
    std::string accessToken; // 获取到的accessToken
    int tokenValidity;     // 有效期（秒）
    std::string error_msg; // 错误信息
} AccessTokenResult;

// int delete_files_by_basename(const char *target_dir, const char *target_basename);
AccessTokenResult get_access_token(const std::string& base_url, 
                                  const std::string& appKey, 
                                  const std::string& appSecret);
#endif // HTTP_WALL_H
