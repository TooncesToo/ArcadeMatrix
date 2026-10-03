#pragma once
#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../engines/graph/GraphPayload.h"

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
 * (retained messages refill them after every reconnect). A payload that is a graph is also parsed
 * into a double buffer that Core 1 copies lock-free (see copyGraph()). Storage lives in PSRAM and is
 * allocated once, the first time the feed is enabled; boards without PSRAM do not run the feed.
 */
class DataFeed {
public:
    static constexpr uint8_t MAX_TOPICS = 8;
    static constexpr size_t MAX_PAYLOAD = 4096;
    static constexpr size_t MAX_TOPIC_LEN = 96;

    DataFeed() = default;
    ~DataFeed();

    /// Starts the Core 0 task (once; later calls are no-ops). Safe before Wi-Fi is up, the task
    /// waits for it. Called when the feed is first enabled, so an unused feed costs no stack.
    void begin();

    bool isConnected() const { return m_connected.load(std::memory_order_acquire); }
    uint8_t topicCount() const { return m_topicCount.load(std::memory_order_acquire); }

    /// Slot holding `topic`, or -1. Lock-free; safe on Core 1.
    int findTopic(const char* topic) const;

    /// Graph publication counter for a slot (0 = no graph received yet). Lock-free.
    uint32_t graphSequence(int slot) const;

    /**
     * Copies the slot's latest graph into `out`. Lock-free: returns false (and `out` must be
     * ignored) if a new graph was published while copying; try again on a later frame.
     */
    bool copyGraph(int slot, graph::GraphData& out, uint32_t& seqOut) const;

private:
    struct Slot {
        std::atomic<uint8_t> ready{0};          ///< topic[] is set and stable
        char topic[MAX_TOPIC_LEN];
        char* raw = nullptr;                    ///< last payload, NUL-terminated (Core 0 only)
        uint16_t rawLen = 0;
        graph::GraphData* graphs = nullptr;     ///< [2], double buffer
        std::atomic<uint8_t> graphIdx{0};
        std::atomic<uint32_t> graphSeq{0};
    };

    Slot m_slots[MAX_TOPICS];
    std::atomic<bool> m_connected{false};
    std::atomic<uint8_t> m_topicCount{0};
    TaskHandle_t m_task = nullptr;
    bool m_storageReady = false;
    bool m_allocFailed = false;
    bool m_unsupported = false;
    bool m_warnedFull = false;
    bool m_warnedSize = false;

    bool allocateStorage();
    static void taskEntry(void* arg);
    void run();
    static void onMessage(char* topic, uint8_t* payload, unsigned int length);
    void handleMessage(const char* topic, const uint8_t* payload, unsigned int length);
    int claimSlot(const char* topic);

    static DataFeed* s_instance;
};

extern DataFeed dataFeed;
