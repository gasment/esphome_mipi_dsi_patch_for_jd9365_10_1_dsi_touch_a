#ifdef USE_ESP32_VARIANT_ESP32P4
#include <utility>
#include <cstring>
#include "mipi_dsi.h"
#include "esphome/core/helpers.h"
#include "esphome/components/i2c/i2c.h"   //Patched
#include <climits>
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

/////////////////////////////////////////////////patch
void MIPI_DSI::reset_dirty_window_() {
  this->x_low_ = this->width_;
  this->y_low_ = this->height_;
  this->x_high_ = 0;
  this->y_high_ = 0;
}

void MIPI_DSI::reset_pending_dirty_() {
  this->pending_rect_count_ = 0;
}

void MIPI_DSI::reset_inflight_dirty_() {
  this->inflight_rect_count_ = 0;
}

bool MIPI_DSI::has_pending_dirty_() const {
  return this->pending_rect_count_ > 0;
}

bool MIPI_DSI::rects_touch_or_overlap_(const DirtyRect &a, const DirtyRect &b) {
  return !((static_cast<uint32_t>(a.x2) + 1U < b.x1) ||
           (static_cast<uint32_t>(b.x2) + 1U < a.x1) ||
           (static_cast<uint32_t>(a.y2) + 1U < b.y1) ||
           (static_cast<uint32_t>(b.y2) + 1U < a.y1));
}

MIPI_DSI::DirtyRect MIPI_DSI::union_rect_(const DirtyRect &a, const DirtyRect &b) {
  DirtyRect out;
  out.x1 = (a.x1 < b.x1) ? a.x1 : b.x1;
  out.y1 = (a.y1 < b.y1) ? a.y1 : b.y1;
  out.x2 = (a.x2 > b.x2) ? a.x2 : b.x2;
  out.y2 = (a.y2 > b.y2) ? a.y2 : b.y2;
  return out;
}

uint32_t MIPI_DSI::rect_area_(const DirtyRect &r) {
  return static_cast<uint32_t>(r.x2 - r.x1 + 1U) * static_cast<uint32_t>(r.y2 - r.y1 + 1U);
}

void MIPI_DSI::merge_rect_into_list_(DirtyRect *rects, uint8_t &count, const DirtyRect &input) {
  if (input.x1 > input.x2 || input.y1 > input.y2)
    return;

  DirtyRect merged = input;

  // 先把所有重叠/相邻 rect 吃进去
  bool merged_any = false;
  do {
    merged_any = false;
    for (uint8_t i = 0; i < count; i++) {
      if (!this->rects_touch_or_overlap_(rects[i], merged))
        continue;

      merged = this->union_rect_(rects[i], merged);

      for (uint8_t j = i + 1; j < count; j++) {
        rects[j - 1] = rects[j];
      }
      count--;
      merged_any = true;
      break;
    }
  } while (merged_any);

  if (count < MAX_DIRTY_RECTS) {
    rects[count++] = merged;
    return;
  }

  // 满了：选一个“扩张代价最小”的 rect 强制合并
  uint8_t best_index = 0;
  uint32_t best_cost = UINT32_MAX;

  for (uint8_t i = 0; i < count; i++) {
    DirtyRect u = this->union_rect_(rects[i], merged);
    uint32_t cost = this->rect_area_(u) - this->rect_area_(rects[i]);
    if (cost < best_cost) {
      best_cost = cost;
      best_index = i;
    }
  }

  rects[best_index] = this->union_rect_(rects[best_index], merged);

  // 强制合并后，可能又和其他 rect 接壤，再做一轮压缩
  bool collapsed = false;
  do {
    collapsed = false;
    for (uint8_t i = 0; i < count; i++) {
      for (uint8_t j = i + 1; j < count; j++) {
        if (!this->rects_touch_or_overlap_(rects[i], rects[j]))
          continue;

        rects[i] = this->union_rect_(rects[i], rects[j]);
        for (uint8_t k = j + 1; k < count; k++) {
          rects[k - 1] = rects[k];
        }
        count--;
        collapsed = true;
        break;
      }
      if (collapsed)
        break;
    }
  } while (collapsed);
}

void MIPI_DSI::merge_pending_dirty_(int x1, int y1, int x2, int y2) {
  if (x1 > x2 || y1 > y2)
    return;

  if (x1 < 0)
    x1 = 0;
  if (y1 < 0)
    y1 = 0;
  if (x2 >= this->width_)
    x2 = this->width_ - 1;
  if (y2 >= this->height_)
    y2 = this->height_ - 1;

  if (x1 > x2 || y1 > y2)
    return;

  DirtyRect rect;
  rect.x1 = static_cast<uint16_t>(x1);
  rect.y1 = static_cast<uint16_t>(y1);
  rect.x2 = static_cast<uint16_t>(x2);
  rect.y2 = static_cast<uint16_t>(y2);

  this->merge_rect_into_list_(this->pending_rects_, this->pending_rect_count_, rect);
}

void MIPI_DSI::merge_rects_into_pending_(const DirtyRect *rects, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    this->merge_rect_into_list_(this->pending_rects_, this->pending_rect_count_, rects[i]);
  }
}





void MIPI_DSI::copy_rect_to_buffer_(int x_start, int y_start, int w, int h, const uint8_t *ptr, int x_offset,
                                    int y_offset, int x_pad) {
  if (w <= 0 || h <= 0)
    return;
  if (!this->check_buffer_())
    return;

  const size_t bytes_per_pixel = 3 - this->color_depth_;
  const size_t src_stride = static_cast<size_t>(x_offset + w + x_pad) * bytes_per_pixel;
  const uint8_t *src =
      ptr + static_cast<size_t>(y_offset) * src_stride + static_cast<size_t>(x_offset) * bytes_per_pixel;

  const size_t dst_stride = this->width_ * bytes_per_pixel;
  uint8_t *dst =
      this->buffer_ + (static_cast<size_t>(y_start) * this->width_ + static_cast<size_t>(x_start)) * bytes_per_pixel;

  const size_t copy_bytes = static_cast<size_t>(w) * bytes_per_pixel;

  for (int y = 0; y < h; y++) {
    memcpy(dst, src, copy_bytes);
    dst += dst_stride;
    src += src_stride;
  }

  this->merge_pending_dirty_(x_start, y_start, x_start + w - 1, y_start + h - 1);
}


bool MIPI_DSI::start_present_() {
  if (!this->check_buffer_())
    return false;

  if (!this->use_panel_double_fb_ || this->panel_fbs_[0] == nullptr || this->panel_fbs_[1] == nullptr) {
    ESP_LOGE(TAG, "Internal DPI framebuffers are not available");
    return false;
  }

  if (this->present_in_progress_)
    return false;

  if (!this->has_pending_dirty_())
    return true;

  const uint8_t next_fb = this->active_panel_fb_ ^ 1;

  this->copy_dirty_rects_to_panel_fb_(next_fb, this->pending_rects_, this->pending_rect_count_);

  // 清掉上一次可能残留的完成信号
  xSemaphoreTake(this->io_lock_, 0);

  esp_err_t err =
      esp_lcd_panel_draw_bitmap(this->handle_, 0, 0, this->width_, this->height_, this->panel_fbs_[next_fb]);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_lcd_panel_draw_bitmap(flip) failed: %s", esp_err_to_name(err));
    return false;
  }

  this->inflight_rect_count_ = this->pending_rect_count_;
  for (uint8_t i = 0; i < this->pending_rect_count_; i++) {
    this->inflight_rects_[i] = this->pending_rects_[i];
  }
  this->reset_pending_dirty_();

  this->queued_panel_fb_ = next_fb;
  this->present_in_progress_ = true;
  return true;
}



void MIPI_DSI::service_present_queue_() {
  if (this->io_lock_ != nullptr && xSemaphoreTake(this->io_lock_, 0) == pdTRUE) {
    this->present_in_progress_ = false;
    this->active_panel_fb_ = this->queued_panel_fb_;

    if (this->inflight_rect_count_ > 0) {
      if (this->has_pending_dirty_()) {
        // 已经有新 dirty 了，不必先回填 inactive fb；
        // 直接把上一帧 rect 列表合并进 pending，下一次 present 一次性拷贝
        this->merge_rects_into_pending_(this->inflight_rects_, this->inflight_rect_count_);
      } else {
        // 没有新 dirty，才把上一帧 rect 列表镜像回另一块 fb，保持双 fb 同步
        const uint8_t inactive_fb = this->active_panel_fb_ ^ 1;
        this->copy_dirty_rects_to_panel_fb_(inactive_fb, this->inflight_rects_, this->inflight_rect_count_);
      }
    }

    this->reset_inflight_dirty_();
  }

  if (!this->present_in_progress_ && this->present_pending_) {
    this->present_pending_ = false;
    if (!this->start_present_()) {
      this->present_pending_ = true;
    }
  }
}






void MIPI_DSI::copy_dirty_rect_to_panel_fb_(uint8_t fb_index, int x1, int y1, int x2, int y2) {
  if (fb_index > 1 || this->panel_fbs_[fb_index] == nullptr)
    return;
  if (x1 > x2 || y1 > y2)
    return;

  const size_t bpp = 3 - this->color_depth_;
  const size_t width = x2 - x1 + 1;
  const size_t height = y2 - y1 + 1;
  const size_t copy_bytes = width * bpp;
  const size_t stride = this->width_ * bpp;

  const uint8_t *src =
      this->buffer_ + (static_cast<size_t>(y1) * this->width_ + static_cast<size_t>(x1)) * bpp;
  uint8_t *dst =
      this->panel_fbs_[fb_index] + (static_cast<size_t>(y1) * this->width_ + static_cast<size_t>(x1)) * bpp;

  for (size_t row = 0; row < height; row++) {
    memcpy(dst, src, copy_bytes);
    src += stride;
    dst += stride;
  }
}
void MIPI_DSI::copy_dirty_rects_to_panel_fb_(uint8_t fb_index, const DirtyRect *rects, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    this->copy_dirty_rect_to_panel_fb_(fb_index, rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2);
  }
}
/////////////////////////////////////////////////patch



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
      .bus_id = 0,  // index from 0, specify the DSI host to use
      .num_data_lanes =
          this->lanes_,  // Number of data lanes to use, can't set a value that exceeds the chip's capability
      .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,  // Clock source for the DPHY
      .lane_bit_rate_mbps = this->lane_bit_rate_,   // Bit rate of the data lanes, in Mbps
  };
  auto err = esp_lcd_new_dsi_bus(&bus_config, &this->bus_handle_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("lcd_new_dsi_bus failed"), err);
    return;
  }
  esp_lcd_dbi_io_config_t dbi_config = {
      .virtual_channel = 0,
      .lcd_cmd_bits = 8,    // according to the LCD spec
      .lcd_param_bits = 8,  // according to the LCD spec
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
                                           .num_fbs = 2,  // number of frame buffers to allocate
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
  // need to know when the display is ready for SLPOUT command - will be 120ms after reset
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
        // are we ready, boots?
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
  this->frame_buffer_size_ = static_cast<size_t>(this->width_) * this->height_ * (3 - this->color_depth_);
  this->queued_panel_fb_ = 0;
  this->present_in_progress_ = false;
  this->present_pending_ = false;
  this->reset_dirty_window_();
  this->reset_pending_dirty_();
  this->reset_inflight_dirty_();


  void *fb0 = nullptr;
  void *fb1 = nullptr;
  err = esp_lcd_dpi_panel_get_frame_buffer(this->handle_, 2, &fb0, &fb1);
  if (err != ESP_OK || fb0 == nullptr || fb1 == nullptr) {
    this->smark_failed(LOG_STR("esp_lcd_dpi_panel_get_frame_buffer failed"), err == ESP_OK ? ESP_FAIL : err);
    return;
  }

  this->panel_fbs_[0] = static_cast<uint8_t *>(fb0);
  this->panel_fbs_[1] = static_cast<uint8_t *>(fb1);
  this->use_panel_double_fb_ = true;
  this->active_panel_fb_ = 0;
  this->queued_panel_fb_ = 0;

  memset(this->panel_fbs_[0], 0, this->frame_buffer_size_);
  memset(this->panel_fbs_[1], 0, this->frame_buffer_size_);

  esp_lcd_dpi_panel_event_callbacks_t cbs = {
      .on_refresh_done = notify_refresh_ready,
  };

  err = esp_lcd_dpi_panel_register_event_callbacks(this->handle_, &cbs, this->io_lock_);
  if (err != ESP_OK) {
    this->smark_failed(LOG_STR("Failed to register callbacks"), err);
    return;
  }

  ESP_LOGCONFIG(TAG, "MIPI DSI setup complete");
}

void MIPI_DSI::loop() {
  this->service_present_queue_();
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

  this->merge_pending_dirty_(this->x_low_, this->y_low_, this->x_high_, this->y_high_);
  this->present_pending_ = true;
  //this->service_present_queue_();
  this->reset_dirty_window_();
}


void MIPI_DSI::draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                              display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) {
  if (w <= 0 || h <= 0)
    return;
  // if color mapping is required, pass the buck.
  // note that endianness is not considered here - it is assumed to match!
  if (bitness != this->color_depth_) {
    display::Display::draw_pixels_at(x_start, y_start, w, h, ptr, order, bitness, big_endian, x_offset, y_offset,
                                     x_pad);
    return;
  }
  this->write_to_display_(x_start, y_start, w, h, ptr, x_offset, y_offset, x_pad);
}




void MIPI_DSI::write_to_display_(int x_start, int y_start, int w, int h, const uint8_t *ptr, int x_offset, int y_offset,
                                 int x_pad) {
  if (w <= 0 || h <= 0)
    return;

  if (ptr != this->buffer_) {
    this->copy_rect_to_buffer_(x_start, y_start, w, h, ptr, x_offset, y_offset, x_pad);
  } else {
    this->merge_pending_dirty_(x_start, y_start, x_start + w - 1, y_start + h - 1);
  }

  this->present_pending_ = true;
}





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

  memset(this->buffer_, 0, this->height_ * this->width_ * bytes_per_pixel);
  this->reset_dirty_window_();
  this->reset_pending_dirty_();
  this->reset_inflight_dirty_();
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
  if (!this->check_buffer_())
    return;
  size_t pos = (y * this->width_) + x;
  switch (this->color_depth_) {
    case display::COLOR_BITNESS_565: {
      auto *ptr_16 = reinterpret_cast<uint16_t *>(this->buffer_);
      uint8_t hi_byte = static_cast<uint8_t>(color.r & 0xF8) | (color.g >> 5);
      uint8_t lo_byte = static_cast<uint8_t>((color.g & 0x1C) << 3) | (color.b >> 3);
      uint16_t new_color = lo_byte | (hi_byte << 8);  // little endian
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
  // low and high watermark may speed up drawing from buffer
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

  // If clipping is active, fall back to base implementation
  if (this->get_clipping().is_set()) {
    Display::fill(color);
    return;
  }

  switch (this->color_depth_) {
    case display::COLOR_BITNESS_565: {
      auto *ptr_16 = reinterpret_cast<uint16_t *>(this->buffer_);
      uint8_t hi_byte = static_cast<uint8_t>(color.r & 0xF8) | (color.g >> 5);
      uint8_t lo_byte = static_cast<uint8_t>((color.g & 0x1C) << 3) | (color.b >> 3);
      uint16_t new_color = lo_byte | (hi_byte << 8);  // little endian
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
  this->x_low_ = 0;
  this->y_low_ = 0;
  this->x_high_ = this->width_ - 1;
  this->y_high_ = this->height_ - 1;
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
