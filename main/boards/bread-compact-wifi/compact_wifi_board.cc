#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>

#define TAG "CompactWifiBoard"

// ═══ SD Card Config (SDMMC mode, confirmed pinout) ═══
#define SD_MMC_CLK   GPIO_NUM_17
#define SD_MMC_CMD   GPIO_NUM_18
#define SD_MMC_D0    GPIO_NUM_21
#define SD_MMC_D3    GPIO_NUM_13  // Also serves as CS in 1-bit mode

// ═══ Battery ADC Config (confirmed via GPIO probe: GPIO 1 = ADC1_CH0) ═══
#define BATTERY_ADC_CHANNEL  ADC_CHANNEL_0  // GPIO 1
#define BATTERY_ADC_ATTEN    ADC_ATTEN_DB_12

// Mount SD card via SDMMC 1-bit mode
static bool MountSdCard() {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_MMC_CLK;
    slot_config.cmd = SD_MMC_CMD;
    slot_config.d0 = SD_MMC_D0;
    slot_config.d3 = SD_MMC_D3;
    slot_config.width = 1;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t* card = nullptr;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret == ESP_OK) {
        // newlib-nano 不支持 %llu，强制转 unsigned long 输出（容量 MB 数远小于 4G）
        unsigned long size_mb = (unsigned long)(((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
        ESP_LOGI(TAG, "SD card mounted (SDMMC)! Name: %s, Size: %lu MB",
                 card->cid.name, size_mb);
        return true;
    } else {
        ESP_LOGE(TAG, "SD card mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        return false;
    }
}

class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    bool sd_mounted_ = false;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;

    void InitBatteryAdc() {
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = ADC_UNIT_1,
            .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        if (adc_oneshot_new_unit(&init_cfg, &adc_handle_) != ESP_OK) {
            ESP_LOGE(TAG, "Battery ADC init failed");
            return;
        }
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = BATTERY_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_oneshot_config_channel(adc_handle_, BATTERY_ADC_CHANNEL, &chan_cfg);
        ESP_LOGI(TAG, "Battery ADC initialized (GPIO 1, CH0)");
    }

    void InitializeCodecI2c() {
        i2c_master_bus_config_t bus_cfg = {
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &codec_i2c_bus_));
        ESP_LOGI(TAG, "Codec I2C bus initialized (SDA=%d SCL=%d)",
                 (int)AUDIO_CODEC_I2C_SDA_PIN, (int)AUDIO_CODEC_I2C_SCL_PIN);
    }

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        // 小鹿豆专用：短按切换录音/停录，长按 3 秒强制进配网（避免误触）
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();

#ifdef CONFIG_XIAOLU_MODE
            // 启动早期、还没初始化 protocol 时，短按也允许进配网（兼容旧行为）
            if (state == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // 录音中 → 停录；待机/空闲 → 开录
            auto* proto = app.GetProtocol();
            if (proto && proto->IsAudioChannelOpened()) {
                DeviceCommand cmd{0, "stop_recording", "{}"};
                app.HandleDeviceCommand(cmd);
            } else {
                DeviceCommand cmd{0, "start_recording", "{}"};
                app.HandleDeviceCommand(cmd);
            }
#else
            if (state == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
#endif
        });

        // 长按 3 秒进配网（仅小鹿豆模式启用，避免对其它板的兼容影响）
#ifdef CONFIG_XIAOLU_MODE
        boot_button_.OnLongPress([this]() {
            ESP_LOGW(TAG, "BOOT long press: forcing WiFi config mode");
            EnterWifiConfigMode();
        });
#endif
    }

public:
    CompactWifiBoard() :
#ifdef CONFIG_XIAOLU_MODE
        // 长按 3000ms 触发 OnLongPress；短按沿用默认（短按时间 0 = 库内默认）
        boot_button_(BOOT_BUTTON_GPIO, false, 3000, 0) {
#else
        boot_button_(BOOT_BUTTON_GPIO) {
#endif
#ifdef CONFIG_XIAOLU_MODE
        // 小鹿豆模式：纯录音设备，跳过 OLED 初始化
        ESP_LOGI(TAG, "XiaoLu mode: skip OLED init, use NoDisplay");
        display_ = new NoDisplay();
        // 初始化 codec I2C 总线（ES8311 配置寄存器走这里）
        InitializeCodecI2c();
        // Mount SD card
        sd_mounted_ = MountSdCard();
        // Init battery ADC
        InitBatteryAdc();
#else
        // 非小鹿豆模式：保留原 OLED 显示路径，但目前这块板没有 OLED，
        // 所以非 XiaoLu 模式下也直接走 NoDisplay + ES8311
        display_ = new NoDisplay();
        InitializeCodecI2c();
#endif
        InitializeButtons();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        if (!adc_handle_) return false;
        
        int raw = 0;
        if (adc_oneshot_read(adc_handle_, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) return false;
        
        // GPIO 1 reads battery voltage through 1:1 voltage divider
        // ADC 12dB attenuation: 0-3.3V range, 12-bit (0-4095)
        // Battery voltage = ADC voltage * 2 (due to divider)
        float adc_voltage = (float)raw / 4095.0f * 3.3f;
        float battery_voltage = adc_voltage * 2.0f;
        
        // Convert voltage to percentage (3.0V = 0%, 4.2V = 100%)
        int percent = (int)((battery_voltage - 3.0f) / (4.2f - 3.0f) * 100.0f);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        
        level = percent;
        charging = false;     // TODO: detect charging pin if available
        discharging = true;
        return true;
    }
};

DECLARE_BOARD(CompactWifiBoard);
