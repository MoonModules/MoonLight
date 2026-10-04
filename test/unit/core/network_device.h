#pragma once

#include "core/system/FilesystemModule.h"
#include "core/system/NetworkModule.h"
#include "core/system/EthernetModule.h"
#include "core/system/WiFiModule.h"
#include "core/system/AccessPointModule.h"
#include "core/system/SystemModule.h"
#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "platform/platform.h"

namespace mm::test {

/// A device booting on a config folder, with Network and its interface cards wired as main wires them: the interfaces first, so their keys come before any other child's.
struct NetworkDevice {
    Scheduler scheduler;                             ///< owns the three top-level modules
    FilesystemModule* fs = new FilesystemModule();   ///< loads and saves the config folder
    SystemModule* sys = new SystemModule();          ///< the device name
    NetworkModule* net = new NetworkModule();        ///< the module under test
    EthernetModule* eth = nullptr;                   ///< Network's first child
    WiFiModule* wifi = nullptr;                      ///< Network's second child
    AccessPointModule* ap = nullptr;                 ///< Network's third child

    /// Boot on the config folder at `root`.
    explicit NetworkDevice(const char* root) {
        platform::fsSetRoot(root);
        ModuleFactory::registerType<EthernetModule>("EthernetModule");
        ModuleFactory::registerType<WiFiModule>("WiFiModule");
        ModuleFactory::registerType<AccessPointModule>("AccessPointModule");
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        sys->setTypeName("SystemModule");
        net->setTypeName("NetworkModule");
        eth = static_cast<EthernetModule*>(ModuleFactory::create("EthernetModule"));
        wifi = static_cast<WiFiModule*>(ModuleFactory::create("WiFiModule"));
        ap = static_cast<AccessPointModule*>(ModuleFactory::create("AccessPointModule"));
        for (MoonModule* child : {static_cast<MoonModule*>(eth), static_cast<MoonModule*>(wifi), static_cast<MoonModule*>(ap)}) {
            child->markWiredByCode();
            net->addChild(child);
        }
        net->setEthernet(eth);
        net->setWiFi(wifi);
        net->setAccessPoint(ap);
        net->setSystemModule(sys);
        scheduler.addModule(fs);
        scheduler.addModule(sys);
        scheduler.addModule(net);
        scheduler.setup();
    }
    /// Release the tree.
    ~NetworkDevice() { scheduler.release(); }
};

}  // namespace mm::test
