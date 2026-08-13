#include "maps/TileCache.h"
#include "maps/TileStore.h"

#include <esp_heap_caps.h>
#include <string.h>

// =============================================================================
// TileCache.cpp — see TileCache.h for the design rationale.
// =============================================================================

namespace {

constexpr uint32_t ABSOLUTE_MAX_PUMP_MS = 200;  // warn if a single pump() misses budget badly

}  // namespace

// =============================================================================
// Static helpers (need access to private Slot, so they're class statics)
// =============================================================================

int TileCache::findLruEvictableSlot(TileCache::Slot slots[], int n) {
    int picked = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < n; ++i) {
        if (slots[i].state == TileCache::SlotState::LOADING) continue;
        if (slots[i].lastTouchMs < oldest) {
            oldest = slots[i].lastTouchMs;
            picked = i;
        }
    }
    return picked;
}

int TileCache::findLoadingSlotIdx(TileCache::Slot slots[], int n) {
    for (int i = 0; i < n; ++i) {
        if (slots[i].state == TileCache::SlotState::LOADING) return i;
    }
    return -1;
}

int TileCache::findFreeSlotIdx(TileCache::Slot slots[], int n) {
    for (int i = 0; i < n; ++i) {
        if (slots[i].state == TileCache::SlotState::FREE) return i;
    }
    return -1;
}

bool TileCache::keyEq(const TileCache::TileKey& a, const TileCache::TileKey& b) {
    return a.z == b.z && a.x == b.x && a.y == b.y && strcmp(a.style, b.style) == 0;
}

// =============================================================================
// Lifecycle
// =============================================================================

void TileCache::begin(SDStore* sd) {
    _sd = sd;
    // Pool is allocated lazily on first pump() so that we don't steal 1.5 MB
    // of PSRAM at boot if the map screen is never opened.
}

void TileCache::ensurePool() {
    if (_poolInited) return;
    TileCache::logPsramBudget("[TILE] before-pool");
    for (int i = 0; i < SLOT_COUNT; ++i) {
        _slots[i].pxbuf = (uint16_t*)heap_caps_malloc(SLOT_BYTES, MALLOC_CAP_SPIRAM);
        if (!_slots[i].pxbuf) {
            Serial.printf("[TILE] FATAL: pool alloc failed at slot %d (%u bytes)\n", i, (unsigned)SLOT_BYTES);
            // Continue; that slot just won't be usable. We'll check on use.
            continue;
        }
        memset(_slots[i].pxbuf, 0, SLOT_BYTES);
        // Build a permanent lv_img_dsc_t pointing at the slot buffer. Header
        // values are filled when the slot becomes READY.
        _slots[i].dsc.header.always_zero = 0;
        _slots[i].dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
        _slots[i].dsc.header.w            = TILE_PX;
        _slots[i].dsc.header.h            = TILE_PX;
        _slots[i].dsc.data_size           = SLOT_BYTES;
        _slots[i].dsc.data                = (const uint8_t*)_slots[i].pxbuf;
    }
    _poolInited = true;
    TileCache::logPsramBudget("[TILE] after-pool");
}

void TileCache::logPsramBudget(const char* tag) {
    Serial.printf("%s free_psram=%u largest_psram_block=%u\n",
                  tag,
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

// =============================================================================
// Negative cache (small, linear, evict-oldest-on-insert)
// =============================================================================

bool TileCache::isNegCached(const TileKey& k) {
    for (int i = 0; i < NEG_CACHE_SIZE; ++i) {
        if (_negCache[i].valid && keyEq(_negCache[i].key, k)) return true;
    }
    return false;
}

void TileCache::addNegCache(const TileKey& k) {
    int oldest = 0;
    uint32_t oldestMs = 0xFFFFFFFFu;
    for (int i = 0; i < NEG_CACHE_SIZE; ++i) {
        if (!_negCache[i].valid) {
            _negCache[i].valid = true;
            _negCache[i].key = k;
            _negCache[i].touchMs = millis();
            return;
        }
        if (_negCache[i].touchMs < oldestMs) {
            oldestMs = _negCache[i].touchMs;
            oldest = i;
        }
    }
    _negCache[oldest].valid = true;
    _negCache[oldest].key = k;
    _negCache[oldest].touchMs = millis();
}

// =============================================================================
// Request queue (small ring, dedup-on-push)
// =============================================================================

bool TileCache::isQueuedOrCached(const TileKey& k) {
    // In queue?
    for (int i = 0; i < _reqCount; ++i) {
        int idx = (_reqHead + i) % REQ_QUEUE_SIZE;
        if (_reqQ[idx].valid && keyEq(_reqQ[idx].key, k)) return true;
    }
    // Cached?
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (_slots[i].state == SlotState::READY && keyEq(_slots[i].key, k)) return true;
    }
    return false;
}

bool TileCache::enqueueRequest(const TileKey& k, Priority p) {
    if (_reqCount >= REQ_QUEUE_SIZE) return false;
    int idx = _reqTail;
    _reqQ[idx].valid = true;
    _reqQ[idx].key = k;
    _reqQ[idx].prio = p;
    _reqQ[idx].enqMs = millis();
    _reqTail = (_reqTail + 1) % REQ_QUEUE_SIZE;
    ++_reqCount;
    return true;
}

bool TileCache::requestTile(const char* style, int z, int x, int y, Priority prio) {
    if (!_sd || !style) return false;
    TileKey k;
    strncpy(k.style, style, STYLE_MAX - 1);
    k.style[STYLE_MAX - 1] = '\0';
    k.z = z; k.x = x; k.y = y;
    if (isNegCached(k)) return false;
    if (isQueuedOrCached(k)) return true;
    return enqueueRequest(k, prio);
}

// =============================================================================
// Slot lookup
// =============================================================================

TileCache::Slot* TileCache::findReadySlot(const char* style, int z, int x, int y) {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (_slots[i].state == SlotState::READY &&
            _slots[i].key.z == z && _slots[i].key.x == x && _slots[i].key.y == y &&
            strcmp(_slots[i].key.style, style) == 0) {
            _slots[i].lastTouchMs = millis();
            return &_slots[i];
        }
    }
    return nullptr;
}

TileCache::Slot* TileCache::pickFreeSlot() {
    int idx = findFreeSlotIdx(_slots, SLOT_COUNT);
    if (idx < 0) {
        idx = findLruEvictableSlot(_slots, SLOT_COUNT);
        if (idx < 0) return nullptr;
        freeSlot(_slots[idx]);
    }
    return &_slots[idx];
}

TileCache::Slot* TileCache::pickLoadingSlot() {
    int idx = findLoadingSlotIdx(_slots, SLOT_COUNT);
    return idx < 0 ? nullptr : &_slots[idx];
}

// =============================================================================
// Decode driver
// =============================================================================

bool TileCache::startDecode(Slot& s, const char* style, int z, int x, int y) {
    if (!s.pxbuf) return false;
    s.key.z = z; s.key.x = x; s.key.y = y;
    strncpy(s.key.style, style, STYLE_MAX - 1);
    s.key.style[STYLE_MAX - 1] = '\0';
    s.file = TileStore::openTile(*_sd, style, z, x, y);
    if (!s.file) {
        addNegCache(s.key);
        s.state = SlotState::MISSING;
        return false;
    }
    s.fileSize = s.file.size();
    s.bytesFed = 0;
    s.ioBufLen = 0;
    s.chunkCount = 0;
    s.pixelsWritten = 0;
    s.pxW = 0;
    s.pxH = 0;
    memset(s.first8, 0, sizeof(s.first8));
    // Clear the slot buffer to black so partially-decoded tiles look sensible
    // (and so any out-of-range pixels render as void).
    memset(s.pxbuf, 0, SLOT_BYTES);

    s.pngle = pngle_new();
    if (!s.pngle) {
        Serial.println("[TILE] pngle_new() failed");
        s.file.close();
        s.state = SlotState::MISSING;
        return false;
    }
    pngle_set_user_data(s.pngle, &s);
    pngle_set_init_callback(s.pngle, &TileCache::onPngInit);
    pngle_set_draw_callback(s.pngle, &TileCache::onPngDraw);
    pngle_set_done_callback(s.pngle, &TileCache::onPngDone);
    s.state = SlotState::LOADING;
    s.lastTouchMs = millis();
    return true;
}

void TileCache::closeDecodeStatic(Slot& s) {
    if (s.file) s.file.close();
    if (s.pngle) {
        pngle_destroy(s.pngle);
        s.pngle = nullptr;
    }
}

void TileCache::freeSlot(Slot& s) {
    closeDecode(s);
    s.state = SlotState::FREE;
    s.key = {};
    s.bytesFed = 0;
    s.fileSize = 0;
    s.ioBufLen = 0;
    s.pxW = 0;
    s.pxH = 0;
}

void TileCache::evict(const char* style, int z, int x, int y) {
    if (!style) return;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (_slots[i].state == SlotState::READY &&
            _slots[i].key.z == z && _slots[i].key.x == x && _slots[i].key.y == y &&
            strcmp(_slots[i].key.style, style) == 0) {
            freeSlot(_slots[i]);
            return;
        }
    }
}

// =============================================================================
// PNGLE callbacks (static, route via user_data)
// =============================================================================

void TileCache::onPngInit(pngle_t* p, uint32_t w, uint32_t h) {
    Slot* s = (Slot*)pngle_get_user_data(p);
    if (!s) return;
    s->pxW = (int32_t)w;
    s->pxH = (int32_t)h;
    if (w != (uint32_t)TILE_PX || h != (uint32_t)TILE_PX) {
        Serial.printf("[TILE] WARNING: non-standard tile %ux%u (expected %dx%d)\n",
                      (unsigned)w, (unsigned)h, TILE_PX, TILE_PX);
    }
    s->dsc.header.always_zero = 0;
    s->dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    s->dsc.header.w = (uint32_t)w;
    s->dsc.header.h = (uint32_t)h;
    s->dsc.data_size = (uint32_t)w * (uint32_t)h * 2;
}

void TileCache::onPngDraw(pngle_t* p, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    Slot* s = (Slot*)pngle_get_user_data(p);
    if (!s || !s->pxbuf) return;
    // LV_COLOR_16_SWAP is applied at compile time by lv_color_make, so just
    // pack RGB565 and write to the buffer as if it were a uint16_t array.
    // On little-endian ESP32 + LV_COLOR_16_SWAP=1, the lv_color_make result
    // already encodes the swap inside the .full uint16_t.
    const uint16_t color = lv_color_make(rgba[0], rgba[1], rgba[2]).full;
    // w/h > 1 only for interlaced PNGs; for non-interlaced (the common case)
    // it's 1x1 and the fill loop is a single iteration.
    for (uint32_t yy = y; yy < y + h; ++yy) {
        if (yy >= (uint32_t)TILE_PX) break;
        for (uint32_t xx = x; xx < x + w; ++xx) {
            if (xx >= (uint32_t)TILE_PX) break;
            s->pxbuf[yy * TILE_PX + xx] = color;
            if (s->pixelsWritten < 8) {
                s->first8[s->pixelsWritten] = color;
            }
            ++s->pixelsWritten;
        }
    }
}

void TileCache::onPngDone(pngle_t* p) {
    Slot* s = (Slot*)pngle_get_user_data(p);
    if (!s) return;
    // Mark READY. The dsc was already populated in onPngInit.
    s->dsc.header.always_zero = 0;
    if (s->pxW > 0 && s->pxH > 0) {
        s->dsc.header.w = (uint32_t)s->pxW;
        s->dsc.header.h = (uint32_t)s->pxH;
        s->dsc.data_size = (uint32_t)s->pxW * (uint32_t)s->pxH * 2;
    }
    closeDecodeStatic(*s);
    s->state = SlotState::READY;
    s->lastTouchMs = millis();
}

// =============================================================================
// pump() — called once per main-loop iteration
// =============================================================================

void TileCache::pump() {
    if (!_sd) return;
    ensurePool();
    if (!_poolInited) return;  // pool alloc failed; nothing we can do

    unsigned long pumpStart = millis();

    // 1. Continue an in-progress decode if any.
    if (Slot* s = pickLoadingSlot()) {
        // Shared IO buffer in BSS (not on the stack — 8 KB would overflow
        // the default 8 KB Arduino loop task stack). Only one slot is
        // LOADING at a time, so a single buffer is safe. Leftover bytes
        // from pngle_feed are tracked per-slot via s->ioBufLen.
        static uint8_t buf[TILE_CHUNK_BYTES * 2];
        size_t room = sizeof(buf);
        if (room > s->ioBufLen + TILE_CHUNK_BYTES) {
            room = s->ioBufLen + TILE_CHUNK_BYTES;
        }
        size_t remaining = (s->bytesFed < s->fileSize) ? (s->fileSize - s->bytesFed) : 0;
        size_t want = (room < remaining) ? room : remaining;
        size_t got = 0;
        if (want > 0) {
            got = s->file.read(buf + s->ioBufLen, want);
            s->bytesFed += got;
        }
        size_t totalLen = s->ioBufLen + got;
        if (totalLen > 0) {
            int fed = pngle_feed(s->pngle, buf, totalLen);
            if (fed < 0) {
                // Decode error — release the slot, mark MISSING.
                Serial.printf("[TILE] decode err z=%ld x=%ld y=%ld: %s\n",
                              (long)s->key.z, (long)s->key.x, (long)s->key.y,
                              pngle_error(s->pngle));
                addNegCache(s->key);
                freeSlot(*s);
            } else {
                size_t leftover = (size_t)totalLen - (size_t)fed;
                if (leftover > 0) {
                    memmove(buf, buf + fed, leftover);
                }
                s->ioBufLen = leftover;
            }
        }
        // EOF check: file exhausted, no leftover, pngle still hasn't marked
        // DONE. Either the PNG is corrupt/truncated, or done callback is in
        // flight. Best-effort: mark READY with partial data.
        if (s->state == SlotState::LOADING &&
            s->file.available() == 0 && s->bytesFed >= s->fileSize &&
            s->ioBufLen == 0) {
            Serial.printf("[TILE] warn: tile exhausted without DONE z=%ld x=%ld y=%ld\n",
                          (long)s->key.z, (long)s->key.x, (long)s->key.y);
            closeDecodeStatic(*s);
            s->state = SlotState::READY;
            s->lastTouchMs = millis();
        }
        ++s->chunkCount;
    }
    // 2. Otherwise, start the next request.
    else if (_reqCount > 0) {
        // Find highest-priority request.
        int bestIdx = -1;
        Priority bestPrio = Priority::PRIO_LOW;
        uint32_t bestEnq = 0xFFFFFFFFu;
        for (int i = 0; i < _reqCount; ++i) {
            int idx = (_reqHead + i) % REQ_QUEUE_SIZE;
            if (!_reqQ[idx].valid) continue;
            if ((int)_reqQ[idx].prio > (int)bestPrio ||
                ((int)_reqQ[idx].prio == (int)bestPrio && _reqQ[idx].enqMs < bestEnq)) {
                bestPrio = _reqQ[idx].prio;
                bestEnq  = _reqQ[idx].enqMs;
                bestIdx  = idx;
            }
        }
        if (bestIdx >= 0) {
            const TileKey& k = _reqQ[bestIdx].key;
            Slot* s = pickFreeSlot();
            if (s) {
                if (!startDecode(*s, k.style, k.z, k.x, k.y)) {
                    // Failed to start (missing file, alloc failure, etc) — the
                    // slot is already marked MISSING/FREE. Don't requeue.
                }
            }
            // Pop the request from the queue regardless.
            _reqQ[bestIdx].valid = false;
            _reqHead = (_reqHead + 1) % REQ_QUEUE_SIZE;
            --_reqCount;
        }
    }

    unsigned long pumpElapsed = millis() - pumpStart;
    _lastPumpMs = (uint32_t)pumpElapsed;
    if (_lastPumpMs > _maxPumpMs) _maxPumpMs = _lastPumpMs;
    ++_pumpCount;
    if (pumpElapsed > ABSOLUTE_MAX_PUMP_MS) {
        Serial.printf("[TILE] pump() exceeded soft budget: %lu ms\n", pumpElapsed);
    }
}

// =============================================================================
// Public read API
// =============================================================================

const lv_img_dsc_t* TileCache::getTileIfReady(const char* style, int z, int x, int y) {
    if (!style) return nullptr;
    if (!_poolInited) return nullptr;
    Slot* s = findReadySlot(style, z, x, y);
    return s ? &s->dsc : nullptr;
}

// =============================================================================
// Diagnostics
// =============================================================================

void TileCache::dumpStatus() const {
    Serial.printf("[TILE] pool=%s slots: ", _poolInited ? "ok" : "UNINIT");
    int ready = 0, loading = 0, missing = 0, free = 0;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        switch (_slots[i].state) {
            case SlotState::READY:   ++ready;   break;
            case SlotState::LOADING: ++loading; break;
            case SlotState::MISSING: ++missing;  break;
            case SlotState::FREE:    ++free;    break;
        }
    }
    Serial.printf("ready=%d loading=%d missing=%d free=%d last=%lums max=%lums pumps=%lu\n",
                  ready, loading, missing, free,
                  (unsigned long)_lastPumpMs, (unsigned long)_maxPumpMs,
                  (unsigned long)_pumpCount);
    if (loading > 0) {
        for (int i = 0; i < SLOT_COUNT; ++i) {
            if (_slots[i].state == SlotState::LOADING) {
                Serial.printf("[TILE]   loading %s/%ld/%ld/%ld pngle=%p bytes=%lu/%lu chunks=%lu px=%lu\n",
                              _slots[i].key.style, (long)_slots[i].key.z,
                              (long)_slots[i].key.x, (long)_slots[i].key.y,
                              (const void*)_slots[i].pngle,
                              (unsigned long)_slots[i].bytesFed,
                              (unsigned long)_slots[i].fileSize,
                              (unsigned long)_slots[i].chunkCount,
                              (unsigned long)_slots[i].pixelsWritten);
            }
        }
    }
}
