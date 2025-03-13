/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "switch_root.h"

#include <fcntl.h>
#include <mntent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/strings.h>

using android::base::StartsWith;

using namespace std::literals;

namespace android {
namespace init {

namespace {

std::vector<std::string> GetMounts(const std::string& new_root) {
    auto fp = std::unique_ptr<std::FILE, decltype(&endmntent)>{setmntent("/proc/mounts", "re"),
                                                               endmntent};
    if (fp == nullptr) {
        PLOG(FATAL) << "Failed to open /proc/mounts";
    }

    std::vector<std::string> result;
    mntent* mentry;
    while ((mentry = getmntent(fp.get())) != nullptr) {
        // We won't try to move rootfs.
        if (mentry->mnt_dir == "/"s) {
            continue;
        }

        // The new root mount is handled separately.
        if (mentry->mnt_dir == new_root) {
            continue;
        }

        // Move operates on subtrees, so do not try to move children of other mounts.
        if (std::find_if(result.begin(), result.end(), [&mentry](const auto& older_mount) {
                return StartsWith(mentry->mnt_dir, older_mount);
            }) != result.end()) {
            continue;
        }

        result.emplace_back(mentry->mnt_dir);
    }

    return result;
}

}  // namespace

void SwitchRoot(const std::string& new_root) {
    auto mounts = GetMounts(new_root);

    LOG(INFO) << "Switching root to '" << new_root << "'";

    for (const auto& mount_path : mounts) {
        LOG(INFO) << "Checkpoint 5.4.1";
        sleep(1);
//        LOG(INFO) << "Moving mount at '" << mount_path << "' to '" << new_root + mount_path << "'";
//        auto new_mount_path = new_root + mount_path;
//        mkdir(new_mount_path.c_str(), 0755);
//        LOG(INFO) << "Created directory '" << new_mount_path << "'";
//        if (mount(mount_path.c_str(), new_mount_path.c_str(), nullptr, MS_MOVE, nullptr) != 0) {
//            PLOG(FATAL) << "Unable to move mount at '" << mount_path << "' to "
//                        << "'" << new_mount_path << "'";
//        } else {
//            LOG(INFO) << "Moved mount at '" << mount_path << "' to '" << new_mount_path << "'";
//        }
    }
   LOG(INFO) << "Checkpoint 5.4.2";

    LOG(INFO) << "Chdir to new root '" << new_root << "'";

    if (chdir(new_root.c_str()) != 0) {
        PLOG(FATAL) << "Could not chdir to new_root, '" << new_root << "'";
    }

    LOG(INFO) << "Pivot_root to new root '" << new_root << "'";

    if (mount(new_root.c_str(), "/", nullptr, MS_MOVE, nullptr) != 0) {
        PLOG(FATAL) << "Unable to move root mount to new_root, '" << new_root << "'";
    }

    LOG(INFO) << "Chroot to new root";

    if (chroot(".") != 0) {
        PLOG(FATAL) << "Unable to chroot to new root";
    }
}

}  // namespace init
}  // namespace android
