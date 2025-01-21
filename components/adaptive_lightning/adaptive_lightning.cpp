#include "adaptive_lightning.h"

#include <cmath>
#include <utility>

#include "esphome/core/log.h"

static const char *TAG = "adaptive_lightning";

namespace esphome {
namespace adaptive_lightning {

void AdaptiveLightningComponent::setup() {
  if (light_ != nullptr) {
    light_->add_new_remote_values_callback([this]() { handle_light_state_change(); });
    light_->add_new_target_state_reached_callback([this]() { handle_target_state_reached(); });

    auto traits = light_->get_traits();
    if (min_mireds_ <= 0) {
      min_mireds_ = traits.get_min_mireds();
    }
    if (max_mireds_ <= 0) {
      max_mireds_ = traits.get_max_mireds();
    }
    ESP_LOGD(TAG, "Color temperature range: %.3f - %.3f", min_mireds_, max_mireds_);
  }
  if (this->restore_mode == switch_::SWITCH_ALWAYS_ON) {
    this->publish_state(true);
  }
}

void AdaptiveLightningComponent::update() {
  if (light_ == nullptr || sun_ == nullptr) {
    ESP_LOGW(TAG, "Light or Sun component not set!");
    return;
  }

  if (!this->state) {
    ESP_LOGD(TAG, "Update skipped - automatic updates disabled");
    return;
  }

  if (!light_->remote_values.is_on()) {
    ESP_LOGD(TAG, "Update skipped - light is off");
    return;
  }

  // Get current timestamp
  const auto now = sun_->get_time()->now();
  // Calculate start of day, to get today's events, not next events
  auto today = now;
  today.hour = today.minute = today.second = 0;
  today.recalc_timestamp_utc();

  auto sunrise = sun_->sunrise(today, sunrise_elevation_);
  auto sunset = sun_->sunset(today, sunset_elevation_);

  if (!sunrise || !sunset) {
    ESP_LOGW(TAG, "Could not determine sunrise or sunset");
    return;
  }

  // Calculate
  float mireds = calc_color_temperature(now.timestamp, sunrise->timestamp, sunset->timestamp);

  // Compare if < 0.1 difference
  if (std::fabs(mireds - last_requested_color_temp_) < 0.1) {
    // This is mandatory to avoid infinite loops when the light is updated
    ESP_LOGD(TAG, "Skipping update, color temperature is the same as last requested");
    return;
  }
  last_requested_color_temp_ = mireds;

  ESP_LOGD(TAG, "Setting color temperature %.3f", mireds);
  auto call = light_->make_call();
  call.set_color_temperature(mireds);
  // add brightness to the effect, otherwise it might not get recalculated properly
  call.set_brightness(light_->remote_values.get_brightness());
  if (transition_length_ > 0) {
    call.set_transition_length_if_supported(transition_length_);
  }
  call.perform();
}

void AdaptiveLightningComponent::write_state(bool state) {
  if (this->state != state) {
    if (state) {
      ESP_LOGD(TAG, "Adaptive lighting enabled");
    } else {
      ESP_LOGD(TAG, "Adaptive lighting disabled");
    }

    this->force_next_update();
    this->publish_state(state);
    this->update();
    this->force_next_update(); // Force update again, to update color after turn-on transition
  }
}

void AdaptiveLightningComponent::handle_light_state_change() {
  if (light_ == nullptr)
    return;

  bool current_state = light_->remote_values.is_on();

  // Light is on
  if (current_state) {
    float current_temp = light_->remote_values.get_color_temperature();

    // Check if we have a previous temperature set and if it differs
    if (this->state && last_requested_color_temp_ > 0 && std::fabs(current_temp - last_requested_color_temp_) > 0.1) {
      ESP_LOGI(
          TAG,
          "Color temperature changed externally (current: %.3f, last requested: %.3f), disabling adaptive lighting",
          current_temp, last_requested_color_temp_);
      this->write_state(false);
    } else if (!previous_light_state_ && !this->state && // Light was just turned on
               this->restore_mode == switch_::SWITCH_ALWAYS_ON) {
      // Enable the switch when light turns on if restore mode is ALWAYS_ON
      this->write_state(true);
    }
  }

  previous_light_state_ = current_state;
}

void AdaptiveLightningComponent::handle_target_state_reached() {
  if (light_ == nullptr)
    return;

  // We rely on previous_light_state_ was set in handle_light_state_change
  if (previous_light_state_ && this->state) {
    // Update color temperature if adaptive lighting is enabled
    this->update();
  }
}

// x in [0, 1]
static float smooth_transition(float x, float y_min, float y_max, float speed = 1) {
  // This influences transition curve and speed
  constexpr double y1 = 0.00001;
  constexpr double y2 = 0.999;
  static float a = (std::atanh(2 * y2 - 1) - std::atanh(2 * y1 - 1));
  static float b = -(std::atanh(2 * y1 - 1) / a);
  auto x_adj = std::pow(std::fabs(1 - x * 2), speed);
  return y_min + (y_max - y_min) * 0.5 * (std::tanh(a * (x_adj - b)) + 1);
}

float AdaptiveLightningComponent::calc_color_temperature(const time_t now, const time_t sunrise, const time_t sunset,
                                                         float min_mireds, float max_mireds, float speed) {
  if (now < sunrise || now > sunset) {
    return max_mireds;
  } else {
    float position = float(now - sunrise) / float(sunset - sunrise);
    float mireds = smooth_transition(position, min_mireds, max_mireds, speed);
    // Round to one decimal place
    return std::roundf(mireds * 10) / 10;
  }
}

void AdaptiveLightningComponent::dump_config() {
  if (light_ == nullptr || sun_ == nullptr) {
    ESP_LOGW(TAG, "Light or Sun component not set!");
    return;
  }

  // Get current timestamp
  const auto now = sun_->get_time()->now();
  // Calculate start of day, to get today's events, not next events
  auto today = now;
  today.hour = today.minute = today.second = 0;
  today.recalc_timestamp_utc();

  auto sunrise = sun_->sunrise(today, sunrise_elevation_);
  auto sunset = sun_->sunset(today, sunset_elevation_);

  if (!sunrise || !sunset) {
    ESP_LOGW(TAG, "Could not determine sunrise or sunset");
    return;
  }

  ESP_LOGI(TAG, "Today: %s", today.strftime("%x %X").c_str());
  ESP_LOGI(TAG, "Sunrise: %s", sunrise->strftime("%x %X").c_str());
  ESP_LOGI(TAG, "Sunset: %s", sunset->strftime("%x %X").c_str());
  ESP_LOGI(TAG, "Sun elevation: %.3f", sun_->elevation());
  ESP_LOGI(TAG, "Sunrise elevation: %.3f, sunset elevation: %.3f", sunrise_elevation_, sunset_elevation_);
  ESP_LOGI(TAG, "Color temperature range: %.3f - %.3f", min_mireds_, max_mireds_);
  ESP_LOGI(TAG, "Transition length: %d", transition_length_);

  for (int i = 0; i < 24; i++) {
    auto time = today;
    time.hour = i;
    time.recalc_timestamp_utc();
    float mireds = calc_color_temperature(time.timestamp, sunrise->timestamp, sunset->timestamp);
    ESP_LOGI(TAG, "Time: %s, Color temperature: %.3f", time.strftime("%x %X").c_str(), mireds);
  }

  ESP_LOGI(TAG, "Last requested color temperature: %.3f", last_requested_color_temp_);
  ESP_LOGI(TAG, "State: %s", this->state ? "enabled" : "disabled");
  ESP_LOGI(TAG, "Previous light state: %s", previous_light_state_ ? "on" : "off");
  ESP_LOGI(TAG, "Current light state: %s", light_->remote_values.is_on() ? "on" : "off");
}

} // namespace adaptive_lightning
} // namespace esphome
