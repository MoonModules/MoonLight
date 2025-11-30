#include <EventSocket.h>

SemaphoreHandle_t clientSubscriptionsMutex = xSemaphoreCreateMutex();

EventSocket::EventSocket(PsychicHttpServer *server,
                         SecurityManager *securityManager,
                         AuthenticationPredicate authenticationPredicate) : _server(server),
                                                                            _securityManager(securityManager),
                                                                            _authenticationPredicate(authenticationPredicate)
{
}

void EventSocket::begin()
{
    _socket.setFilter(_securityManager->filterRequest(_authenticationPredicate));
    _socket.onOpen((std::bind(&EventSocket::onWSOpen, this, std::placeholders::_1)));
    _socket.onClose(std::bind(&EventSocket::onWSClose, this, std::placeholders::_1));
    _socket.onFrame(std::bind(&EventSocket::onFrame, this, std::placeholders::_1, std::placeholders::_2));
    _server->on(EVENT_SERVICE_PATH, &_socket);

    ESP_LOGV(SVK_TAG, "Registered event socket endpoint: %s", EVENT_SERVICE_PATH);
}

void EventSocket::registerEvent(String event)
{
    if (!isEventValid(event))
    {
        ESP_LOGD(SVK_TAG, "Registering event: %s", event.c_str());
        events.push_back(event);
    }
    else
    {
        ESP_LOGW(SVK_TAG, "Event already registered: %s", event.c_str());
    }
}

void EventSocket::onWSOpen(PsychicWebSocketClient *client)
{
    ESP_LOGI(SVK_TAG, "ws[%s][%u] connect", client->remoteIP().toString().c_str(), client->socket());
}

void EventSocket::onWSClose(PsychicWebSocketClient *client)
{
    // 🌙 adding semaphore wait too long logging
    if (xSemaphoreTake(clientSubscriptionsMutex, pdMS_TO_TICKS(200)) == pdFALSE) {
        ESP_LOGW(SVK_TAG, "clientSubscriptionsMutex wait too long");
        xSemaphoreTake(clientSubscriptionsMutex, portMAX_DELAY);
    }
    for (auto &event_subscriptions : client_subscriptions)
    {
        event_subscriptions.second.remove(client->socket());
    }
    xSemaphoreGive(clientSubscriptionsMutex);
    ESP_LOGI(SVK_TAG, "ws[%s][%u] disconnect", client->remoteIP().toString().c_str(), client->socket());
}

esp_err_t EventSocket::onFrame(PsychicWebSocketRequest *request, httpd_ws_frame *frame)
{
    ESP_LOGV(SVK_TAG, "ws[%s][%u] opcode[%d]", request->client()->remoteIP().toString().c_str(),
             request->client()->socket(), frame->type);

    JsonDocument doc;
#if FT_ENABLED(EVENT_USE_JSON)
    if (frame->type == HTTPD_WS_TYPE_TEXT)
    {
        ESP_LOGV(SVK_TAG, "ws[%s][%u] request: %s", request->client()->remoteIP().toString().c_str(),
                 request->client()->socket(), (char *)frame->payload);

        DeserializationError error = deserializeJson(doc, (char *)frame->payload, frame->len);
#else
    if (frame->type == HTTPD_WS_TYPE_BINARY)
    {
        ESP_LOGV(SVK_TAG, "ws[%s][%u] request: %s", request->client()->remoteIP().toString().c_str(),
                 request->client()->socket(), (char *)frame->payload);

        DeserializationError error = deserializeMsgPack(doc, (char *)frame->payload, frame->len);
#endif

        if (!error && doc.is<JsonObject>())
        {
            String event = doc["event"];
            if (event == "subscribe")
            {
                // only subscribe to events that are registered
                if (isEventValid(doc["data"].as<String>()))
                {
                    client_subscriptions[doc["data"]].push_back(request->client()->socket());
                    handleSubscribeCallbacks(doc["data"], String(request->client()->socket()));
                }
                else
                {
                    ESP_LOGW(SVK_TAG, "Client tried to subscribe to unregistered event: %s", doc["data"].as<String>().c_str());
                }
            }
            else if (event == "unsubscribe")
            {
                client_subscriptions[doc["data"]].remove(request->client()->socket());
            }
            else
            {
                JsonObject jsonObject = doc["data"].as<JsonObject>();
                handleEventCallbacks(event, jsonObject, request->client()->socket());
            }
            return ESP_OK;
        }
        ESP_LOGW(SVK_TAG, "Error[%d] parsing JSON: %s", error, (char *)frame->payload);
    }
    return ESP_OK;
}

void EventSocket::emitEvent(const String& event, const JsonObject &jsonObject, const char *originId, bool onlyToSameOrigin)
{
    JsonDocument doc;
    doc["event"] = event;
    doc["data"] = jsonObject;

    emitEvent(doc, originId, onlyToSameOrigin);
}

// 🌙 extracted from above function so the caller can prepare the JsonDocument, which saves on heap usage
void EventSocket::emitEvent(const JsonDocument &doc, const char *originId, bool onlyToSameOrigin)
{
    #if FT_ENABLED(EVENT_USE_JSON)
        static String outBuffer;      // reused across calls to avoid repeated allocation
        outBuffer.clear();            // keep capacity, reset length
        outBuffer.reserve(measureJson(doc)); // pre-reserve exact size (optional, improves speed for large JSON)
        serializeJson(doc, outBuffer);

        emitEvent(doc["event"], outBuffer.c_str(), outBuffer.length(), originId, onlyToSameOrigin);
    #else
        // --- MsgPack path ---
        struct VecWriter {
            std::vector<uint8_t> &v;
            size_t write(uint8_t c) {
                v.push_back(c);
                return 1;
            }
            size_t write(const uint8_t *buf, size_t size) {
                v.insert(v.end(), buf, buf + size);
                return size;
            }
        };

        static std::vector<uint8_t> outBuffer;
        outBuffer.clear();
        
        // 🌙 Check available heap before reserving to prevent crash on ESP32-D0
        size_t requiredSize = measureMsgPack(doc);
        size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        
        // Only reserve if we have enough memory + safety margin
        if (requiredSize > 0 && largestBlock > requiredSize + 4096) {
            try {
                outBuffer.reserve(requiredSize);
            } catch (const std::bad_alloc& e) {
                ESP_LOGW(SVK_TAG, "Failed to reserve %d bytes for MsgPack buffer (largest block: %d) %s %s", 
                         requiredSize, largestBlock, doc["event"].as<const char *>(), originId);
                // Continue anyway - vector will grow dynamically (slower but safer)
            }
        } else {
            ESP_LOGW(SVK_TAG, "Insufficient heap for MsgPack buffer: need %d, available %d %s %s", 
                     requiredSize, largestBlock, doc["event"].as<const char *>(), originId);
            // Skip reserve, let vector grow naturally
        }
        
        VecWriter writer{outBuffer};
        serializeMsgPack(doc, writer);

        emitEvent(doc["event"], (char *)outBuffer.data(), outBuffer.size(), originId, onlyToSameOrigin);
    #endif
}

// 🌙 extracted from above function for FT_MONITOR, which uses char *output
void EventSocket::emitEvent(const String& event, const char *output, size_t len, const char *originId, bool onlyToSameOrigin)
{
    // Only process valid events
    if (!isEventValid(event))
    {
        ESP_LOGW(SVK_TAG, "Method tried to emit unregistered event: %s (%d) from %s", event.c_str(), len, originId); // 🌙 
        return;
    }

    int originSubscriptionId = originId[0] ? atoi(originId) : -1;
    // 🌙 adding semaphore wait too long logging
    if (xSemaphoreTake(clientSubscriptionsMutex, pdMS_TO_TICKS(100)) == pdFALSE) {
        ESP_LOGW(SVK_TAG, "clientSubscriptionsMutex wait too long");
        xSemaphoreTake(clientSubscriptionsMutex, portMAX_DELAY);
    }
    auto &subscriptions = client_subscriptions[event];
    if (subscriptions.empty())
    {
        xSemaphoreGive(clientSubscriptionsMutex);
        return;
    }

    // if onlyToSameOrigin == true, send the message back to the origin
    if (onlyToSameOrigin && originSubscriptionId > 0)
    {
        auto *client = _socket.getClient(originSubscriptionId);
        if (client)
        {
            if (event != "monitor")
                ESP_LOGV(SVK_TAG, "Emitting event: %s to %s[%u], Message[%d]: %s", event.c_str(), client->remoteIP().toString().c_str(), client->socket(), len, output);
#if FT_ENABLED(EVENT_USE_JSON)
            client->sendMessage(HTTPD_WS_TYPE_TEXT, output, len);
#else
            client->sendMessage(HTTPD_WS_TYPE_BINARY, output, len);
#endif
        }
    }
    else
    { // else send the message to all other clients

        for (int subscription : client_subscriptions[event])
        {
            if (subscription == originSubscriptionId)
                continue;
            auto *client = _socket.getClient(subscription);
            if (!client)
            {
                subscriptions.remove(subscription);
                continue;
            }
            if (event != "monitor")
                ESP_LOGV(SVK_TAG, "Emitting event: %s to %s[%u], Message[%d]: %s", event.c_str(), client->remoteIP().toString().c_str(), client->socket(), len, output);
#if FT_ENABLED(EVENT_USE_JSON)
            client->sendMessage(HTTPD_WS_TYPE_TEXT, output, len);
#else
            client->sendMessage(HTTPD_WS_TYPE_BINARY, output, len);
#endif
        }
    }

    xSemaphoreGive(clientSubscriptionsMutex);
}

void EventSocket::handleEventCallbacks(String event, JsonObject &jsonObject, int originId)
{
    for (auto &callback : event_callbacks[event])
    {
        callback(jsonObject, originId);
    }
}

void EventSocket::handleSubscribeCallbacks(String event, const String &originId)
{
    for (auto &callback : subscribe_callbacks[event])
    {
        callback(originId);
    }
}

void EventSocket::onEvent(String event, EventCallback callback)
{
    if (!isEventValid(event))
    {
        ESP_LOGW(SVK_TAG, "Method tried to register unregistered event: %s", event.c_str());
        return;
    }
    event_callbacks[event].push_back(callback);
}

void EventSocket::onSubscribe(String event, SubscribeCallback callback)
{
    if (!isEventValid(event))
    {
        ESP_LOGW(SVK_TAG, "Method tried to subscribe to unregistered event: %s", event.c_str());
        return;
    }
    subscribe_callbacks[event].push_back(callback);
    ESP_LOGI(SVK_TAG, "onSubscribe for event: %s", event.c_str());
}

bool EventSocket::isEventValid(String event)
{
    return std::find(events.begin(), events.end(), event) != events.end();
}

unsigned int EventSocket::getConnectedClients()
{
    return (unsigned int)_socket.getClientList().size();
}
