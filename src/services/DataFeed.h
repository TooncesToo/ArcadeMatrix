#pragma once
#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../engines/graph/GraphPayload.h"
#include "../engines/graph/FeedPayloads.h"
#include "../engines/graph/TopicTable.h"

/**
 * Data feed: a second MQTT connection, separate from the arcade Frontend Sync one, that only
 * receives data for engines to draw (for example a Home Assistant automation publishing a graph).
 * It never takes over the display.
 *
 * Everything network-side runs on a task of its own on Core 0: connect, reconnect with backoff,
 * receive, and parse graph payloads. The task follows the `data_mqtt` config by itself, so a
 * change made in the web UI applies without a reboot.
 *
 * The client subscribes to `<topic_prefix>/#` and keeps the last payload of up to MAX_TOPICS topics
 * (retained messages refill them after every reconnect), parsed by kind - graph, values or weather
 * (feed::detectKind) - into a per-topic double buffer that readers copy lock-free (copyGraph(),
 * copyValues(), copyWeather()). Storage lives in PSRAM and is allocated once, the first time the
 * feed is enabled; boards without PSRAM do not run the feed.
 */
class DataFeed {
public:
    static constexpr uint8_t MAX_TOPICS = feed::TopicTable::SLOTS;
    static constexpr size_t MAX_PAYLOAD = 8192;
    static constexpr size_t MAX_TOPIC_LEN = feed::TopicTable::TOPIC_LEN;

    DataFeed() = default;
    ~DataFeed();

    /// Starts the Core 0 task (once; later calls are no-ops). Safe before Wi-Fi is up, the task
    /// waits for it. Called when the feed is first enabled, so an unused feed costs no stack.
    void begin();

    bool isConnected() const { return m_connected.load(std::memory_order_acquire); }
    uint8_t topicCount() const { return m_table.count(); }

    /// Slot holding `topic`, or -1. Lock-free; safe on Core 1.
    int findTopic(const char* topic) const { return m_table.find(topic); }

    /// Whether slot `slot` still holds `topic`. A slot is freed by an empty payload and may be
    /// evicted for a new topic, so a reader that cached a slot index checks this before trusting it.
    bool holds(int slot, const char* topic) const { return m_table.holds(slot, topic); }

    /// Change counter for a slot: bumped by every payload and every change of occupant. Lock-free.
    uint32_t sequence(int slot) const { return m_table.sequence(slot); }

    /// Kind of the slot's latest payload (Kind::None before the first one). Lock-free.
    feed::Kind kindOf(int slot) const;

    /**
     * Copy the slot's latest payload of that kind into `out`, provided the slot still holds `topic`.
     * Lock-free: false when the slot holds another topic or kind, nothing yet, or changed while
     * copying (then `out` must be ignored; try again on a later frame).
     */
    bool copyGraph(int slot, const char* topic, graph::GraphData& out, uint32_t& seqOut) const;
    bool copyValues(int slot, const char* topic, feed::ValuesData& out, uint32_t& seqOut) const;
    bool copyWeather(int slot, const char* topic, feed::WeatherFeed& out, uint32_t& seqOut) const;

private:
    /// Payload storage per topic slot (which topic a slot holds lives in m_table).
    struct Slot {
        graph::GraphData* graphs = nullptr;     ///< [2], double buffers per kind
        feed::ValuesData* values = nullptr;
        feed::WeatherFeed* weather = nullptr;
        std::atomic<uint8_t> kinds[2];          ///< feed::Kind held by each buffer index
        std::atomic<uint8_t> idx{0};            ///< published buffer index
    };

    template <typename T>
    bool copyKind(int slot, const char* topic, feed::Kind kind, T* const Slot::*arr, T& out, uint32_t& seqOut) const;

    feed::TopicTable m_table;
    Slot m_slots[MAX_TOPICS];
    std::atomic<bool> m_connected{false};
    char m_lastRefused[MAX_TOPIC_LEN] = {0};   ///< feed task only: log a refused topic once
    TaskHandle_t m_task = nullptr;
    bool m_storageReady = false;
    bool m_allocFailed = false;
    bool m_unsupported = false;
    bool m_warnedSize = false;

    bool allocateStorage();
    static void taskEntry(void* arg);
    void run();
    static void onMessage(char* topic, uint8_t* payload, unsigned int length);
    void handleMessage(const char* topic, const uint8_t* payload, unsigned int length);
    static bool topicReferenced(const char* topic, void* ctx);

    static DataFeed* s_instance;
};

extern DataFeed dataFeed;
