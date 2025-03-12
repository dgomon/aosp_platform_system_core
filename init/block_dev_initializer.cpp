// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <chrono>
#include <string_view>
#include <vector>

#include <android-base/chrono_utils.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <fs_mgr.h>

#include "block_dev_initializer.h"

namespace android {
namespace init {

using android::base::Timer;
using namespace std::chrono_literals;

BlockDevInitializer::BlockDevInitializer() : uevent_listener_(16 * 1024 * 1024) {
    auto boot_devices = android::fs_mgr::GetBootDevices();
    boot_devices_ = boot_devices;
    device_handler_ = std::make_unique<DeviceHandler>(
            std::vector<Permissions>{}, std::vector<SysfsPermissions>{}, std::vector<Subsystem>{},
            std::move(boot_devices), false);
}

bool BlockDevInitializer::InitDeviceMapper() {
    return InitMiscDevice("device-mapper");
}

bool BlockDevInitializer::InitDmUser(const std::string& name) {
    return InitMiscDevice("dm-user!" + name);
}

bool BlockDevInitializer::InitMiscDevice(const std::string& name) {
    const std::string dm_path = "/devices/virtual/misc/" + name;
    bool found = false;
    auto dm_callback = [this, &dm_path, &found](const Uevent& uevent) {
        if (uevent.path == dm_path) {
            device_handler_->HandleUevent(uevent);
            found = true;
            return ListenerAction::kStop;
        }
        return ListenerAction::kContinue;
    };
    uevent_listener_.RegenerateUeventsForPath("/sys" + dm_path, dm_callback);
    if (!found) {
        LOG(INFO) << name << " device not found in /sys, waiting for its uevent";
        Timer t;
        uevent_listener_.Poll(dm_callback, 10s);
        LOG(INFO) << "Wait for " << name << " returned after " << t;
    }
    if (!found) {
        LOG(ERROR) << name << " device not found after polling timeout";
        return false;
    }
    return true;
}

ListenerAction BlockDevInitializer::HandleUevent(const Uevent& uevent,
                                                 std::set<std::string>* devices) {
    LOG(INFO) << __PRETTY_FUNCTION__ << ": Received uevent with subsystem: " << uevent.subsystem
              << ", device name: " << uevent.device_name << ", path: " << uevent.path;

    // Ignore everything that is not a block device.
    if (uevent.subsystem != "block") {
        LOG(DEBUG) << __PRETTY_FUNCTION__ << ": Ignoring non-block device";
        return ListenerAction::kContinue;
    }

    auto name = uevent.partition_name;
    if (name.empty()) {
        size_t base_idx = uevent.path.rfind('/');
        if (base_idx == std::string::npos) {
            LOG(WARNING) << __PRETTY_FUNCTION__ << ": Unable to extract partition name from path: " << uevent.path;
            return ListenerAction::kContinue;
        }
        name = uevent.path.substr(base_idx + 1);
    }

    LOG(INFO) << __PRETTY_FUNCTION__ << ": Extracted partition name: " << name;

    auto iter = devices->find(name);
    if (iter == devices->end()) {
        LOG(DEBUG) << __PRETTY_FUNCTION__ << ": Partition name not found in devices set, checking alternate names.";

        auto partition_name = DeviceHandler::GetPartitionNameForDevice(uevent.device_name);
        if (!partition_name.empty()) {
            LOG(INFO) << __PRETTY_FUNCTION__ << ": Using alternate partition name: " << partition_name;
            iter = devices->find(partition_name);
        }
        if (iter == devices->end()) {
            LOG(WARNING) << __PRETTY_FUNCTION__ << ": Partition not found, ignoring uevent.";
            return ListenerAction::kContinue;
        }
    }

    // Check if the device is an MMC (SD/eMMC) and if it's a valid boot device
    if (uevent.path.find("mmc") != uevent.path.npos) {
        LOG(INFO) << __PRETTY_FUNCTION__ << ": MMC device detected in path: " << uevent.path;

        auto boot_device_it = std::find_if(boot_devices_.begin(), boot_devices_.end(),
                                           [&](const std::string& boot_dev) {
                                               return uevent.path.find(boot_dev) != std::string::npos;
                                           });

        if (boot_device_it == boot_devices_.end()) {
            LOG(WARNING) << __PRETTY_FUNCTION__ << ": MMC device is not a known boot device, ignoring.";
            return ListenerAction::kContinue;
        }
        LOG(INFO) << __PRETTY_FUNCTION__ << ": MMC device is a valid boot device: " << *boot_device_it;
    }

    LOG(VERBOSE) << __PRETTY_FUNCTION__ << ": Found partition: " << name << ", processing uevent.";

    devices->erase(iter);
    device_handler_->HandleUevent(uevent);

    bool should_stop = devices->empty();
    LOG(INFO) << __PRETTY_FUNCTION__ << ": Returning " << (should_stop ? "ListenerAction::kStop" : "ListenerAction::kContinue");
    return should_stop ? ListenerAction::kStop : ListenerAction::kContinue;
}

bool BlockDevInitializer::InitDevices(std::set<std::string> devices) {
    auto uevent_callback = [&, this](const Uevent& uevent) -> ListenerAction {
        return HandleUevent(uevent, &devices);
    };
    uevent_listener_.RegenerateUevents(uevent_callback);

    // UeventCallback() will remove found partitions from |devices|. So if it
    // isn't empty here, it means some partitions are not found.
    if (!devices.empty()) {
        LOG(INFO) << __PRETTY_FUNCTION__
                  << ": partition(s) not found in /sys, waiting for their uevent(s): "
                  << android::base::Join(devices, ", ");
        Timer t;
        uevent_listener_.Poll(uevent_callback, 10s);
        LOG(INFO) << "Wait for partitions returned after " << t;
    }

    if (!devices.empty()) {
        LOG(ERROR) << __PRETTY_FUNCTION__ << ": partition(s) not found after polling timeout: "
                   << android::base::Join(devices, ", ");
        return false;
    }
    return true;
}

// Creates "/dev/block/dm-XX" for dm nodes by running coldboot on /sys/block/dm-XX.
bool BlockDevInitializer::InitDmDevice(const std::string& device) {
    const std::string device_name(basename(device.c_str()));
    const std::string syspath = "/sys/block/" + device_name;
    bool found = false;

    auto uevent_callback = [&device_name, &device, this, &found](const Uevent& uevent) {
        if (uevent.device_name == device_name) {
            LOG(VERBOSE) << "Creating device-mapper device : " << device;
            device_handler_->HandleUevent(uevent);
            found = true;
            return ListenerAction::kStop;
        }
        return ListenerAction::kContinue;
    };

    uevent_listener_.RegenerateUeventsForPath(syspath, uevent_callback);
    if (!found) {
        LOG(INFO) << "dm device '" << device << "' not found in /sys, waiting for its uevent";
        Timer t;
        uevent_listener_.Poll(uevent_callback, 10s);
        LOG(INFO) << "wait for dm device '" << device << "' returned after " << t;
    }
    if (!found) {
        LOG(ERROR) << "dm device '" << device << "' not found after polling timeout";
        return false;
    }
    return true;
}

}  // namespace init
}  // namespace android
