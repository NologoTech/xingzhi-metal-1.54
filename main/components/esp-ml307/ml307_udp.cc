#include "ml307_udp.h"

#include <esp_log.h>

#define TAG "Ml307Udp"


Ml307Udp::Ml307Udp(Ml307AtModem& modem, int udp_id) : modem_(modem), udp_id_(udp_id) {
    event_group_handle_ = xEventGroupCreate();

    command_callback_it_ = modem_.RegisterCommandResponseCallback([this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
        if (command == "MIPOPEN") {
            if (arguments.size() >= 1 && arguments[0].type == AtArgumentValue::Type::Int &&
                arguments[0].int_value == udp_id_) {
                last_mipopen_err_ = arguments.size() > 1 ? arguments[1].int_value : 0;
                // ML307 成功=0；美格/L716 成功=1；800/550+ 为失败
                if (last_mipopen_err_ <= 1) {
                    connected_ = true;
                    xEventGroupClearBits(event_group_handle_, ML307_UDP_DISCONNECTED | ML307_UDP_ERROR);
                    xEventGroupSetBits(event_group_handle_, ML307_UDP_CONNECTED);
                } else {
                    connected_ = false;
                    xEventGroupSetBits(event_group_handle_, ML307_UDP_ERROR);
                    ESP_LOGE(TAG, "UDP socket %d MIPOPEN error=%d", udp_id_, last_mipopen_err_);
                }
            }
        } else if (command == "MIPCLOSE" && arguments.size() == 1) {
            if (arguments[0].int_value == udp_id_) {
                connected_ = false;
                xEventGroupSetBits(event_group_handle_, ML307_UDP_DISCONNECTED);
            }
        } else if (command == "MIPSEND" && arguments.size() == 2) {
            if (arguments[0].int_value == udp_id_) {
                xEventGroupSetBits(event_group_handle_, ML307_UDP_SEND_COMPLETE);
            }
        } else if (command == "MIPURC" && arguments.size() == 4) {
            if (arguments[1].int_value == udp_id_) {
                if (arguments[0].string_value == "rudp") {
                    if (message_callback_) {
                        message_callback_(modem_.DecodeHex(arguments[3].string_value));
                    }
                } else if (arguments[0].string_value == "disconn") {
                    connected_ = false;
                    xEventGroupSetBits(event_group_handle_, ML307_UDP_DISCONNECTED);
                } else {
                    ESP_LOGE(TAG, "Unknown MIPURC command: %s", arguments[0].string_value.c_str());
                }
            }
        } else if (command == "MIPSTATE") {
            if (arguments.size() >= 1 && arguments[0].int_value == udp_id_) {
                if (arguments.size() >= 5 && arguments[4].string_value == "INITIAL") {
                    connected_ = false;
                } else if (arguments.size() >= 5) {
                    connected_ = true;
                }
                xEventGroupSetBits(event_group_handle_, ML307_UDP_INITIALIZED);
            }
        } else if (command == "MDNSGIP" || command == "CDNSGIP" || command == "MDNSCFG") {
            resolved_ip_.clear();
            for (const auto& arg : arguments) {
                if (IsIpv4(arg.string_value)) {
                    resolved_ip_ = arg.string_value;
                    break;
                }
            }
            xEventGroupSetBits(event_group_handle_, ML307_UDP_DNS_DONE);
        } else if (command == "FIFO_OVERFLOW") {
            xEventGroupSetBits(event_group_handle_, ML307_UDP_ERROR);
            Disconnect();
        }
    });
}

Ml307Udp::~Ml307Udp() {
    Disconnect();
    modem_.UnregisterCommandResponseCallback(command_callback_it_);
}

bool Ml307Udp::IsIpv4(const std::string& value) {
    if (value.empty() || value.find('.') == std::string::npos) {
        return false;
    }
    int dots = 0;
    for (char c : value) {
        if (c == '.') {
            dots++;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return dots == 3;
}

bool Ml307Udp::ResolveHost(const std::string& host, std::string& ip) {
    if (IsIpv4(host)) {
        ip = host;
        return true;
    }

    resolved_ip_.clear();
    xEventGroupClearBits(event_group_handle_, ML307_UDP_DNS_DONE);
    std::string command = "AT+MDNSGIP=\"" + host + "\"";
    if (!modem_.Command(command, UDP_DNS_TIMEOUT_MS)) {
        command = "AT+CDNSGIP=\"" + host + "\"";
        if (!modem_.Command(command, UDP_DNS_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "DNS command failed for %s", host.c_str());
            return false;
        }
    }

    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_UDP_DNS_DONE, pdTRUE, pdFALSE,
                                    pdMS_TO_TICKS(UDP_DNS_TIMEOUT_MS));
    if (!(bits & ML307_UDP_DNS_DONE) || resolved_ip_.empty()) {
        ESP_LOGE(TAG, "DNS URC timeout or no IPv4 for %s", host.c_str());
        return false;
    }
    ip = resolved_ip_;
    return true;
}

bool Ml307Udp::TryOpen(const std::string& cmd, int id) {
    udp_id_ = id;
    last_mipopen_err_ = -1;
    connected_ = false;
    xEventGroupClearBits(event_group_handle_, ML307_UDP_CONNECTED | ML307_UDP_ERROR);
    if (!modem_.Command(cmd, UDP_CONNECT_TIMEOUT_MS)) {
        return false;
    }
    xEventGroupWaitBits(event_group_handle_, ML307_UDP_CONNECTED | ML307_UDP_ERROR,
                        pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    if (last_mipopen_err_ > 1) {
        modem_.Command("AT+MIPCLOSE=" + std::to_string(id), 2000);
        return false;
    }

    xEventGroupClearBits(event_group_handle_, ML307_UDP_INITIALIZED);
    modem_.Command("AT+MIPSTATE=" + std::to_string(id));
    xEventGroupWaitBits(event_group_handle_, ML307_UDP_INITIALIZED, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
    if (!connected_) {
        modem_.Command("AT+MIPCLOSE=" + std::to_string(id), 2000);
        return false;
    }
    return true;
}

bool Ml307Udp::Connect(const std::string& host, int port) {
    xEventGroupClearBits(event_group_handle_, ML307_UDP_CONNECTED | ML307_UDP_DISCONNECTED | ML307_UDP_ERROR);

    std::string command = "AT+MIPSTATE=" + std::to_string(udp_id_);
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "Failed to query UDP socket state");
        return false;
    }
    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_UDP_INITIALIZED, pdTRUE, pdFALSE, pdMS_TO_TICKS(UDP_CONNECT_TIMEOUT_MS));
    if (!(bits & ML307_UDP_INITIALIZED)) {
        ESP_LOGE(TAG, "Timeout while querying UDP socket state");
        return false;
    }

    if (connected_) {
        Disconnect();
    }

    std::string ip;
    if (!ResolveHost(host, ip)) {
        ip = host;
    }

    const int ids[] = {1, 2};
    for (int id : ids) {
        udp_id_ = id;
        modem_.Command("AT+MIPCFG=\"encoding\"," + std::to_string(id) + ",1,1");
        modem_.Command("AT+MIPCFG=\"ssl\"," + std::to_string(id) + ",0,0");

        std::string p = std::to_string(port);
        std::string i = std::to_string(id);
        std::string cmds[] = {
            "AT+MIPOPEN=" + i + ",\"UDP\",\"" + ip + "\"," + p,
            "AT+MIPOPEN=" + i + ",\"UDP\",\"" + ip + "\"," + p + ",60",
            "AT+MIPOPEN=" + i + ",\"UDP\",\"" + ip + "\"," + p + ",60,0",
            "AT+MIPOPEN=" + i + ",\"UDP\",\"" + ip + "\"," + p + ",60,0,0",
            "AT+MIPOPEN=" + i + ",0,\"" + ip + "\"," + p + ",1",
            "AT+MIPOPEN=" + i + ",0,\"" + ip + "\"," + p + ",1,0",
        };
        for (const auto& cmd : cmds) {
            if (TryOpen(cmd, id)) {
                return true;
            }
        }
    }

    ESP_LOGE(TAG, "Failed to connect UDP %s:%d", host.c_str(), port);
    return false;
}


void Ml307Udp::Disconnect() {
    if (!connected_) {
        return;
    }
    connected_ = false;
    modem_.Command("AT+MIPCLOSE=" + std::to_string(udp_id_));
}

int Ml307Udp::Send(const std::string& data) {
    const size_t MAX_PACKET_SIZE = 1460 / 2;

    if (!connected_) {
        ESP_LOGE(TAG, "未连接");
        return -1;
    }

    if (data.size() > MAX_PACKET_SIZE) {
        ESP_LOGE(TAG, "数据块超过最大限制");
        return -1;
    }

    // 在循环外预先分配command
    std::string command = "AT+MIPSEND=" + std::to_string(udp_id_) + "," + std::to_string(data.size()) + ",";
    
    // 直接在command字符串上进行十六进制编码
    modem_.EncodeHexAppend(command, data.c_str(), data.size());
    
    if (!modem_.Command(command, 100)) {
        ESP_LOGE(TAG, "发送数据块失败");
        return -1;
    }
    return data.size();
}
