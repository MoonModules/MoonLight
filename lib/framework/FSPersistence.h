#ifndef FSPersistence_h
#define FSPersistence_h

/**
 *   ESP32 SvelteKit
 *
 *   A simple, secure and extensible framework for IoT projects for ESP32 platforms
 *   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
 *   https://github.com/theelims/ESP32-sveltekit
 *
 *   Copyright (C) 2018 - 2023 rjwats
 *   Copyright (C) 2023 - 2025 theelims
 *
 *   All Rights Reserved. This software may be modified and distributed under
 *   the terms of the LGPL v3 license. See the LICENSE file for details.
 **/

#include <StatefulService.h>
#include <FS.h>

inline std::vector<std::function<void(char)>> delayedWrites; // 🌙 Global vector to store delayed write functions (not in FSPersistence as it has to be used for all T types)

template <class T>
class FSPersistence
{
public:
    FSPersistence(JsonStateReader<T> stateReader,
                  JsonStateUpdater<T> stateUpdater,
                  StatefulService<T> *statefulService,
                  FS *fs,
                  const char *filePath,
                  bool delayedWriting = false) // 🌙
                                        : _stateReader(stateReader),
                                          _stateUpdater(stateUpdater),
                                          _statefulService(statefulService),
                                          _fs(fs),
                                          _filePath(filePath),
                                          _updateHandlerId(0),
                                          _delayedWriting(delayedWriting) // 🌙
    {
        enableUpdateHandler();
    }

    void readFromFS()
    {
        File settingsFile = _fs->open(_filePath, "r");

        if (settingsFile)
        {
            JsonDocument jsonDocument;
            DeserializationError error = deserializeJson(jsonDocument, settingsFile);
            if (error == DeserializationError::Ok && jsonDocument.is<JsonObject>())
            {
                JsonObject jsonObject = jsonDocument.as<JsonObject>();
                _statefulService->updateWithoutPropagation(jsonObject, _stateUpdater, _filePath);
                settingsFile.close();
                return;
            }
            settingsFile.close();
        }

        // If we reach here we have not been successful in loading the config and hard-coded defaults are now applied.
        // The settings are then written back to the file system so the defaults persist between resets. This last step is
        // required as in some cases defaults contain randomly generated values which would otherwise be modified on reset.
        applyDefaults();
        writeToFS();
    }

    bool writeToFSNow() // 🌙
    {
        // create and populate a new json object
        JsonDocument jsonDocument;
        JsonObject jsonObject = jsonDocument.to<JsonObject>();
        _statefulService->read(jsonObject, _stateReader, _filePath); // 🌙 Add originId

        // make directories if required
        mkdirs();

        // serialize it to filesystem
        File settingsFile = _fs->open(_filePath, "w");

        // failed to open file, return false
        if (!settingsFile)
        {
            return false;
        }

        // serialize the data to the file
        serializeJson(jsonDocument, settingsFile);
        settingsFile.close();
        return true;
    }

    bool writeToFS() { // 🌙
        if (_delayedWriting) {
            if (!_hasDelayedWrite) {
                ESP_LOGD(SVK_TAG, "delayedWrites: Add %s", _filePath.c_str());
                delayedWrites.push_back([this](char writeOrCancel) {
                    ESP_LOGD(SVK_TAG, "delayedWrites: %c %s", writeOrCancel, this->_filePath.c_str());
                    if (writeOrCancel == 'W')
                        this->writeToFSNow(); 
                    else {
                        //write is cancelled, read the old situation back from FS
                        this->readFromFS();
                        // update state to UI
                        _statefulService->update([&](T &state) {
                            return StateUpdateResult::CHANGED; // notify StatefulService by returning CHANGED
                        }, SVK_TAG);
                    }
                    this->_hasDelayedWrite = false;
                });
                _hasDelayedWrite = true;
            }
            return true;
        } else {
            return writeToFSNow();
        }
    }

    static void writeToFSDelayed(char writeOrCancel) { // 🌙
        ESP_LOGD(SVK_TAG, "calling %d writeFuncs from delayedWrites", delayedWrites.size());
        for (auto& writeFunc : delayedWrites)
        {
            writeFunc(writeOrCancel);
        }
        delayedWrites.clear();
        // saveNeeded = false;
    }

    void disableUpdateHandler()
    {
        if (_updateHandlerId)
        {
            _statefulService->removeUpdateHandler(_updateHandlerId);
            _updateHandlerId = 0;
        }
    }

    void enableUpdateHandler()
    {
        if (!_updateHandlerId)
        {
            _updateHandlerId = _statefulService->addUpdateHandler([&](const String &originId)
                                                                  { writeToFS(); });
        }
    }

private:
    JsonStateReader<T> _stateReader;
    JsonStateUpdater<T> _stateUpdater;
    StatefulService<T> *_statefulService;
    FS *_fs;
    String _filePath;
    update_handler_id_t _updateHandlerId;
    bool _delayedWriting; // 🌙
    bool _hasDelayedWrite = false; // 🌙

    // We assume we have a _filePath with format "/directory1/directory2/filename"
    // We create a directory for each missing parent
    void mkdirs()
    {
        int index = 0;
        while ((index = _filePath.indexOf('/', index + 1)) != -1)
        {
            String segment = _filePath.substring(0, index);
            if (!_fs->exists(segment))
            {
                _fs->mkdir(segment);
            }
        }
    }

protected:
    // We assume the updater supplies sensible defaults if an empty object
    // is supplied, this virtual function allows that to be changed.
    virtual void applyDefaults()
    {
        JsonDocument jsonDocument;
        JsonObject jsonObject = jsonDocument.to<JsonObject>(); // 🌙: to instead of as
        _statefulService->updateWithoutPropagation(jsonObject, _stateUpdater, _filePath);
    }
};

#endif // end FSPersistence
