#include "dual_network_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"

#include "system_reset.h"

#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>

#include "power_manager.h"
#include "power_save_timer.h"

#include "led/single_led.h"
#include "assets/lang_config.h"
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <wifi_station.h>
#include "sc7a20h.h"

#define TAG "XINGZHI_METAL_1_54"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class Cst816x : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };

    Cst816x(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        ESP_LOGI(TAG, "Get chip ID");
        uint8_t chip_id = ReadReg(0xA7);//0xAA
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Cst816x() {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint() {
        TouchPoint_t tp1,tp2;
        ReadRegs(0x02, read_buffer_, 6);
        tp1.num = read_buffer_[0] & 0x0F;
        tp1.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp1.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
        for (int i = 0; i < 6; i++) {
        read_buffer_[i] = 0;}
        vTaskDelay(pdMS_TO_TICKS(10));
        ReadRegs(0x02, read_buffer_, 6);
        tp2.num = read_buffer_[0] & 0x0F;
        tp2.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp2.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
        if ((tp1.num==tp2.num) && (tp1.x==tp2.x))
        {
            tp_.num = tp2.num;
            tp_.x = tp2.x;
            tp_.y = tp2.y;
        }else{
            tp_.num = 0;
            tp_.x = 0;
            tp_.y = 0;
        }
    }

    const TouchPoint_t &GetTouchPoint() {
        return tp_;
    }

private:
    uint8_t *read_buffer_ = nullptr;
    TouchPoint_t tp_;
};


class XINGZHI_METAL_1_54 : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Cst816x *cst816d_;

    Button boot_button_;
    SpiLcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    int touch_press_num = 0;
    int64_t touchpress_us = 0;
    bool touch_released = true;
    bool touch_40_600_detected = false;  // 新增：记录是否检测到(40,600)触摸点
    bool single_click_pending = false;  // 新增：标记单次点击待处理
    esp_err_t err;
    bool is_device_found = false;
    Sc7a20hSensor* sc7a20h_sensor_;
    void InitializePowerManager() {
        power_manager_ = new PowerManager(POWER_USB_IN);//USB是否插入
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            display_->SetChatMessage("system", "");
            display_->SetEmotion("sleepy");
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            display_->SetChatMessage("system", "");
            display_->SetEmotion("neutral");
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            rtc_gpio_set_level(NETWORK_MODULE_POWER_IN, 0);
            // 启用保持功能，确保睡眠期间电平不变
            rtc_gpio_hold_en(NETWORK_MODULE_POWER_IN);
            esp_lcd_panel_disp_on_off(panel_, false); //关闭显示
            // esp_deep_sleep_start();
            // gpio_set_level(DISPLAY_RES, 0);
            gpio_set_level(Power_Control, 0);
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        for (uint8_t addr = 1; addr < 127; addr++) {
            err = i2c_master_probe(i2c_bus_, addr, 100);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Device found at address 0x%02X", addr);
                if (addr == 0x15) {
                    is_device_found = true;
                }
            }
        }
    }
    void I2cDetect() {
    uint8_t address;
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
    for (int i = 0; i < 128; i += 16) {
        printf("%02x: ", i);
        for (int j = 0; j < 16; j++) {
            fflush(stdout);
            address = i + j;
            esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
            if (ret == ESP_OK) {
                printf("%02x ", address);
            } else if (ret == ESP_ERR_TIMEOUT) {
                printf("UU ");
            } else {
                printf("-- ");
            }
        }
        printf("\r\n");
    }
    }

    void InitializeSC7A20HSensor() {
            // 初始化传感器
        sc7a20h_sensor_ = new Sc7a20hSensor(i2c_bus_);
        esp_err_t err = sc7a20h_sensor_->Initialize();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "初始化SC7A20H传感器失败 (err=0x%x)", err);
            return;
        }

        // 设置加速度数据回调
        sc7a20h_sensor_->SetAccelerationCallback([this](float x, float y, float z) {
            UpdateAccelerationDisplay(x, y, z);
        });

        // 启动周期性读取（每100ms一次）
        err = sc7a20h_sensor_->StartReading(1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "启动周期性读取失败 (err=0x%x)", err);
        }
    }
    void UpdateAccelerationDisplay(float x, float y, float z) {
        // if (custom_display_) {
            char buffer[50];
            sprintf(buffer, "X:%.2f Y:%.2f Z:%.2f", x, y, z);
            // custom_display_->SetAccelerationText(buffer);
        // }
    }
    void touchpad_daemon(void *param) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int count = 0;
        auto &board = (XINGZHI_METAL_1_54&)Board::GetInstance();
        auto touchpad = board.GetTouchpad();
        bool was_touched = false;
        int count_double = 0;
        // 长按检测相关变量
        bool is_touching_40_600 = false;      // 是否正在触摸40,600位置
        bool is_long_press_detected = false;  // 是否已检测到长按
        int64_t touch_start_time = 0;         // 触摸开始时间(微秒)
        const int64_t LONG_PRESS_THRESHOLD = 4000000;  // 5秒(微秒)
        const int64_t CLICK_THRESHOLD = 300000;       // 单击判断阈值(300毫秒)

        while (1) {
            touchpad->UpdateTouchPoint();
            // ESP_LOGI(TAG, "read_buffer_: %d %d %d",touchpad->GetTouchPoint().num,touchpad->GetTouchPoint().x,touchpad->GetTouchPoint().y);

            // 音量减小逻辑 (60,600)
            if ((touchpad->GetTouchPoint().x == 60) && (touchpad->GetTouchPoint().y == 600)) {
                ESP_LOGI(TAG, "Volume down: %d %d", touchpad->GetTouchPoint().x, touchpad->GetTouchPoint().y);
                if (!was_touched) {
                    was_touched = true;
                    auto codec = GetAudioCodec();
                    auto volume = codec->output_volume() - 10;
                    if (volume < 0) volume = 0;
                    codec->SetOutputVolume(volume);
                    GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
                    count = 0;
                } else {
                    count++;
                    if (count > 10) {
                        was_touched = false;
                        count = 0;
                    }
                }
                // resetTouchCounters();  // 重置双击相关状态
            }
            // 音量增大逻辑 (20,600)
            else if ((touchpad->GetTouchPoint().x == 20) && (touchpad->GetTouchPoint().y == 600)) {
                ESP_LOGI(TAG, "Volume up: %d %d", touchpad->GetTouchPoint().x, touchpad->GetTouchPoint().y);
                if (!was_touched) {
                    was_touched = true;
                    auto codec = GetAudioCodec();
                    auto volume = codec->output_volume() + 10;
                    if (volume > 100) volume = 100;
                    codec->SetOutputVolume(volume);
                    GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
                    count = 0;
                } else {
                    count++;
                    if (count > 10) {
                        was_touched = false;
                        count = 0;
                    }
                }
            }
            // 双击检测逻辑 (40,600)
            else if ((touchpad->GetTouchPoint().x == 40) && (touchpad->GetTouchPoint().y == 600)) {
                ESP_LOGI(TAG, "Touch detected at (40,600)");
                // 首次检测到触摸
                if (!is_touching_40_600) {
                    is_touching_40_600 = true;
                    struct timeval tv_now;
                    gettimeofday(&tv_now, NULL);
                    touch_start_time = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
                    is_long_press_detected = false;  // 重置长按标记
                } else {
                    // 已在触摸中，检查是否达到长按阈值
                    if (!is_long_press_detected) {
                        struct timeval tv_now;
                        gettimeofday(&tv_now, NULL);
                        int64_t now_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
                        
                        if (now_us - touch_start_time >= LONG_PRESS_THRESHOLD) {
                            // 长按操作示例
                            auto& app = Application::GetInstance();
                            // if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                                SwitchNetworkType();
                            // }
                            ESP_LOGI(TAG, "long press detected");
                            is_long_press_detected=true;
                        }
                    }
                }
                was_touched = true;
                count = 0;
            }
            // 触摸释放检测 (0,0)
            else if ((touchpad->GetTouchPoint().x == 0) && (touchpad->GetTouchPoint().y == 0)) {
                // 处理40,600位置的释放
                if (is_touching_40_600) {
                    struct timeval tv_now;
                    gettimeofday(&tv_now, NULL);
                    int64_t now_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
                    int64_t touch_duration = now_us - touch_start_time;
                    
                    // 如果未检测到长按且触摸时间短于阈值，则视为单击
                    if (!is_long_press_detected && touch_duration < LONG_PRESS_THRESHOLD && touch_duration > CLICK_THRESHOLD) {
                        power_save_timer_->WakeUp();
                        auto& app = Application::GetInstance();
                        if (GetNetworkType() == NetworkType::WIFI) {
                            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                                auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                                wifi_board.ResetWifiConfiguration();
                            }
                        }
                        app.ToggleChatState();
                        ESP_LOGI(TAG, "one click");
                    }
                    // 重置触摸状态
                    is_touching_40_600 = false;
                    is_long_press_detected = false;
                    ESP_LOGI(TAG, "触摸释放(40,600)");
                }
                // 处理其他触摸点的释放
                if (was_touched) {
                    was_touched = false;
                    count = 0;
                }
            }
            // 其他触摸点处理
            else {
                ESP_LOGI(TAG, "Other touch point: %d %d", touchpad->GetTouchPoint().x, touchpad->GetTouchPoint().y);
                // resetTouchCounters();
                is_touching_40_600 = false;
                is_long_press_detected = false;
                
                if (was_touched) {
                    was_touched = false;
                    count = 0;
                }
            }
                vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelete(NULL);
    }

    // 新增：重置触摸计数器的辅助函数
    void resetTouchCounters() {
        touch_40_600_detected = false;
        single_click_pending = false;
        if (touch_released) {
            touch_press_num = 0;
            touchpress_us = 0;
        } 
    }

    void InitCst816d() {
        if (is_device_found)
        {
            ESP_LOGI(TAG, "Init CST816x");
            cst816d_ = new Cst816x(i2c_bus_, 0x15);
            xTaskCreate([](void *param) {static_cast<XINGZHI_METAL_1_54*>(param)->touchpad_daemon(param);}, "tp", 4096, this, 1, NULL);
        }
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (GetNetworkType() == NetworkType::WIFI) {
                if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                    // cast to WifiBoard
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.ResetWifiConfiguration();
                }
            }
            app.ToggleChatState();
        });
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                SwitchNetworkType();
            }
        });
    }

    void InitializeSt7789Display() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io_));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));

        display_ = new SpiLcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, 
        {
            .text_font = &font_puhui_20_4,
            .icon_font = &font_awesome_20_4,
            .emoji_font = font_emoji_64_init(),
        });
    }

    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
        thing_manager.AddThing(iot::CreateThing("Battery"));
    }

    void InitializeGpio() {
        rtc_gpio_init(NETWORK_MODULE_POWER_IN);
        rtc_gpio_set_direction(NETWORK_MODULE_POWER_IN, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(NETWORK_MODULE_POWER_IN, 1); // 拉高4g供电引脚

        gpio_config_t zxc = {};
        zxc.intr_type = GPIO_INTR_DISABLE;
        zxc.mode = GPIO_MODE_INPUT;
        zxc.pin_bit_mask = (1ULL << NETWORK_ZXC);
        zxc.pull_down_en = GPIO_PULLDOWN_DISABLE; 
        zxc.pull_up_en = GPIO_PULLUP_DISABLE;     
        gpio_config(&zxc);
        gpio_set_level(NETWORK_ZXC, 0);
    }
public:
    XINGZHI_METAL_1_54() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, 4096),
        boot_button_(BOOT_BUTTON_GPIO) {
        // InitializeGpio();      
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeI2c();
        InitCst816d();//
        // InitializeSC7A20HSensor();//陀螺仪
        InitializeSpi();
        // InitializeButtons();
        InitializeSt7789Display();
        InitializeIot();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, 
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            NETWORK_MODULE_POWER_IN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_INPUT_REFERENCE);
            return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    Cst816x *GetTouchpad() {
        return cst816d_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        DualNetworkBoard::SetPowerSaveMode(enabled);
    }
};

DECLARE_BOARD(XINGZHI_METAL_1_54);
