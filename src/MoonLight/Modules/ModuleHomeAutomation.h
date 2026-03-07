/**
    @title     MoonLight
    @file      ModuleHomeAutomation.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/homeautomation/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleHomeAutomation_h
#define ModuleHomeAutomation_h

#if FT_MOONLIGHT

#include "MoonBase/Module.h"
#include "MoonLight/Modules/ModuleLightsControl.h"

// 💫 HomeKit native support via HomeSpan (only compiled on boards with FT_HOMEKIT)
#if FT_ENABLED(FT_HOMEKIT)
  #include "HomeSpan.h"
  static void rgbToHsv(uint8_t r, uint8_t g, uint8_t b, float &h, float &s, float &v) {
    float rf=r/255.f, gf=g/255.f, bf=b/255.f;
    float mx=max(max(rf,gf),bf), mn=min(min(rf,gf),bf), d=mx-mn;
    v=mx; s=(mx>0)?d/mx:0;
    if(d==0){h=0;} else if(mx==rf){h=60.f*fmod((gf-bf)/d,6.f);} else if(mx==gf){h=60.f*((bf-rf)/d+2);} else{h=60.f*((rf-gf)/d+4);}
    if(h<0)h+=360.f;
  }
  static void hsvToRgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
    float c=v*s, x=c*(1.f-fabs(fmod(h/60.f,2.f)-1.f)), m=v-c, rf,gf,bf;
    if(h<60){rf=c;gf=x;bf=0;} else if(h<120){rf=x;gf=c;bf=0;} else if(h<180){rf=0;gf=c;bf=x;}
    else if(h<240){rf=0;gf=x;bf=c;} else if(h<300){rf=x;gf=0;bf=c;} else{rf=c;gf=0;bf=x;}
    r=(uint8_t)((rf+m)*255.5f); g=(uint8_t)((gf+m)*255.5f); b=(uint8_t)((bf+m)*255.5f);
  }
  class ModuleHomeAutomation;
  struct MoonLightBulb : Service::LightBulb {
    SpanCharacteristic *power, *brightness, *hue, *saturation;
    ModuleHomeAutomation *module;
    explicit MoonLightBulb(ModuleHomeAutomation *mod);
    boolean update() override;
  };
#endif

// 💫
class ModuleHomeAutomation : public Module {
 private:
  ModuleLightsControl *_lightsControl;
#if FT_ENABLED(FT_HOMEKIT)
  MoonLightBulb *_lightBulb = nullptr;
  bool _homeKitStarted = false;
#endif

 public:
  ModuleHomeAutomation(PsychicHttpServer *server, ESP32SvelteKit *sveltekit, ModuleLightsControl *lightsControl)
      : Module("homeautomation", server, sveltekit), _lightsControl(lightsControl) {}

  void begin() override {
    Module::begin();
#if FT_ENABLED(FT_HOMEKIT)
    if (_state.data["homeAutomation"] == 2) startHomeKit();
    _lightsControl->addUpdateHandler([this](const String &originId) { if (originId != "homekit") syncToHomeKit(); }, false);
#endif
  }

  void setupDefinition(const JsonArray &controls) override {
    JsonObject control = addControl(controls, "homeAutomation", "select");
    control["default"] = 0;
    JsonArray values = control["values"].to<JsonArray>();
    values.add("Off");
    values.add("Homebridge HTTP");
#if FT_ENABLED(FT_HOMEKIT)
    values.add("HomeKit Native");
#endif
  }

  void onUpdate(const UpdatedItem &updatedItem) override {
    if (updatedItem.name != "homeAutomation") return;
    uint8_t mode = _state.data["homeAutomation"];
    if (mode == 1)
      EXT_LOGI(ML_TAG, "Homebridge HTTP: configure plugin → http://%s/api/lightscontrol",
               _sveltekit->getWiFiSettingsService()->getHostname().c_str());
#if FT_ENABLED(FT_HOMEKIT)
    if (mode == 2) { if (!_homeKitStarted) startHomeKit(); else syncToHomeKit(); }
#endif
  }

#if FT_ENABLED(FT_HOMEKIT)
  void loop() override { if (_homeKitStarted) homeSpan.poll(); }

  void startHomeKit() {
    _homeKitStarted = true;
    String hn = _sveltekit->getWiFiSettingsService()->getHostname();
    homeSpan.setHostNameSuffix(""); homeSpan.setPortNum(1201);
    homeSpan.begin(Category::Lighting, hn.c_str());
    new SpanAccessory();
      new Service::AccessoryInformation();
        new Characteristic::Name(hn.c_str()); new Characteristic::Manufacturer("MoonModules");
        new Characteristic::SerialNumber("MoonLight"); new Characteristic::Model("MoonLight");
        new Characteristic::FirmwareRevision("1.0.0"); new Characteristic::Identify();
      _lightBulb = new MoonLightBulb(this);
    syncToHomeKit();
    EXT_LOGI(ML_TAG, "HomeKit started as \"%s\"", hn.c_str());
  }

  void syncToHomeKit() {
    if (!_lightBulb) return;
    _lightsControl->read([&](ModuleState &state) {
      float h, s, v;
      rgbToHsv(state.data["red"], state.data["green"], state.data["blue"], h, s, v);
      _lightBulb->power->setVal(state.data["lightsOn"].as<bool>());
      _lightBulb->brightness->setVal((int)(state.data["brightness"].as<uint8_t>() / 2.55f + 0.5f));
      _lightBulb->hue->setVal(h); _lightBulb->saturation->setVal(s * 100.f);
    }, "homekit-sync");
  }

  void applyFromHomeKit(bool on, int hkBri, float h, float s) {
    uint8_t r, g, b;
    hsvToRgb(h, s / 100.f, 1.f, r, g, b);
    JsonDocument doc; JsonObject ns = doc.to<JsonObject>();
    ns["lightsOn"]=on; ns["brightness"]=(uint8_t)(hkBri*2.55f+0.5f);
    ns["red"]=r; ns["green"]=g; ns["blue"]=b;
    _lightsControl->update(ns, ModuleState::update, "homekit");
  }
#endif
};  // class ModuleHomeAutomation

#if FT_ENABLED(FT_HOMEKIT)
MoonLightBulb::MoonLightBulb(ModuleHomeAutomation *mod) : Service::LightBulb(), module(mod) {
  power=new Characteristic::On(false); brightness=new Characteristic::Brightness(20);
  hue=new Characteristic::Hue(0); saturation=new Characteristic::Saturation(0);
}
boolean MoonLightBulb::update() {
  if (!module) return false;
  module->applyFromHomeKit(
    power->updated()      ? power->getNewVal<bool>()       : power->getVal<bool>(),
    brightness->updated() ? brightness->getNewVal<int>()   : brightness->getVal<int>(),
    hue->updated()        ? hue->getNewVal<float>()        : hue->getVal<float>(),
    saturation->updated() ? saturation->getNewVal<float>() : saturation->getVal<float>());
  return true;
}
#endif

#endif  // FT_MOONLIGHT
#endif  // ModuleHomeAutomation_h
