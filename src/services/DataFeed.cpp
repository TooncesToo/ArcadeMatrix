#include "DataFeed.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_heap_caps.h>
#include "../core/ConfigLoader.h"
#include "../core/Logger.h"
#include "../core/SpiRamJsonDocument.h"
#include "../hal/HardwareHAL.h"

extern ConfigLoader config;

DataFeed dataFeed;
DataFeed* DataFeed::s_instance = nullptr;

namespace {
// Scratch for parsing payloads in place; PSRAM, allocated with the slot storage.
char* s_parseBuf = nullptr;
SpiRamJsonDocument* s_doc = nullptr;
// Room for a 12 KB packet, so a payload over MAX_PAYLOAD still arrives and can be reported
// instead of being dropped silently by the client library. Above the PSRAM malloc threshold, so
// the library's buffer is placed in PSRAM rather than internal DRAM.
constexpr uint16_t kPacketBuffer = 12288;
constexpr uint32_t kBackoffStartMs = 5000;
constexpr uint32_t kBackoffMaxMs = 60000;

bool sameConfig(const DataMqttConfig& a, const DataMqttConfig& b) {
    return a.enabled == b.enabled && a.broker == b.broker && a.port == b.port && a.user == b.user &&
           a.pass == b.pass && a.topicPrefix == b.topicPrefix;
}
}  // namespace

DataFeed::~DataFeed() {
    if (s_instance == this) s_instance = nullptr;
}

void DataFeed::begin() {
    if (m_task || m_unsupported) return;
    if (!hardwareHAL.capabilities().hasPsram) {
        m_unsupported = true;   // say it once, not on every config change
        // Its payload store lives in PSRAM; without it the task would only hold a stack for nothing.
        LOGI("DataFeed", "no PSRAM on this board; the data feed MQTT link is not available");
        return;
    }
    s_instance = this;
    // Core 0, like the other network workers; the render loop on Core 1 never waits on it.
    if (xTaskCreatePinnedToCore(taskEntry, "data_feed", 6144, this, 1, &m_task, 0) != pdPASS) {
        m_task = nullptr;
        LOGW("DataFeed", "task did not start; the data feed is off");
    }
}

bool DataFeed::allocateStorage() {
    if (m_storageReady) return true;
    if (m_allocFailed || !hardwareHAL.capabilities().hasPsram) return false;
    m_allocFailed = true;   // one attempt only, so a failure cannot leak on every retry
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    for (auto& s : m_slots) {
        s.graphs = (graph::GraphData*)heap_caps_calloc(2, sizeof(graph::GraphData), caps);
        s.values = (feed::ValuesData*)heap_caps_calloc(2, sizeof(feed::ValuesData), caps);
        s.weather = (feed::WeatherFeed*)heap_caps_calloc(2, sizeof(feed::WeatherFeed), caps);
        if (!s.graphs || !s.values || !s.weather) return false;
        s.kinds[0].store(0, std::memory_order_relaxed);
        s.kinds[1].store(0, std::memory_order_relaxed);
    }
    s_parseBuf = (char*)heap_caps_malloc(MAX_PAYLOAD + 1, caps);
    // ~1,200 numbers fill an 8 KB payload; 48 KB of pool covers that with room for the strings.
    s_doc = new SpiRamJsonDocument(49152);
    if (!s_parseBuf || !s_doc || s_doc->capacity() == 0) return false;
    m_allocFailed = false;
    m_storageReady = true;
    return true;
}

int DataFeed::findTopic(const char* topic) const {
    if (!topic || !*topic) return -1;
    for (int i = 0; i < MAX_TOPICS; i++) {
        if (m_slots[i].ready.load(std::memory_order_acquire) && strcmp(m_slots[i].topic, topic) == 0) return i;
    }
    return -1;
}

uint32_t DataFeed::sequence(int slot) const {
    if (slot < 0 || slot >= MAX_TOPICS) return 0;
    return m_slots[slot].seq.load(std::memory_order_acquire);
}

feed::Kind DataFeed::kindOf(int slot) const {
    if (slot < 0 || slot >= MAX_TOPICS) return feed::Kind::None;
    const Slot& s = m_slots[slot];
    if (s.seq.load(std::memory_order_acquire) == 0) return feed::Kind::None;
    return (feed::Kind)s.kinds[s.idx.load(std::memory_order_acquire)].load(std::memory_order_acquire);
}

template <typename T>
bool DataFeed::copyKind(int slot, feed::Kind kind, T* const Slot::*arr, T& out, uint32_t& seqOut) const {
    if (slot < 0 || slot >= MAX_TOPICS) return false;
    const Slot& s = m_slots[slot];
    // Double buffer plus a sequence check, no lock: Core 0 only ever writes the buffer that is not
    // published, and bumps the sequence after each publication. If the sequence moved while we
    // copied, the writer may have reached the buffer we were reading, so the copy is discarded.
    uint32_t before = s.seq.load(std::memory_order_acquire);
    if (before == 0 || !(s.*arr)) return false;
    uint8_t idx = s.idx.load(std::memory_order_acquire);
    if (s.kinds[idx].load(std::memory_order_acquire) != (uint8_t)kind) return false;
    memcpy(&out, &(s.*arr)[idx], sizeof(T));
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t after = s.seq.load(std::memory_order_acquire);
    if (before != after) return false;
    seqOut = after;
    return true;
}

bool DataFeed::copyGraph(int slot, graph::GraphData& out, uint32_t& seqOut) const {
    return copyKind(slot, feed::Kind::Graph, &Slot::graphs, out, seqOut);
}

bool DataFeed::copyValues(int slot, feed::ValuesData& out, uint32_t& seqOut) const {
    return copyKind(slot, feed::Kind::Values, &Slot::values, out, seqOut);
}

bool DataFeed::copyWeather(int slot, feed::WeatherFeed& out, uint32_t& seqOut) const {
    return copyKind(slot, feed::Kind::Weather, &Slot::weather, out, seqOut);
}

int DataFeed::claimSlot(const char* topic) {
    if (strlen(topic) >= MAX_TOPIC_LEN) return -1;
    for (int i = 0; i < MAX_TOPICS; i++) {
        Slot& s = m_slots[i];
        if (s.ready.load(std::memory_order_relaxed)) continue;
        strncpy(s.topic, topic, MAX_TOPIC_LEN - 1);
        s.topic[MAX_TOPIC_LEN - 1] = '\0';
        s.ready.store(1, std::memory_order_release);   // topic is final before anyone matches it
        m_topicCount.fetch_add(1, std::memory_order_release);
        return i;
    }
    return -1;
}

void DataFeed::onMessage(char* topic, uint8_t* payload, unsigned int length) {
    if (s_instance) s_instance->handleMessage(topic, payload, length);
}

void DataFeed::handleMessage(const char* topic, const uint8_t* payload, unsigned int length) {
    if (length > MAX_PAYLOAD) {
        if (!m_warnedSize) {
            LOGW("DataFeed", "payload on %s is %u bytes; the limit is %u, dropped", topic, length, (unsigned)MAX_PAYLOAD);
            m_warnedSize = true;
        }
        return;
    }
    int slot = findTopic(topic);
    if (slot < 0) slot = claimSlot(topic);
    if (slot < 0) {
        if (!m_warnedFull) {
            LOGW("DataFeed", "no room for topic %s (%u topics max)", topic, (unsigned)MAX_TOPICS);
            m_warnedFull = true;
        }
        return;
    }
    Slot& s = m_slots[slot];

    // Parse into the buffer that is not published. The full fence keeps these writes behind the
    // sequence bump of the previous publication, which is what copyKind() relies on.
    memcpy(s_parseBuf, payload, length);
    s_parseBuf[length] = '\0';
    s_doc->clear();
    if (deserializeJson(*s_doc, s_parseBuf) != DeserializationError::Ok) {
        LOGD("DataFeed", "payload on %s is not JSON; ignored", topic);
        return;
    }
    const feed::Kind kind = feed::detectKind(*s_doc);
    const uint8_t next = s.idx.load(std::memory_order_relaxed) ^ 1;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    bool ok = false;
    switch (kind) {
        case feed::Kind::Graph:   ok = graph::parseDocument(*s_doc, s.graphs[next]); break;
        case feed::Kind::Values:  ok = feed::parseValues(*s_doc, s.values[next]); break;
        case feed::Kind::Weather: ok = feed::parseWeather(*s_doc, s.weather[next]); break;
        default: break;
    }
    if (!ok) return;
    s.kinds[next].store((uint8_t)kind, std::memory_order_release);
    s.idx.store(next, std::memory_order_release);
    s.seq.fetch_add(1, std::memory_order_acq_rel);
    LOGD("DataFeed", "payload kind %u on %s", (unsigned)kind, topic);
}

void DataFeed::taskEntry(void* arg) {
    static_cast<DataFeed*>(arg)->run();
}

void DataFeed::run() {
    WiFiClient net;
    PubSubClient client(net);
    client.setCallback(onMessage);
    client.setKeepAlive(30);
    client.setSocketTimeout(5);
    bool bufferSized = false;

    DataMqttConfig active;
    bool haveActive = false;
    uint32_t seenVersion = 0xFFFFFFFFu;
    uint32_t backoffMs = kBackoffStartMs;
    uint32_t nextAttemptMs = 0;
    bool warnedNoStorage = false;
    char clientId[80] = {0};
    char subTopic[MAX_TOPIC_LEN + 4] = {0};

    for (;;) {
        // Follow the config: a changed broker, login or prefix reconnects; disabling disconnects.
        uint32_t ver = config.getVersion();
        if (ver != seenVersion) {
            seenVersion = ver;
            DataMqttConfig want;
            String host;
            {
                ConfigSnapshotGuard guard = config.acquireSnapshot();
                want = guard->dataMqtt;
                host = guard->wifi.hostname.length() ? guard->wifi.hostname : guard->mqtt.deviceName;
            }
            if (!haveActive || !sameConfig(want, active)) {
                if (client.connected()) client.disconnect();
                m_connected.store(false, std::memory_order_release);
                active = want;
                haveActive = true;
                backoffMs = kBackoffStartMs;
                nextAttemptMs = millis();
                // Unique per sign (two signs share one broker), and distinct from the arcade link.
                snprintf(clientId, sizeof(clientId), "%s-data", host.length() ? host.c_str() : "arcadematrix");
                snprintf(subTopic, sizeof(subTopic), "%s/#", active.topicPrefix.c_str());
                // PubSubClient keeps the pointer, and `active` outlives it inside this loop.
                client.setServer(active.broker.c_str(), (uint16_t)active.port);
            }
        }

        if (!active.enabled || active.broker.isEmpty()) {
            if (client.connected()) client.disconnect();
            m_connected.store(false, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!allocateStorage()) {
            if (!warnedNoStorage) {
                LOGW("DataFeed", "could not allocate the payload store in PSRAM; the data feed stays off");
                warnedNoStorage = true;
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (!bufferSized) bufferSized = client.setBufferSize(kPacketBuffer);
        if (WiFi.status() != WL_CONNECTED) {
            m_connected.store(false, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!client.connected()) {
            m_connected.store(false, std::memory_order_release);
            if ((int32_t)(millis() - nextAttemptMs) >= 0) {
                bool ok = active.user.length()
                    ? client.connect(clientId, active.user.c_str(), active.pass.c_str())
                    : client.connect(clientId);
                if (ok && client.subscribe(subTopic)) {
                    LOGI("DataFeed", "connected to %s:%d as %s, subscribed to %s",
                         active.broker.c_str(), active.port, clientId, subTopic);
                    backoffMs = kBackoffStartMs;
                    m_connected.store(true, std::memory_order_release);
                } else {
                    LOGW("DataFeed", "connect to %s:%d failed (state %d); retry in %u s",
                         active.broker.c_str(), active.port, client.state(), (unsigned)(backoffMs / 1000));
                    if (ok) client.disconnect();
                    nextAttemptMs = millis() + backoffMs;
                    backoffMs = backoffMs * 2 > kBackoffMaxMs ? kBackoffMaxMs : backoffMs * 2;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        client.loop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
