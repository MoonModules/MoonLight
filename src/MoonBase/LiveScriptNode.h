/**
    @title     MoonLight
    @file      LiveScriptNode.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_LIVESCRIPT

  #include "Nodes.h"

/// Node subclass that compiles and runs ESPLiveScript (.sc) files on the ESP32.
/// Scripts can define setup(), loop(), onLayout(), and modifyPosition() functions
/// which are called through the LiveScript runtime. Guarded by FT_LIVESCRIPT.
class LiveScriptNode : public Node {
 public:
  static const char* name() { return "LiveScriptNode"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "⚙️"; }

  bool hasSetupFunction = false;     ///< True if the script defines a setup() function
  bool hasLoopFunction = false;      ///< True if the script defines a loop() function
  bool hasModifyFunction = false;    ///< True if the script defines a modifyPosition() function
  bool hasOnLayoutFunction = false;  ///< True if the script defines an onLayout() function

  bool isLiveScriptNode() const override { return true; }
  bool hasModifier() const override { return hasModifyFunction; }
  bool hasOnLayout() const override { return hasOnLayoutFunction; }

  const char* animation = nullptr;  ///< Path to the .sc script file on ESPFS

  /// Registers external functions/variables with the LiveScript runtime, then compiles and runs the script.
  void setup() override;

  /// Signals the running script to continue its next animation frame via semaphore.
  void loop() override;

  /// Calls the script's onLayout() function if it exists, for layout mapping.
  void onLayout() override;

  /// Kills the running script on destruction.
  ~LiveScriptNode() override;

  /// Reads the .sc file from ESPFS, parses it, and starts execution.
  void compileAndRun();
  /// Requests mappings and starts script execution (as task if loop exists, synchronous otherwise).
  void execute();
  /// Kills the running script instance.
  void kill();
  /// Frees the script runtime resources.
  void free();
  /// Kills the script and deletes its executable from the runtime.
  void killAndDelete();
  /// Populates a JsonArray with info about all running LiveScript executables.
  static void getScriptsJson(JsonArray scripts);
};

#endif  // FT_LIVESCRIPT
