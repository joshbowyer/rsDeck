#pragma once

// =============================================================================
// TileCache.h — chunked tile-loading state machine + fixed PSRAM pool
// =============================================================================
//
// Design constraints (from project brief):
//   * LoRa RX is a single-"latest-packet" pointer; if loop() stalls too long
//     an unread packet is overwritten before drain. Budget per pump() call:
//     ~20–25 ms (well below the ~30 ms fastest LoRa packet airtime).
//   * LVGL's decoder framework is OFF (LV_USE_PNG == 0). We build
//     lv_img_dsc_t manually and feed LVGL via lv_img_set_src().
//   * LV_COLOR_16_SWAP == 1 — pixel bytes must be stored swap-correct so the
//     display fetches them in the right order. We use lv_color_make().full,
//     which already applies the swap at compile time.
//   * Pool allocated ONCE lazily on first use, never freed. PSRAM is the only
//     allocation target (heap_caps_malloc, MALLOC_CAP_SPIRAM).
//   * Negative cache of ~64 (z,x,y) keys confirmed missing on SD, so fast
//     panning doesn't keep calling exists().
//   * LVGL render happens on the same main loop; the only "blocking" interior
//     in a single pump() call is one SD read + one pngle_feed.
//   * packetAvailable guard: skipped by design (TODO comment) — the chunk
//     budget is the primary safety property, and wiring SX1262::packetAvailable
//     into TileCache would couple unrelated layers just to add a redundant
//     guard.
// =============================================================================

#include <Arduino.h>
#include <SD.h>
#include <lvgl.h>
#include <cstdint>

#include "storage/SDStore.h"

extern "C" {
#include "pngle.h"
}

// Forward-declare to avoid pulling miniz.h into the public surface.
struct _pngle_t;

class TileCache {
public:
    // Tunable: bytes of compressed PNG data fed per pump() call. Lower =
    // shorter per-call latency / more responsive to LoRa. Too low = thousands
    // of pump() calls per tile. 4 KB is a reasonable starting point; tune
    // after measuring on real hardware.
    static constexpr size_t TILE_CHUNK_BYTES = 4096;

    static constexpr int     TILE_PX         = 256;          // standard XYZ tile
    static constexpr int     SLOT_COUNT      = 12;           // ~1.5 MB PSRAM pool
    static constexpr size_t  SLOT_BYTES      = TILE_PX * TILE_PX * 2;  // RGB565
    static constexpr size_t  STYLE_MAX       = 24;           // incl. NUL
    static constexpr int     REQ_QUEUE_SIZE  = 8;
    static constexpr int     NEG_CACHE_SIZE  = 64;

    enum class SlotState : uint8_t {
        FREE,        // pool slot is unallocated
        LOADING,     // file open, pngle feeding
        READY,       // decoded, lv_img_dsc_t valid
        MISSING      // file confirmed absent on SD
    };

    enum class Priority : uint8_t {
        PRIO_LOW = 0, PRIO_NORMAL = 1, PRIO_HIGH = 2
    };

    struct TileKey {
        char     style[STYLE_MAX];
        int32_t  z;
        int32_t  x;
        int32_t  y;

        bool operator==(const TileKey& o) const {
            return z == o.z && x == o.x && y == o.y && strcmp(style, o.style) == 0;
        }
    };

    void begin(SDStore* sd);

    // Enqueue a tile-load request. Dedupes against in-flight + cached entries.
    // Returns true if the request was queued (or already satisfied).
    bool requestTile(const char* style, int z, int x, int y, Priority prio = Priority::PRIO_NORMAL);

    // Called once per main-loop iteration. Does at most one chunk of work.
    // Either continues an in-progress decode, or starts the next queued
    // request onto a free/LRU-evicted slot.
    void pump();

    // Returns a ready-to-use lv_img_dsc_t pointing into the slot's PSRAM
    // buffer if the tile is cached and READY, else nullptr.
    const lv_img_dsc_t* getTileIfReady(const char* style, int z, int x, int y);

    // Manually evict a tile (e.g. on memory pressure or zoom-out).
    void evict(const char* style, int z, int x, int y);

    // Diagnostic: dump current pool state to serial.
    void dumpStatus() const;

    // Diagnostic: latest per-pump() timing in ms (max across recent calls).
    // Used by the serial-command test to verify the chunk-latency budget.
    uint32_t lastPumpMs() const { return _lastPumpMs; }
    uint32_t maxPumpMs() const  { return _maxPumpMs; }
    uint32_t pumpCount() const { return _pumpCount; }

private:
    // ---- Pool bookkeeping ----
    SDStore* _sd = nullptr;
    bool _poolInited = false;

    struct Slot {
        SlotState  state          = SlotState::FREE;
        TileKey    key;
        File       file;
        uint16_t*  pxbuf          = nullptr;  // PSRAM, 128 KB, owned
        lv_img_dsc_t dsc          = {};
        pngle_t*   pngle          = nullptr;
        size_t     bytesFed       = 0;       // total bytes read from file
        size_t     fileSize       = 0;
        size_t     ioBufLen       = 0;       // bytes of leftover in the shared ioBuf
        uint32_t   lastTouchMs    = 0;
        uint32_t   chunkCount     = 0;       // pump() calls spent on this slot
        uint32_t   first8[8]      = {0};     // first 8 pixel RGB565 values, for sanity
        int32_t    pxW            = 0;       // pngle IHDR width
        int32_t    pxH            = 0;       // pngle IHDR height
        uint32_t   pixelsWritten  = 0;
    };
    Slot _slots[SLOT_COUNT] = {};

    // ---- Request queue ----
    struct Request {
        bool    valid = false;
        TileKey key;
        Priority prio = Priority::PRIO_NORMAL;
        uint32_t enqMs = 0;
    };
    Request _reqQ[REQ_QUEUE_SIZE] = {};
    int _reqHead = 0;  // pop from head
    int _reqTail = 0;  // push to tail
    int _reqCount = 0;

    // ---- Negative cache (linear, replace-on-evict oldest) ----
    struct NegEntry {
        bool    valid = false;
        TileKey key;
        uint32_t touchMs = 0;
    };
    NegEntry _negCache[NEG_CACHE_SIZE] = {};

    // ---- Stats ----
    uint32_t _lastPumpMs = 0;
    uint32_t _maxPumpMs  = 0;
    uint32_t _pumpCount  = 0;

    // ---- Internal helpers ----
    void   ensurePool();                                          // allocate PSRAM lazily
    Slot*  findReadySlot(const char* style, int z, int x, int y);
    Slot*  pickFreeSlot();                                        // or LRU-evict
    Slot*  pickLoadingSlot();                                     // for pump() continuation
    bool   startDecode(Slot& s, const char* style, int z, int x, int y);
    static void closeDecodeStatic(Slot& s);                       // close file, free pngle — callable from static PNGLE callbacks
    void   closeDecode(Slot& s) { closeDecodeStatic(s); }        // thin wrapper
    void   freeSlot(Slot& s);                                     // recycle to FREE
    bool   isQueuedOrCached(const TileKey& k);
    bool   enqueueRequest(const TileKey& k, Priority p);
    bool   isNegCached(const TileKey& k);
    void   addNegCache(const TileKey& k);
    static int  findLruEvictableSlot(Slot slots[], int n);
    static int  findLoadingSlotIdx(Slot slots[], int n);
    static int  findFreeSlotIdx(Slot slots[], int n);
    static bool keyEq(const TileKey& a, const TileKey& b);
    static void onPngInit(pngle_t* p, uint32_t w, uint32_t h);
    static void onPngDraw(pngle_t* p, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]);
    static void onPngDone(pngle_t* p);

    // ---- diagnostics ----
    static void logPsramBudget(const char* tag);
};
