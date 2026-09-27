// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/system/MoonCloudModule.h"
#include "core/module/Scheduler.h"
#include "core/util/JsonSink.h"
#include "core/module/MoonModule.h"
#include "platform/platform.h"

namespace mm {

/// A MoonCloud child with its own consent, sharing a chip model implying nothing more.
/// Everything sent is public and permanent: no private message, and no delete.
/// The module never posts on its own, so a message exists because somebody typed it.
///
/// @moreinfo
///
/// Two consents govern it: one allows posting, the other shares the device name.
/// The second is off by default, a name saying more than an identifier does.
/// Without it a message carries the first characters of the installation id.
///
/// Reading sends no identifier, and happens only while consent is on.
/// There is no authentication, so a sender id can be fabricated by hand.
/// That is acceptable where nothing is gated on identity, and is stated in the policy.
class MoonTalkModule : public MoonModule {
public:
    /// Declare the two consents, the message box, and the button that publishes.
    void defineControls() override {
        controls_.clear();
        controls_.addControl("consent", consent_);       // publishing is on or off
        controls_.addControl("shareName", shareName_);   // off: a name identifies a person
        controls_.addText("message", message_, sizeof(message_));
        controls_.addButton("send");                     // the only thing that publishes
    }

    /// Publish the consent explanation, a message being sent only when the button is pressed.
    void setup() override {
        refreshStatus();
        MoonModule::setup();
    }

    /// Say what this setting exchanges, on the status slot rather than only in the policy.
    void refreshStatus() {
        if (consent_) clearStatus();
        else setStatus("Off. Switch on to post to a public board shared by MoonLight devices. "
                       "What you type is readable by anyone, permanently, and cannot be withdrawn. "
                       "Each message carries the country it came from and the time it was sent.");
    }

    /// Refresh the explanation on a consent change, and publish on the button.
    void onControlChanged(const char* name) override {
        if (name && std::strcmp(name, "consent") == 0) { refreshStatus(); return; }
        if (!name || std::strcmp(name, "send") != 0) return;
        if (!consent_) return;           // never publish without consent
        if (message_[0] == 0) return;    // nothing typed

        // Only on a successful hand-off, so an unreachable server keeps what somebody typed.
        if (send()) {
            message_[0] = 0;
            markDirty();
        }
    }

    /// The device's name, read at send time so a rename takes effect on the next message.
    const char* deviceName() const {
        auto* sched = Scheduler::instance();
        const MoonModule* system = sched ? sched->firstByName("System") : nullptr;
        if (!system) return "";
        auto& ctrls = system->controls();
        for (uint8_t c = 0; c < ctrls.count(); c++) {
            const ControlDescriptor& d = ctrls[c];
            if (d.ptr && d.name && std::strcmp(d.name, "deviceName") == 0 &&
                (d.type == ControlType::Text || d.type == ControlType::ReadOnly)) {
                return static_cast<const char*>(d.ptr);
            }
        }
        return "";
    }

    /// Whether posting is allowed at all.
    bool consent() const { return consent_; }
    /// Whether the device name rides along, which needs both consents.
    bool sharesName() const { return shareName_ && consent_; }
    /// What is currently typed in the box.
    const char* message() const { return message_; }

    /// Set the posting consent directly, so a test needs no UI write.
    void setConsentForTest(bool yes) { consent_ = yes; refreshStatus(); }
    /// Set the name consent directly, so a test can walk the matrix.
    void setShareNameForTest(bool share) { shareName_ = share; }

    /// Put text in the box the way a control write does, so a test can press send without a UI.
    void setMessageForTest(const char* text) {
        std::snprintf(message_, sizeof(message_), "%s", text ? text : "");
    }

private:
    /// Build and post one message, returning false so the caller can keep unsent text.
    bool send() {
        char id[kInstallationIdChars + 1] = {};
        installationId(id);
        if (!id[0]) return false;

        JsonSink body;
        body.append("{\"sender\":");
        body.writeJsonString(id);
        // Omitted rather than sent empty, and only when both consents allow it.
        const char* name = deviceName();
        if (shareName_ && name && name[0]) {
            body.append(",\"name\":");
            body.writeJsonString(name);
        }
        body.append(",\"text\":");
        body.writeJsonString(message_);
        body.append("}");
        body.flush();

        // Through the container, which owns the address.
        auto* cloud = static_cast<const MoonCloudModule*>(parent());
        return cloud && cloud->post("/api/talk", body.data());
    }

    bool consent_ = false;      ///< whether posting is allowed
    bool shareName_ = false;    ///< whether the device name rides along
    // Sized to the server's own limit plus a terminator, rather than under it.
    char message_[281] = {};
};

}  // namespace mm

