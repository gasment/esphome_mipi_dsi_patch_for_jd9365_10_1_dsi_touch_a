# esphome_mipi_dsi_patch_for_jd9365_10_1_dsi_touch_a
A ESPHome MIPI-DSI Component Patch For WAVESHARE-10.1-DSI-TOUCH-A
* 全部代码修改由Chat-AI提供
* 基于ESPHome 2026.3.0 修改，用于修复特定型号MIPI-DSI屏幕的启动和初始化问题。
* 仅针对ESP32-P4连接屏幕型号：WAVESHARE-10.1-DSI-TOUCH-A / jd9365_10_1_dsi_touch_a（https://www.waveshare.net/wiki/10.1-DSI-TOUCH-A）
其他型号不要使用。
* 修复的问题：ESPhome官方MIPI-DSI组件对于jd9365_10_1_dsi_touch_a的初始化未有配置屏幕PWR使能部分，在mipi_dis组件Setup阶段屏幕未开启导致通讯超时触发看门狗，进而循环重启
* 修复的方法：参考微雪官方BSP驱动`https://components.espressif.com/components/waveshare/esp32_p4_nano/versions/1.2.0/readme`，在mipi_dsi组件Setup前注入特定i2c操作，拉起屏幕完成通讯（没有使用esphome on_boot配置，因为此方法执行时机并不稳定）
  ```
    if (this->power_i2c_bus_ != nullptr) {
      uint8_t d1[2] = {0x95, 0x11};
      uint8_t d2[2] = {0x95, 0x17};
      uint8_t d3[2] = {0x96, 0x00};
  
      this->power_i2c_bus_->write(0x45, d1, 2);
      this->power_i2c_bus_->write(0x45, d2, 2);
      this->power_i2c_bus_->write(0x45, d3, 2);
  
      delay(100);
  
      uint8_t d4[2] = {0x96, 0xFF};
      this->power_i2c_bus_->write(0x45, d4, 2);
  
      ESP_LOGI(TAG, "Screen I2C Power Init Done!");
  
      delay(300);
  } else {
    ESP_LOGW(TAG, "Screen I2C Power bus not Funnd!");
  }
  ```
* esphome yaml使用示例：（只引入了一个新配置power_i2c，其余用官方用法）
    ```
    display:
      - platform: mipi_dsi
        id: main_display
        model: WAVESHARE-10.1-DSI-TOUCH-A
        power_i2c: bus_a   #使用屏幕配套的i2c总线id
        rotation: 270
        hsync_pulse_width: 20
        hsync_back_porch: 20
        hsync_front_porch: 40
        vsync_pulse_width: 4
        vsync_back_porch: 10
        vsync_front_porch: 30
        update_interval: never
        auto_clear_enabled: false

    external_components:
      - source:
          type: git
          url: https://github.com/gasment/esphome_mipi_dsi_patch_for_jd9365_10_1_dsi_touch_a
          ref: main
        components: [ mipi_dsi ]
        refresh: always
    i2c:
      sda: GPIO7
      scl: GPIO8
      scan: true         
      id: bus_a
      frequency: 400kHz
      timeout: 10ms
    esp32:
      framework:
        sdkconfig_options:
          CONFIG_ESP_TASK_WDT_TIMEOUT_S: "30"  #可选，增大看门狗超时时间，防止mipi setup延迟
    ```
* 效果检查：（需要断电启动才能检查效果，在屏幕开启时reset无法验证Power Init效果）
  ```
  [C][display.mipi_dsi:025]: Running Setup - with I2C Power Init
  [I][display.mipi_dsi:042]: Screen I2C Power Init Done!
  [C][display.mipi_dsi:178]: MIPI DSI setup complete
  [C][component:252]: Setup display took 3117ms
  [D][gt911.touchscreen:062]: Switches ADDR: 0x5D DATA: 0x35
  ```
* 额外：
  * 修改为双缓冲（Double Framebuffer）和 Cache Sync屏幕刷新机制，规避一些animation组件导致的全屏刷脏与全屏闪烁问题
