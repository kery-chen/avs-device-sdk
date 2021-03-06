/*
 * FocusManager.cpp
 *
 * Copyright 2017 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "AFML/FocusManager.h"
#include <AVSCommon/Utils/Logger/Logger.h>

namespace alexaClientSDK {
namespace afml {

using namespace avsCommon::sdkInterfaces;
using namespace avsCommon::utils;
using namespace avsCommon::avs;

/// String to identify log entries originating from this file.
static const std::string TAG("FocusManager");

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsCommon::utils::logger::LogEntry(TAG, event)

FocusManager::FocusManager(const std::vector<ChannelConfiguration>& channelConfigurations) {
    for (auto config : channelConfigurations) {
        if (doesChannelNameExist(config.name)) {
            ACSDK_ERROR(LX("createChannelFailed")
                    .d("reason", "channelNameExists")
                    .d("config", config.toString()));
            continue;
        }
        if (doesChannelPriorityExist(config.priority)) {
            ACSDK_ERROR(LX("createChannelFailed")
                    .d("reason", "channelPriorityExists")
                    .d("config", config.toString()));
            continue;
        }

        auto channel = std::make_shared<Channel>(config.priority);
        m_allChannels.insert({config.name, channel});
    }
}

bool FocusManager::acquireChannel(
        const std::string& channelName,
        std::shared_ptr<ChannelObserverInterface> channelObserver,
        const std::string& activityId) {
    std::shared_ptr<Channel> channelToAcquire = getChannel(channelName);
    if (!channelToAcquire) {
        ACSDK_ERROR(LX("acquireChannelFailed")
                .d("reason", "channelNotFound")
                .d("channelName", channelName));
        return false;
    }

    m_executor.submit(
        [this, channelToAcquire, channelObserver, activityId] () {
            acquireChannelHelper(channelToAcquire, channelObserver, activityId);
        }
    );
    return true;
}

std::future<bool> FocusManager::releaseChannel(
        const std::string& channelName, std::shared_ptr<ChannelObserverInterface> channelObserver) {
    // Using a shared_ptr here so that the promise stays in scope by the time the Executor picks up the task.
    auto releaseChannelSuccess = std::make_shared<std::promise<bool>>();
    std::future<bool> returnValue = releaseChannelSuccess->get_future();
    std::shared_ptr<Channel> channelToRelease = getChannel(channelName);
    if (!channelToRelease) {
        ACSDK_ERROR(LX("releaseChannelFailed")
                .d("reason", "channelNotFound")
                .d("channelName", channelName));
        releaseChannelSuccess->set_value(false);
        return returnValue;
    }

    m_executor.submit(
        [this, channelToRelease, channelObserver, releaseChannelSuccess, channelName] () {
            releaseChannelHelper(channelToRelease, channelObserver, releaseChannelSuccess, channelName);
        }
    );

    return returnValue;
}

void FocusManager::stopForegroundActivity() {
    // We lock these variables so that we can correctly capture the currently foregrounded channel and activity.
    std::unique_lock<std::mutex> lock(m_mutex);
    std::shared_ptr<Channel> foregroundChannel = getHighestPriorityActiveChannelLocked();
    if (!foregroundChannel) {
        ACSDK_DEBUG(LX("stopForegroundActivityFailed").d("reason", "noForegroundActivity"));
        return;
    }

    std::string foregroundChannelActivityId = foregroundChannel->getActivityId();
    lock.unlock();

    m_executor.submitToFront(
        [this, foregroundChannel, foregroundChannelActivityId] () {
            stopForegroundActivityHelper(foregroundChannel, foregroundChannelActivityId);
        }
    );
}

void FocusManager::acquireChannelHelper(
        std::shared_ptr<Channel> channelToAcquire,
        std::shared_ptr<ChannelObserverInterface> channelObserver,
        const std::string& activityId) {
    // Lock here to update internal state which stopForegroundActivity may concurrently access.
    std::unique_lock<std::mutex> lock(m_mutex);
    std::shared_ptr<Channel> foregroundChannel = getHighestPriorityActiveChannelLocked();
    channelToAcquire->setActivityId(activityId);
    m_activeChannels.insert(channelToAcquire);
    lock.unlock();

    channelToAcquire->setObserver(channelObserver);
    if (!foregroundChannel) {
        channelToAcquire->setFocus(FocusState::FOREGROUND);
    } else if (foregroundChannel == channelToAcquire) {
        channelToAcquire->setFocus(FocusState::FOREGROUND);
    } else if (*channelToAcquire > *foregroundChannel) {
        foregroundChannel->setFocus(FocusState::BACKGROUND);
        channelToAcquire->setFocus(FocusState::FOREGROUND);
    } else {
        channelToAcquire->setFocus(FocusState::BACKGROUND);
    }
}

void FocusManager::releaseChannelHelper(
        std::shared_ptr<Channel> channelToRelease,
        std::shared_ptr<ChannelObserverInterface> channelObserver,
        std::shared_ptr<std::promise<bool>> releaseChannelSuccess,
        const std::string& name) {
    if (!channelToRelease->doesObserverOwnChannel(channelObserver)) {
        ACSDK_ERROR(LX("releaseChannelHelperFailed").d("reason", "observerDoesNotOwnChannel").d("channel", name));
        releaseChannelSuccess->set_value(false);
        return;
    }

    releaseChannelSuccess->set_value(true);
    // Lock here to update internal state which stopForegroundActivity may concurrently access.
    std::unique_lock<std::mutex> lock(m_mutex);
    bool wasForegrounded = isChannelForegroundedLocked(channelToRelease);
    m_activeChannels.erase(channelToRelease);
    lock.unlock();

    channelToRelease->setFocus(FocusState::NONE);
    if (wasForegrounded) {
        foregroundHighestPriorityActiveChannel();
    }
}

void FocusManager::stopForegroundActivityHelper(
        std::shared_ptr<Channel> foregroundChannel, std::string foregroundChannelActivityId) {
    if (!foregroundChannel->stopActivity(foregroundChannelActivityId)) {
        return;
    }

    // Lock here to update internal state which stopForegroundActivity may concurrently access.
    std::unique_lock<std::mutex> lock(m_mutex);
    foregroundChannel->setActivityId("");
    m_activeChannels.erase(foregroundChannel);
    lock.unlock();
    foregroundHighestPriorityActiveChannel();
}

std::shared_ptr<Channel> FocusManager::getChannel(const std::string& channelName) const {
    auto search = m_allChannels.find(channelName);
    if (search != m_allChannels.end()) {
        return search->second;
    }
    return nullptr;
}

std::shared_ptr<Channel> FocusManager::getHighestPriorityActiveChannelLocked() const {
    if (m_activeChannels.empty()) {
        return nullptr;
    }
    return *m_activeChannels.begin();
}

bool FocusManager::isChannelForegroundedLocked(const std::shared_ptr<Channel>& channel) const {
    return getHighestPriorityActiveChannelLocked() == channel;
}

bool FocusManager::doesChannelNameExist(const std::string& name) const {
    return m_allChannels.find(name) != m_allChannels.end();
}

bool FocusManager::doesChannelPriorityExist(const unsigned int priority) const {
    for (auto it = m_allChannels.begin(); it != m_allChannels.end(); ++it) {
        if (it->second->getPriority() == priority) {
            return true;
        }
    }
    return false;
}

void FocusManager::foregroundHighestPriorityActiveChannel() {
    // Lock here to update internal state which stopForegroundActivity may concurrently access.
    std::unique_lock<std::mutex> lock(m_mutex);
    std::shared_ptr<Channel> channelToForeground = getHighestPriorityActiveChannelLocked();
    lock.unlock();

    if (channelToForeground) {
        channelToForeground->setFocus(FocusState::FOREGROUND);
    }
}

} // namespace afml
} // namespace alexaClientSDK
