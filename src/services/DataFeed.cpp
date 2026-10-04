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

feed::Kind DataFeed::kindOf(int slot) const {
    if (slot < 0 || slot >= MAX_TOPICS) return feed::Kind::None;
    const Slot& s = m_slots[slot];
    return (feed::Kind)s.kinds[s.idx.load(std::memory_order_acquire)].load(std::memory_order_acquire);
}

template <typename T>
bool DataFeed::copyKind(int slot, const char* topic, feed::Kind kind, T* const Slot::*arr, T& out, uint32_t& seqOut) const {
    if (slot < 0 || slot >= MAX_TOPICS) return false;
    const Slot& s = m_slots[slot];
    // Double buffer plus a sequence check, no lock: Core 0 only ever writes the buffer that is not
    // published and bumps the sequence after each publication, and also whenever the slot is freed
    // or given to another topic. Checking the topic inside that window means a copy taken while the
    // slot changed hands is discarded, never shown under the wrong topic.
    uint32_t before = m_table.sequence(slot);
    if (!(s.*arr) || !m_table.holds(slot, topic)) return false;
    uint8_t idx = s.idx.load(std::memory_order_acquire);
    if (s.kinds[idx].load(std::memory_order_acquire) != (uint8_t)kind) return false;
    memcpy(&out, &(s.*arr)[idx], sizeof(T));
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t after = m_table.sequence(slot);
    if (before != after) return false;
    seqOut = after;
    return true;
}

bool DataFeed::copyGraph(int slot, const char* topic, graph::GraphData& out, uint32_t& seqOut) const {
    return copyKind(slot, topic, feed::Kind::Graph, &Slot::graphs, out, seqOut);
}

bool DataFeed::copyValues(int slot, const char* topic, feed::ValuesData& out, uint32_t& seqOut) const {
    return copyKind(slot, topic, feed::Kind::Values, &Slot::values, out, seqOut);
}

bool DataFeed::copyWeather(int slot, const char* topic, feed::WeatherFeed& out, uint32_t& seqOut) const {
    return copyKind(slot, topic, feed::Kind::Weather, &Slot::weather, out, seqOut);
}

// Feed task only (Core 0): does a configured screen read this topic? Graph and Values screens by
// their `topic`, weather screens only with the Home Assistant source. Such topics are never evicted.
bool DataFeed::topicReferenced(const char* topic, void* ctx) {
    (void)ctx;
    ConfigSnapshotGuard guard = config.acquireSnapshot();
    for (const auto& inst : guard->instances) {
        const bool feedScreen = inst.engine_id == "graph" || inst.engine_id == "values" ||
                                (inst.engine_id == "weather" && inst.config.getString("source", "owm").equalsIgnoreCase("ha"));
        if (!feedScreen) continue;
        const char* def = inst.engine_id == "graph" ? "arcadematrix/graph/ac"
                        : inst.engine_id == "values" ? "arcadematrix/values/energy" : "arcadematrix/weather/local";
        String t = inst.config.getString("topic", def);
        t.trim();
        if (t == topic) return true;
    }
    return false;
}

void DataFeed::onMessage(char* topic, uint8_t* payload, unsigned int length) {
    if (s_instance) s_instance->handleMessage(topic, payload, length);
}

void DataFeed::handleMessage(const char* topic, const uint8_t* payload, unsigned int length) {
    // An empty payload deletes a retained message: forget the topic and free its slot. Screens on it
    // see the sequence change, find the slot gone and go back to their no-data state.
    if (length == 0) {
        if (m_table.release(topic) >= 0) LOGI("DataFeed", "topic %s cleared; %u in use", topic, (unsigned)m_table.count());
        return;
    }
    if (length > MAX_PAYLOAD) {
        if (!m_warnedSize) {
            LOGW("DataFeed", "payload on %s is %u bytes; the limit is %u, dropped", topic, length, (unsigned)MAX_PAYLOAD);
            m_warnedSize = true;
        }
        return;
    }
    feed::TopicTable::Claim how;
    const char* evicted = nullptr;
    char evictedTopic[MAX_TOPIC_LEN];
    {
        // Name of the slot that would be evicted, for the log line (claim() overwrites it).
        int victim = m_table.find(topic) < 0 && m_table.count() >= MAX_TOPICS
                   ? m_table.evictionCandidate(millis(), topicReferenced, nullptr) : -1;
        if (victim >= 0) { strlcpy(evictedTopic, m_table.topicAt(victim), sizeof(evictedTopic)); evicted = evictedTopic; }
    }
    const int slot = m_table.claim(topic, millis(), topicReferenced, nullptr, &how);
    if (slot < 0) {
        if (strcmp(m_lastRefused, topic) != 0) {   // once per topic, not on every retained repeat
            strlcpy(m_lastRefused, topic, sizeof(m_lastRefused));
            if (how == feed::TopicTable::Claim::TooLong) LOGW("DataFeed", "topic %s is too long; ignored", topic);
            else LOGW("DataFeed", "no room for topic %s: all %u topics are used by screens", topic, (unsigned)MAX_TOPICS);
        }
        return;
    }
    if (how == feed::TopicTable::Claim::New || how == feed::TopicTable::Claim::Evicted) {
        // A fresh occupant starts with no payload, so nothing of the previous topic can show.
        m_slots[slot].kinds[0].store((uint8_t)feed::Kind::None, std::memory_order_release);
        m_slots[slot].kinds[1].store((uint8_t)feed::Kind::None, std::memory_order_release);
        m_table.activate(slot);
        if (how == feed::TopicTable::Claim::Evicted && evicted) LOGI("DataFeed", "topic %s evicted for %s", evicted, topic);
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
    m_table.publish(slot, millis());
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
