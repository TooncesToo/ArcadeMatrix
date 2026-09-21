#pragma once
#include <Arduino.h>
#include <stdint.h>

/**
 * @brief Play statistics for the GIF library: how often and when each file was shown.
 *
 * Neither firmware remembered anything beyond the last file played, so "it keeps showing the same
 * ones" could not be checked. This keeps one entry per played file, keyed by a hash of its name
 * inside its folder, in PSRAM. The render path (GifEngine, Core 1) only calls recordPlay(), which
 * hashes the path and bumps a counter: no String, no heap, no SD.
 *
 * A Core 0 task does everything that touches the card, under bounded SD holds (Golden Rule #5):
 * loading each folder's stats.txt once, writing dirty folders back at most every 15 minutes (joined
 * against index.txt so entries for deleted files fall away), and honouring a reset. Files live next
 * to index.txt as `<root>/<folder>/stats.txt`, one "name<TAB>count<TAB>last_epoch" line per file, so
 * a rescan does not disturb them and they are readable by hand.
 */
namespace GifStats {
    struct Entry { uint32_t nameHash; uint16_t count; uint32_t last; };

    /// Start the Core 0 keeper task; call once after the card is mounted.
    void start();
    /// Record that `fullPath` ("/gifs/<folder>/<name>" or "/gifs_tate/...") started playing. Hot-path safe.
    void recordPlay(const char* fullPath);
    /// FNV-1a of a file name, the key used everywhere.
    uint32_t hashName(const char* name);

    /// Snapshot of one folder for the API: entries (count > 0 only) copied into `out`, capped at `max`.
    /// Returns the number copied; `plays` gets the folder's total and `played` its distinct file count.
    size_t folderEntries(const char* root, const char* folder, Entry* out, size_t max, uint32_t& plays, uint32_t& played);
    /// Iterate folders known to the keeper for the given root: calls fn(folder, plays, played, lastEpoch).
    void forEachFolder(const char* root, void (*fn)(const char* folder, uint32_t plays, uint32_t played, uint32_t last, void* user), void* user);
    /// Epoch seconds when counting started (last reset), 0 if unknown.
    uint32_t since();
    // Duration of the most recent write pass, milliseconds (0 until one has run).
    uint32_t lastFlushMs();
    /// Forget everything and delete the stats files (runs on the keeper task; returns immediately).
    void requestReset();
    /// Ask the keeper to write dirty folders now instead of at the next 15-minute mark.
    void requestFlush();
    // Write dirty folders now and wait up to timeoutMs for it (call before a planned reboot/OTA restart).
    bool flushNow(uint32_t timeoutMs);
}
