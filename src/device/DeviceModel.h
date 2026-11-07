#pragma once
#include "main.h"
#include <algorithm>
#include <vector>

// Keep existing cards (and their focus/popups) alive across ADB polls.
class DeviceModel : public slint::VectorModel<DeviceRow> {
public:
  void update(const std::vector<DeviceRow> &devices) {
    for (size_t i = row_count(); i > 0; --i) {
      auto current = row_data(i - 1).value();
      if (std::none_of(devices.begin(), devices.end(), [&](const auto &device) {
            return device.serial == current.serial;
          }))
        erase(i - 1);
    }
    for (const auto &device : devices) {
      size_t i = 0;
      for (; i < row_count(); ++i) {
        auto current = row_data(i).value();
        if (current.serial == device.serial) {
          if (!(current == device))
            set_row_data(i, device);
          break;
        }
      }
      if (i == row_count())
        push_back(device);
    }
  }
};
