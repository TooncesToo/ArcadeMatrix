#pragma once
#include <Arduino.h>
#include <stdint.h>

/**
 * @brief Play statistics for the GIF library: how often and when each file was shown.
 *
 * Neither firmware remembered anything beyond the last file played, so "it keeps showing the same
 * ones" could not be checked. This keeps one entry per played file in PSRAM, keyed by a hash of its
 * name inside its folder and carrying the name itself. The render path (GifEngine, Core 1) only
 * calls recordPlay(), which hashes the path, bumps a counter and copies the name into a slot the
 * keeper has already reserved: no String, no heap, no SD.
 *
 * A Core 0 task does everything that touches the card: reading each folder's stats.txt once, growing
 * the arrays so the render path never has to, writing dirty folders back at most every 15 minutes,
 * and honouring a reset. Every card access reads or writes one buffer in a single short hold, so a
 * playing GIF (which holds the SD mutex for its own reads) is never blocked for long. Files live
 * next to index.txt as `<root>/<folder>/stats.txt`, one "name<TAB>count<TAB>last_epoch" line per
 * file, so a rescan does not disturb them and they are readable by hand.
 */
namespace GifStats {
    static constexpr size_t GIFSTATS_NAME_CAP = 112;   ///< Longest file name kept; longer ones are counted but not persisted

    struct Entry {
        uint32_t nameHash;
        uint16_t count;
        uint32_t last;
        char name[GIFSTATS_NAME_CAP];   ///< Empty when the name was too long to keep
    };

    /// Start the Core 0 keeper task; call once after the card is mounted.
    void start();
    /// Record that `fullPath` ("/gifs/<folder>/<name>" or "/gifs_tate/...") started playing. Hot-path safe.
    void recordPlay(const char* fullPath);
    /// FNV-1a of a file name, the key used everywhere.
    uint32_t hashName(const char* name);

    /// Snapshot of one folder for the API: entries (count > 0 only) copied into `out`, capped at `max`.
    /// Returns the number copied; `plays` gets the folder's total and `played` its distinct file count.
    size_t folderEntries(const char* root, const char* folder, Entry* out, size_t max, uint32_t& plays, uint32_t& played);
    /// Iterate folders known to the keeper for the given root: fn(folder, plays, played, lastEpoch, files).
    /// `files` is the folder's size from playlists.json, cached by the keeper so callers touch no card.
    void forEachFolder(const char* root, void (*fn)(const char* folder, uint32_t plays, uint32_t played, uint32_t last, uint32_t files, void* user), void* user);
    /// Epoch seconds when counting started (last reset), 0 if unknown.
    uint32_t since();
    /// Duration of the most recent write pass in milliseconds, and how many folders it wrote.
    uint32_t lastFlushMs();
    uint32_t lastFlushFolders();
    /// Plays dropped because no slot was free when they happened (should stay 0).
    uint32_t dropped();
    /// Forget everything and delete the stats files (runs on the keeper task; returns immediately).
    void requestReset();
    /// Ask the keeper to write dirty folders now instead of at the next 15-minute mark.
    void requestFlush();
    /// Write dirty folders now and wait up to timeoutMs for it (call before a planned reboot/OTA restart).
    bool flushNow(uint32_t timeoutMs);
}
