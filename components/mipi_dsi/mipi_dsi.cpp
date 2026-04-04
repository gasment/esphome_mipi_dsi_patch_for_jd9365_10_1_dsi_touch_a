#ifdef USE_ESP32_VARIANT_ESP32P4
#include <utility>
#include "mipi_dsi.h"
#include "esphome/core/helpers.h"
#include "esphome/components/i2c/i2c.h"   //Patched
#include "esp_cache.h"                     // ← 新增：cache sync

namespace esphome {
namespace mipi_dsi {

// Maximum bytes to log for init commands (truncated if larger)
static constexpr size_t MIPI_DSI_MAX_CMD_LOG_BYTES = 64;

static bool notify_refresh_ready(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
  auto sem = static_cast<SemaphoreHandle_t>(user_ctx);
  BaseType_t need_yield = pdFALSE;
  xSemaphoreGiveFromISR(sem, &need_yield);
  return (need_yield == pdTRUE);
}

void MIPI_DSI::smark_failed(const LogString *message, esp_err_t err) {
  ESP_LOGE(TAG, "%s: %s", LOG_STR_ARG(message), esp_err_to_name(err));
  this->mark_failed(message);
}

void MIPI_DSI::setup() {
  ESP_LOGCONFIG(TAG, "Running Setup - with I2C Power Init");

///////////////////////Patch start
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
////////////////////////Patch end


  if (!this->enable_pins_.empty()) {
    for (auto *pin : this->enable_pins_) {
      pin->setup();
      pin->digital_write(true);
    }
    delay(10);
  }

  esp_lcd_dsi_bus_config_t bus_config = {
      .bus_id = 0,
      .num_data_lanes = this->lanes_,
      .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
      .lane_bit_rate_mbps = this->lane_bit_rate_,
  };
  auto err = esp_lcd_new_dsi_bus(&bus_config, &this->bus_handle_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("lcd_new_dsi_bus failed"), err);
    return;
  }
  esp_lcd_dbi_io_config_t dbi_config = {
      .virtual_channel = 0,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
  };
  err = esp_lcd_new_panel_io_dbi(this->bus_handle_, &dbi_config, &this->io_handle_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("new_panel_io_dbi failed"), err);
    return;
  }
  auto pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  if (this->color_depth_ == display::COLOR_BITNESS_888) {
    pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
  }
  esp_lcd_dpi_panel_config_t dpi_config = {.virtual_channel = 0,
                                           .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
                                           .dpi_clock_freq_mhz = this->pclk_frequency_,
                                           .pixel_format = pixel_format,
                                           // ========== 改动：双 framebuffer ==========
                                           .num_fbs = 2,
                                           // =========================================
                                           .video_timing =
                                               {
                                                   .h_size = this->width_,
                                                   .v_size = this->height_,
                                                   .hsync_pulse_width = this->hsync_pulse_width_,
                                                   .hsync_back_porch = this->hsync_back_porch_,
                                                   .hsync_front_porch = this->hsync_front_porch_,
                                                   .vsync_pulse_width = this->vsync_pulse_width_,
                                                   .vsync_back_porch = this->vsync_back_porch_,
                                                   .vsync_front_porch = this->vsync_front_porch_,
                                               },
                                           .flags = {
                                               .use_dma2d = true,
                                           }};
  err = esp_lcd_new_panel_dpi(this->bus_handle_, &dpi_config, &this->handle_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("esp_lcd_new_panel_dpi failed"), err);
    return;
  }

  // ========== 新增：打印双 FB 信息 ==========
  void *fb1 = nullptr, *fb2 = nullptr;
  esp_lcd_dpi_panel_get_frame_buffer(this->handle_, 2, &fb1, &fb2);
  ESP_LOGW(TAG, "DPI panel: num_fbs=2, fb1=%p fb2=%p", fb1, fb2);
  // ==========================================

  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
    this->reset_pin_->digital_write(true);
    delay(5);
    this->reset_pin_->digital_write(false);
    delay(5);
    this->reset_pin_->digital_write(true);
  } else {
    esp_lcd_panel_io_tx_param(this->io_handle_, SW_RESET_CMD, nullptr, 0);
  }
  auto when = millis() + 120;
  err = esp_lcd_panel_init(this->handle_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("esp_lcd_init failed"), err);
    return;
  }
  size_t index = 0;
  auto &vec = this->init_sequence_;
  while (index != vec.size()) {
    if (vec.size() - index < 2) {
      this->mark_failed(LOG_STR("Malformed init sequence"));
      return;
    }
    uint8_t cmd = vec[index++];
    uint8_t x = vec[index++];
    if (x == DELAY_FLAG) {
      ESP_LOGD(TAG, "Delay %dms", cmd);
      delay(cmd);
    } else {
      uint8_t num_args = x & 0x7F;
      if (vec.size() - index < num_args) {
        this->mark_failed(LOG_STR("Malformed init sequence"));
        return;
      }
      if (cmd == SLEEP_OUT) {
        int duration = when - millis();
        if (duration > 0) {
          delay(duration);
        }
      }
      const auto *ptr = vec.data() + index;
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
      char hex_buf[format_hex_pretty_size(MIPI_DSI_MAX_CMD_LOG_BYTES)];
#endif
      ESP_LOGVV(TAG, "Command %02X, length %d, byte(s) %s", cmd, num_args,
                format_hex_pretty_to(hex_buf, ptr, num_args, '.'));
      err = esp_lcd_panel_io_tx_param(this->io_handle_, cmd, ptr, num_args);
      if (err != ESP_OK) {
        this->smark_failed(LOG_STR("lcd_panel_io_tx_param failed"), err);
        return;
      }
      index += num_args;
      if (cmd == SLEEP_OUT)
        delay(10);
    }
  }
  this->io_lock_ = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {
      .on_color_trans_done = notify_refresh_ready,
  };

  err = (esp_lcd_dpi_panel_register_event_callbacks(this->handle_, &cbs, this->io_lock_));
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("Failed to register callbacks"), err);
    return;
  }

  ESP_LOGCONFIG(TAG, "MIPI DSI setup complete");
}

void MIPI_DSI::update() {
  if (this->auto_clear_enabled_) {
    this->clear();
  }
  if (this->show_test_card_) {
    this->test_card();
  } else if (this->page_ != nullptr) {
    this->page_->get_writer()(*this);
  } else if (this->writer_.has_value()) {
    (*this->writer_)(*this);
  } else {
    this->stop_poller();
  }
  if (this->buffer_ == nullptr || this->x_low_ > this->x_high_ || this->y_low_ > this->y_high_)
    return;
  ESP_LOGV(TAG, "x_low %d, y_low %d, x_high %d, y_high %d", this->x_low_, this->y_low_, this->x_high_, this->y_high_);
  int w = this->x_high_ - this->x_low_ + 1;
  int h = this->y_high_ - this->y_low_ + 1;
  this->write_to_display_(this->x_low_, this->y_low_, w, h, this->buffer_, this->x_low_, this->y_low_,
                          this->width_ - w - this->x_low_);
  this->x_low_ = this->width_;
  this->y_low_ = this->height_;
  this->x_high_ = 0;
  this->y_high_ = 0;
}

void MIPI_DSI::draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                              display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) {
  if (w <= 0 || h <= 0)
    return;
  if (bitness != this->color_depth_) {
    display::Display::draw_pixels_at(x_start, y_start, w, h, ptr, order, bitness, big_endian, x_offset, y_offset,
                                     x_pad);
    return;
  }
  this->write_to_display_(x_start, y_start, w, h, ptr, x_offset, y_offset, x_pad);
}

// ========== 改动：加入 cache sync ==========
void MIPI_DSI::write_to_display_(int x_start, int y_start, int w, int h, const uint8_t *ptr, int x_offset, int y_offset,
                                 int x_pad) {
  esp_err_t err = ESP_OK;
  auto bytes_per_pixel = 3 - this->color_depth_;
  auto stride = (x_offset + w + x_pad) * bytes_per_pixel;
  ptr += y_offset * stride + x_offset * bytes_per_pixel;

  if (x_offset == 0 && x_pad == 0) {
    // ---- Cache sync: CPU cache → PSRAM, 让 DMA2D 读到最新数据 ----
    size_t data_size = (size_t)w * h * bytes_per_pixel;
    uintptr_t aligned_start = (uintptr_t)ptr & ~63UL;
    uintptr_t aligned_end   = ((uintptr_t)ptr + data_size + 63UL) & ~63UL;
    esp_cache_msync((void *)aligned_start, aligned_end - aligned_start,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    // ---------------------------------------------------------------

    err = esp_lcd_panel_draw_bitmap(this->handle_, x_start, y_start, x_start + w, y_start + h, ptr);
    xSemaphoreTake(this->io_lock_, portMAX_DELAY);

  } else {
    // ---- Cache sync: 一次性刷整个源数据区域 ----
    size_t total_size = (size_t)stride * h;
    uintptr_t aligned_start = (uintptr_t)ptr & ~63UL;
    uintptr_t aligned_end   = ((uintptr_t)ptr + total_size + 63UL) & ~63UL;
    esp_cache_msync((void *)aligned_start, aligned_end - aligned_start,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    // ---------------------------------------------

    for (int y = 0; y != h; y++) {
      err = esp_lcd_panel_draw_bitmap(this->handle_, x_start, y + y_start, x_start + w, y + y_start + 1, ptr);
      if (err != ESP_OK)
        break;
      ptr += stride;
      xSemaphoreTake(this->io_lock_, portMAX_DELAY);
    }
  }
  if (err != ESP_OK)
    ESP_LOGE(TAG, "lcd_lcd_panel_draw_bitmap failed: %s", esp_err_to_name(err));
}
// ============================================

bool MIPI_DSI::check_buffer_() {
  if (this->is_failed())
    return false;
  if (this->buffer_ != nullptr)
    return true;
  auto bytes_per_pixel = 3 - this->color_depth_;
  RAMAllocator<uint8_t> allocator;
  this->buffer_ = allocator.allocate(this->height_ * this->width_ * bytes_per_pixel);
  if (this->buffer_ == nullptr) {
    this->mark_failed(LOG_STR("Could not allocate buffer for display!"));
    return false;
  }
  return true;
}

void MIPI_DSI::draw_pixel_at(int x, int y, Color color) {
  if (!this->get_clipping().inside(x, y))
    return;

  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_0_DEGREES:
      break;
    case display::DISPLAY_ROTATION_90_DEGREES:
      std::swap(x, y);
      x = this->width_ - x - 1;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      x = this->width_ - x - 1;
      y = this->height_ - y - 1;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      std::swap(x, y);
      y = this->height_ - y - 1;
      break;
  }
  if (x >= this->get_width_internal() || x < 0 || y >= this->get_height_internal() || y < 0) {
    return;
  }
  auto pixel = convert_big_endian(display::ColorUtil::color_to_565(color));
  if (!this->check_buffer_())
    return;
  size_t pos = (y * this->width_) + x;
  switch (this->color_depth_) {
    case display::COLOR_BITNESS_565: {
      auto *ptr_16 = reinterpret_cast<uint16_t *>(this->buffer_);
      uint8_t hi_byte = static_cast<uint8_t>(color.r & 0xF8) | (color.g >> 5);
      uint8_t lo_byte = static_cast<uint8_t>((color.g & 0x1C) << 3) | (color.b >> 3);
      uint16_t new_color = lo_byte | (hi_byte << 8);
      if (ptr_16[pos] == new_color)
        return;
      ptr_16[pos] = new_color;
      break;
    }
    case display::COLOR_BITNESS_888:
      if (this->color_mode_ == display::COLOR_ORDER_BGR) {
        this->buffer_[pos * 3] = color.b;
        this->buffer_[pos * 3 + 1] = color.g;
        this->buffer_[pos * 3 + 2] = color.r;
      } else {
        this->buffer_[pos * 3] = color.r;
        this->buffer_[pos * 3 + 1] = color.g;
        this->buffer_[pos * 3 + 2] = color.b;
      }
      break;
    case display::COLOR_BITNESS_332:
      break;
  }
  if (x < this->x_low_)
    this->x_low_ = x;
  if (y < this->y_low_)
    this->y_low_ = y;
  if (x > this->x_high_)
    this->x_high_ = x;
  if (y > this->y_high_)
    this->y_high_ = y;
}

void MIPI_DSI::fill(Color color) {
  if (!this->check_buffer_())
    return;
  if (this->get_clipping().is_set()) {
    Display::fill(color);
    return;
  }
  switch (this->color_depth_) {
    case display::COLOR_BITNESS_565: {
      auto *ptr_16 = reinterpret_cast<uint16_t *>(this->buffer_);
      uint8_t hi_byte = static_cast<uint8_t>(color.r & 0xF8) | (color.g >> 5);
      uint8_t lo_byte = static_cast<uint8_t>((color.g & 0x1C) << 3) | (color.b >> 3);
      uint16_t new_color = lo_byte | (hi_byte << 8);
      std::fill_n(ptr_16, this->width_ * this->height_, new_color);
      break;
    }
    case display::COLOR_BITNESS_888:
      if (this->color_mode_ == display::COLOR_ORDER_BGR) {
        for (size_t i = 0; i != this->width_ * this->height_; i++) {
          this->buffer_[i * 3 + 0] = color.b;
          this->buffer_[i * 3 + 1] = color.g;
          this->buffer_[i * 3 + 2] = color.r;
        }
      } else {
        for (size_t i = 0; i != this->width_ * this->height_; i++) {
          this->buffer_[i * 3 + 0] = color.r;
          this->buffer_[i * 3 + 1] = color.g;
          this->buffer_[i * 3 + 2] = color.b;
        }
      }
    default:
      break;
  }
}

int MIPI_DSI::get_width() {
  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_90_DEGREES:
    case display::DISPLAY_ROTATION_270_DEGREES:
      return this->get_height_internal();
    case display::DISPLAY_ROTATION_0_DEGREES:
    case display::DISPLAY_ROTATION_180_DEGREES:
    default:
      return this->get_width_internal();
  }
}

int MIPI_DSI::get_height() {
  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_0_DEGREES:
    case display::DISPLAY_ROTATION_180_DEGREES:
      return this->get_height_internal();
    case display::DISPLAY_ROTATION_90_DEGREES:
    case display::DISPLAY_ROTATION_270_DEGREES:
    default:
      return this->get_width_internal();
  }
}

static const uint8_t PIXEL_MODES[] = {0, 16, 18, 24};

void MIPI_DSI::dump_config() {
  ESP_LOGCONFIG(TAG,
                "MIPI_DSI RGB LCD"
                "\n  Model: %s"
                "\n  Width: %u"
                "\n  Height: %u"
                "\n  Mirror X: %s"
                "\n  Mirror Y: %s"
                "\n  Swap X/Y: %s"
                "\n  Rotation: %d degrees"
                "\n  DSI Lanes: %u"
                "\n  Lane Bit Rate: %.0fMbps"
                "\n  HSync Pulse Width: %u"
                "\n  HSync Back Porch: %u"
                "\n  HSync Front Porch: %u"
                "\n  VSync Pulse Width: %u"
                "\n  VSync Back Porch: %u"
                "\n  VSync Front Porch: %u"
                "\n  Buffer Color Depth: %d bit"
                "\n  Display Pixel Mode: %d bit"
                "\n  Color Order: %s"
                "\n  Invert Colors: %s"
                "\n  Pixel Clock: %.1fMHz",
                this->model_, this->width_, this->height_, YESNO(this->madctl_ & (MADCTL_XFLIP | MADCTL_MX)),
                YESNO(this->madctl_ & (MADCTL_YFLIP | MADCTL_MY)), YESNO(this->madctl_ & MADCTL_MV), this->rotation_,
                this->lanes_, this->lane_bit_rate_, this->hsync_pulse_width_, this->hsync_back_porch_,
                this->hsync_front_porch_, this->vsync_pulse_width_, this->vsync_back_porch_, this->vsync_front_porch_,
                (3 - this->color_depth_) * 8, this->pixel_mode_, this->madctl_ & MADCTL_BGR ? "BGR" : "RGB",
                YESNO(this->invert_colors_), this->pclk_frequency_);
  LOG_PIN("  Reset Pin ", this->reset_pin_);
}
}  // namespace mipi_dsi
}  // namespace esphome
#endif  // USE_ESP32_VARIANT_ESP32P4
