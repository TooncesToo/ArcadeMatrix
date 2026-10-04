#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <atomic>

namespace feed {

/**
 * The data feed's topic slots: which topic each of the SLOTS payload stores holds.
 *
 * One writer (the feed task) claims, frees and publishes; any number of readers look topics up
 * lock-free. Every change of a slot's occupant (freed, or reused for another topic) bumps that
 * slot's sequence, the same counter a new payload bumps, so a reader that copies under a sequence
 * check and re-checks the topic (holds()) never takes another topic's data for its own.
 *
 * Slots are freed by an empty payload (MQTT's way of deleting a retained message). When every
 * slot is taken, a new topic evicts the longest-unchanged topic that no screen references; if
 * every stored topic is referenced, the new topic is refused.
 */
class TopicTable {
public:
    static constexpr uint8_t SLOTS = 8;
    static constexpr size_t TOPIC_LEN = 96;

    /// Whether a configured screen uses `topic` (such a topic is never evicted).
    using RefFn = bool (*)(const char* topic, void* ctx);

    enum class Claim : uint8_t { Existing, New, Evicted, Full, TooLong };

    // ---- readers (lock-free) ----
    int find(const char* topic) const {
        if (!topic || !*topic) return -1;
        for (int i = 0; i < SLOTS; i++) {
            if (holds(i, topic)) return i;
        }
        return -1;
    }
    bool holds(int i, const char* topic) const {
        return i >= 0 && i < SLOTS && m_ready[i].load(std::memory_order_acquire) && strcmp(m_topic[i], topic) == 0;
    }
    uint32_t sequence(int i) const {
        return (i >= 0 && i < SLOTS) ? m_seq[i].load(std::memory_order_acquire) : 0;
    }
    uint8_t count() const { return m_count.load(std::memory_order_acquire); }

    // ---- writer (one task) ----
    /**
     * Slot for `topic`, claiming a free one (or evicting) when it is new. A New or Evicted slot is
     * returned NOT yet visible to readers: reset its payload state, then call activate().
     */
    int claim(const char* topic, uint32_t nowMs, RefFn isReferenced, void* ctx, Claim* how) {
        Claim dummy;
        Claim& h = how ? *how : dummy;
        if (!topic || strlen(topic) >= TOPIC_LEN) { h = Claim::TooLong; return -1; }
        int i = find(topic);
        if (i >= 0) { h = Claim::Existing; return i; }
        for (i = 0; i < SLOTS; i++) {
            if (!m_ready[i].load(std::memory_order_relaxed) && !m_pending[i]) { h = Claim::New; break; }
        }
        if (i == SLOTS) {
            i = evictionCandidate(nowMs, isReferenced, ctx);
            if (i < 0) { h = Claim::Full; return -1; }
            freeSlot(i);
            h = Claim::Evicted;
        }
        strncpy(m_topic[i], topic, TOPIC_LEN - 1);
        m_topic[i][TOPIC_LEN - 1] = '\0';
        m_lastUpdate[i] = nowMs;
        m_pending[i] = true;
        m_seq[i].fetch_add(1, std::memory_order_acq_rel);   // a reader still holding this index sees the change
        return i;
    }

    /// Makes a claimed slot visible to readers.
    void activate(int i) {
        if (i < 0 || i >= SLOTS || !m_pending[i]) return;
        m_pending[i] = false;
        m_ready[i].store(1, std::memory_order_release);
        m_count.fetch_add(1, std::memory_order_acq_rel);
    }

    /// Records a new payload in slot `i` (readers re-copy on the sequence change).
    void publish(int i, uint32_t nowMs) {
        if (i < 0 || i >= SLOTS) return;
        m_lastUpdate[i] = nowMs;
        m_seq[i].fetch_add(1, std::memory_order_acq_rel);
    }

    /// Empty payload on `topic`: frees its slot. Returns the freed slot, or -1 if it was not stored.
    int release(const char* topic) {
        int i = find(topic);
        if (i >= 0) freeSlot(i);
        return i;
    }

    /// Unreferenced slot with the oldest update (by age, so millis() wrap is harmless), or -1.
    int evictionCandidate(uint32_t nowMs, RefFn isReferenced, void* ctx) const {
        int best = -1;
        uint32_t bestAge = 0;
        for (int i = 0; i < SLOTS; i++) {
            if (!m_ready[i].load(std::memory_order_relaxed)) continue;
            if (isReferenced && isReferenced(m_topic[i], ctx)) continue;
            uint32_t age = nowMs - m_lastUpdate[i];
            if (best < 0 || age > bestAge) { best = i; bestAge = age; }
        }
        return best;
    }

    const char* topicAt(int i) const { return (i >= 0 && i < SLOTS) ? m_topic[i] : ""; }

private:
    void freeSlot(int i) {
        if (m_ready[i].load(std::memory_order_relaxed)) {
            m_ready[i].store(0, std::memory_order_release);
            m_count.fetch_sub(1, std::memory_order_acq_rel);
        }
        m_pending[i] = false;
        m_seq[i].fetch_add(1, std::memory_order_acq_rel);
    }

    char m_topic[SLOTS][TOPIC_LEN] = {};
    std::atomic<uint8_t> m_ready[SLOTS] = {};
    std::atomic<uint32_t> m_seq[SLOTS] = {};
    std::atomic<uint8_t> m_count{0};
    uint32_t m_lastUpdate[SLOTS] = {};   ///< writer only
    bool m_pending[SLOTS] = {};          ///< claimed, not yet activated (writer only)
};

}  // namespace feed
