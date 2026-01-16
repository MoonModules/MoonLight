/**
    @title     MoonBase
    @file      FileManager.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/FileManager/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

// * filesState: all files on FS
// * folderList: all files in a folder
// * editableFile: current file
// * getState / postFilesState: get filesState and post changes to files (update, delete, new)
// * addFile / addFolder: create new items
// * breadcrumbs(String): folder path as string array and as string, stored in localStorage
// * folderListFromBreadCrumbs: create folderList of current folder
// * handleEdit: when edit button pressed: navigate back and forward through folders, edit current file
// * confirmDelete: when delete button pressed
// * socket files / handleFileState (->folderListFromBreadCrumbs)
// Using component FileManager, FileEditWidget and EditRowWidget, see [Components](https://moonmodules.org/MoonLight/components/#FileEditWidget)

#ifndef FileManager_h
#define FileManager_h

#if FT_MOONBASE == 1

  #include <ESP32SvelteKit.h>
  #include <EventEndpoint.h>
  #include <EventSocket.h>
  #include <HttpEndpoint.h>
  #include <PsychicHttp.h>
  #include <WebSocketServer.h>

class FilesState {
 public:
  std::vector<String> updatedItems;
  bool showHidden = false;

  static void read(FilesState& settings, JsonObject& stateJson);

  static StateUpdateResult update(JsonObject& newData, FilesState& filesState, const String &originId);
};

class FileManager : public StatefulService<FilesState> {
 public:
  FileManager(PsychicHttpServer* server, ESP32SvelteKit* sveltekit);

  void begin();

 protected:
  EventSocket* _socket;

 private:
  HttpEndpoint<FilesState> _httpEndpoint;
  EventEndpoint<FilesState> _eventEndpoint;
  WebSocketServer<FilesState> _webSocketServer;
  PsychicHttpServer* _server;
  ESP32SvelteKit* _sveltekit;
};

#endif
#endif
