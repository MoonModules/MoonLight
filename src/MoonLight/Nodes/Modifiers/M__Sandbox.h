/**
    @title     MoonLight
    @file      M__Sandbox.h (Modifier sandbox)
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/


#if FT_MOONLIGHT

// example template, do not remove
// add effects in ModuleEffects::addNodes()
class NewModifier: public Node {
  public:

  static const char * name() {return "New Modifier";}
  static uint8_t dim() {return _3D;} // for which effect dimension this modifier can be used, preferably 3D
  static const char * tags() {return "💎";} // use emojis see https://moonmodules.org/MoonLight/moonlight/overview/#emoji-coding, 💎 for modifier

  bool mirror = true;
  
  void setup() override {
    // controls will show in the UI
    // for different type of controls see other Nodes
    addControl(mirror, "mirror", "checkbox");
  }

  Coord3D modifierSize; //store modified size for use in modifyPosition and modifyXYZ, useful for multiple modifiers
  
  bool hasModifier() const override { return true; } // so the mapping system knows this node is a modifier

  //modify the (virtual) size during mapping
  void modifySize() override {
    layerV->size/=2;
    modifierSize = layerV->size;
    MB_LOGV(ML_TAG, "%d %d %d", layerV->size.x, layerV->size.y, layerV->size.z);
  }

  //modify the position of each light during mapping
  void modifyPosition(Coord3D &position) override {
    position = modifierSize * 2 - 1 - position;
  }

  //modify the position of each light on each frame
  void modifyXYZ(Coord3D &position) override {
    position = modifierSize - position;
  }

};

#endif