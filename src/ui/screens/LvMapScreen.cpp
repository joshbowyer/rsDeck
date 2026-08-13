#include "LvMapScreen.h"

#include "maps/TileCache.h"
#include "maps/SlippyMath.h"
#include "hal/GPSManager.h"
#include "hal/Trackball.h"
#include "hal/TouchInput.h"
#include "ui/Theme.h"
#include "ui/UIManager.h"
#include "ui/LvTabBar.h"
#include <Arduino.h>
#include <cstdlib>
#include <cstring>
#include "fonts/fonts.h"

// Shared hardware singletons — same `extern` pattern as Power.cpp uses
// for Display/Keyboard. The map screen is the only screen that reads
// raw trackball/touch state outside the LVGL input drivers, so an
// extern is cleaner than passing a setter from main.cpp's globals.
extern Trackball trackball;
extern TouchInput touch;

namespace {

// ---- HUD positioning helpers ----
//
// All HUD labels live on the clipped content area's top/bottom edges,
// overlaid on the tile grid. Coordinates are relative to the content
// parent (which UIManager sets at y=STATUS_BAR_H=20).

constexpr int kHudZoomX = 4;
constexpr int kHudZoomY = 2;
constexpr int kHudMapsetX = 4;
constexpr int kHudMapsetY = 174;     // CONTENT_H - 20 - 0 = 174

constexpr int kHudFollowX = 244;
constexpr int kHudFollowY = 2;
constexpr int kHudGpsX = 196;
constexpr int kHudGpsY = 174;

constexpr int kMarkerSize = 14;

lv_obj_t* makeHudLabel(lv_obj_t* parent, int x, int y, int w, int h,
                       const lv_font_t* font, uint32_t color,
                       const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_size(lbl, w, h);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_bg_color(lbl, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_70, 0);
    lv_obj_set_style_pad_left(lbl, 3, 0);
    lv_obj_set_style_pad_right(lbl, 3, 0);
    lv_obj_set_style_pad_top(lbl, 1, 0);
    lv_obj_set_style_pad_bottom(lbl, 1, 0);
    lv_obj_set_style_radius(lbl, 2, 0);
    lv_obj_set_style_border_width(lbl, 0, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(lbl, text);
    return lbl;
}

}  // namespace

void LvMapScreen::createUI(lv_obj_t* parent) {
    _screen = parent;

    // Full-screen clipped container — LVGL handles clipping of tile imgs
    // positioned outside the viewport, so we don't re-blit per frame.
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Tile container — sits at (0,0) of the content area, full size.
    // We don't enable parent-level clipping here because the content
    // parent itself clips (UIManager's _lvContent is the boundary), and
    // each tile img renders normally outside its own bounds (LVGL clips
    // at the rendering stage).
    _mapContainer = lv_obj_create(parent);
    lv_obj_set_size(_mapContainer, VIEW_W, VIEW_H);
    lv_obj_set_pos(_mapContainer, 0, 0);
    lv_obj_set_style_bg_opa(_mapContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_mapContainer, 0, 0);
    lv_obj_set_style_pad_all(_mapContainer, 0, 0);
    lv_obj_clear_flag(_mapContainer, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Tile slots (placeholders + image widgets) ----
    for (int i = 0; i < SLOT_COUNT; ++i) {
        TileSlot& s = _slots[i];

        // Gray placeholder — shown until the tile is READY. We size it
        // exactly 256x256; positioning is done by rebuildTiles().
        s.bg = lv_obj_create(_mapContainer);
        lv_obj_set_size(s.bg, TILE_PX, TILE_PX);
        lv_obj_set_pos(s.bg, 0, 0);
        lv_obj_set_style_bg_color(s.bg, lv_color_hex(Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(s.bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s.bg, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(s.bg, 1, 0);
        lv_obj_set_style_radius(s.bg, 0, 0);
        lv_obj_set_style_pad_all(s.bg, 0, 0);
        lv_obj_clear_flag(s.bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        // Tile image — sits exactly on top of the placeholder. When the
        // tile is READY, lv_img_set_src() points it at the TileCache's
        // pre-decoded lv_img_dsc_t; LVGL handles blitting. When no
        // tile is ready, the img is HIDDEN so the placeholder shows.
        s.img = lv_img_create(_mapContainer);
        lv_obj_set_size(s.img, TILE_PX, TILE_PX);
        lv_obj_set_pos(s.img, 0, 0);
        lv_img_set_antialias(s.img, false);
        lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s.img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    // ---- HUD (drawn on top of tiles) ----
    _hudZoom = makeHudLabel(parent, kHudZoomX, kHudZoomY, 60, 14,
                            &lv_font_rsdeck_10, Theme::TEXT_PRIMARY, "z-");
    _hudMapset = makeHudLabel(parent, kHudMapsetX, kHudMapsetY, 180, 14,
                              &lv_font_rsdeck_10, Theme::TEXT_MUTED, MAPSET_NAME);
    _hudGps = makeHudLabel(parent, kHudGpsX, kHudGpsY, 120, 14,
                           &lv_font_rsdeck_10, Theme::TEXT_MUTED, "GPS --");
    _hudFollow = makeHudLabel(parent, kHudFollowX, kHudFollowY, 70, 14,
                              &lv_font_rsdeck_10, Theme::TEXT_SECONDARY,
                              _followGPS ? "FOLLOW" : "MANUAL");

    // ---- GPS marker (separate overlay, NOT baked into the tile grid) ----
    // A small filled circle. We don't use a canvas — a plain lv_obj with
    // circular styling is cheaper and perfectly fine for one static dot.
    _marker = lv_obj_create(parent);
    lv_obj_set_size(_marker, kMarkerSize, kMarkerSize);
    lv_obj_set_pos(_marker, 0, 0);
    lv_obj_set_style_radius(_marker, kMarkerSize / 2, 0);
    lv_obj_set_style_bg_color(_marker, lv_color_hex(Theme::PRIMARY), 0);
    lv_obj_set_style_bg_opa(_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_marker, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_obj_set_style_border_width(_marker, 1, 0);
    lv_obj_set_style_pad_all(_marker, 0, 0);
    lv_obj_set_style_shadow_width(_marker, 0, 0);
    lv_obj_add_flag(_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_marker, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Initial layout
    rebuildTiles();
    centerOnGpsIfAvailable();
    rebuildTiles();  // second pass: center may have moved the viewport
    updateHud();
    requestVisibleTiles();
}

void LvMapScreen::destroyUI() {
    // UIManager will lv_obj_clean(_lvContent) immediately after this —
    // every child widget we created on `parent` is about to be deleted.
    // Just null out our local pointers so a stray refreshUI() between
    // now and the clean doesn't dereference a tombstone.
    _mapContainer = nullptr;
    _marker = nullptr;
    _hudZoom = nullptr;
    _hudMapset = nullptr;
    _hudGps = nullptr;
    _hudFollow = nullptr;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        _slots[i].bg = nullptr;
        _slots[i].img = nullptr;
        _slots[i].curDsc = nullptr;
        _slots[i].tx = INT32_MIN;
        _slots[i].ty = INT32_MIN;
        _slots[i].tz = -1;
    }
    LvScreen::destroyUI();
}

void LvMapScreen::onEnter() {
    _lastMarkerMs = 0;
    _lastTileRequestMs = 0;
    _lastHudRefreshMs = 0;
    _touchActive = false;
    _touchDoubleArmed = false;

    // Remember which tab the user came from so Esc can put them back.
    if (_ui) _prevTab = _ui->lvTabBar().getActiveTab();

    // Snap to GPS fix if we're already following (so a brief tab-switch
    // away and back doesn't yank the map). Otherwise keep the previous
    // center so the user's last view is preserved.
    if (_followGPS) centerOnGpsIfAvailable();
    rebuildTiles();
    updateHud();
    requestVisibleTiles();
}

void LvMapScreen::refreshUI() {
    if (!_mapContainer) return;

    unsigned long now = millis();

    // 1. Drain accumulated trackball deltas into pan offset. Trackball
    //    ISR fires per-tick; we sample + accumulate here and flush every
    //    PAN_FLUSH_MS so each flush is a discrete move rather than
    //    single-tick micro-pans.
    static int8_t accumX = 0;
    static int8_t accumY = 0;
    static unsigned long lastPanMs = 0;
    int8_t dx = trackball.lastDeltaX();
    int8_t dy = trackball.lastDeltaY();
    if (dx != 0 || dy != 0) {
        accumX += dx;
        accumY += dy;
        if (accumX > 16) accumX = 16;
        if (accumX < -16) accumX = -16;
        if (accumY > 16) accumY = 16;
        if (accumY < -16) accumY = -16;
    }
    if ((accumX != 0 || accumY != 0) && now - lastPanMs >= PAN_FLUSH_MS) {
        lastPanMs = now;
        int8_t ax = accumX; accumX = 0;
        int8_t ay = accumY; accumY = 0;
        panBy((int32_t)ax * PAN_SPEED, (int32_t)ay * PAN_SPEED);
        rebuildTiles();
    }

    // 2. Touch handling — single-point GT911, no pinch possible.
    //    Drag = pan (delta of touch.x/y while isTouched). Two taps
    //    within DOUBLE_TAP_MS / DOUBLE_TAP_RADIUS_PX = zoom in.
    static bool wasTouched = false;
    static int16_t downX = 0, downY = 0;
    static unsigned long downMs = 0;
    static int16_t lastTouchX = 0, lastTouchY = 0;
    bool touched = touch.isTouched();
    if (touched) {
        int16_t tx = touch.x();
        int16_t ty = touch.y();
        if (!wasTouched) {
            // Touch-down event
            wasTouched = true;
            downX = tx;
            downY = ty;
            downMs = now;
            lastTouchX = tx;
            lastTouchY = ty;
            _touchActive = true;

            if (_touchDoubleArmed &&
                now - _touchDoubleMs <= DOUBLE_TAP_MS &&
                abs(tx - _touchDoubleX) <= DOUBLE_TAP_RADIUS_PX &&
                abs(ty - _touchDoubleY) <= DOUBLE_TAP_RADIUS_PX) {
                zoomIn();
                rebuildTiles();
                _touchDoubleArmed = false;  // consumed
            } else {
                _touchDoubleArmed = true;
                _touchDoubleX = tx;
                _touchDoubleY = ty;
                _touchDoubleMs = now;
            }
        } else {
            // Touch-move: pan based on delta from last position
            int16_t dxT = tx - lastTouchX;
            int16_t dyT = ty - lastTouchY;
            if (dxT != 0 || dyT != 0) {
                // Touch drags move the map OPPOSITE to the finger motion
                // (finger moves right = world content moves right = camera
                //  moves left in world px).
                panBy(-(int32_t)dxT, -(int32_t)dyT);
                rebuildTiles();
            }
            lastTouchX = tx;
            lastTouchY = ty;
        }
    } else if (wasTouched) {
        // Touch-up — disarm the double-tap if the touch was held too long
        // (so a long press doesn't get misinterpreted as two taps).
        wasTouched = false;
        _touchActive = false;
        if (now - downMs > DOUBLE_TAP_MS) {
            _touchDoubleArmed = false;
        }
    }

    // 3. Marker + follow-GPS — throttled to 1Hz.
    if (now - _lastMarkerMs >= MARKER_REFRESH_MS) {
        _lastMarkerMs = now;
        bool didRecenter = false;
        if (_followGPS) {
            centerOnGpsIfAvailable();
            didRecenter = true;
        }
        updateMarker();
        if (didRecenter) rebuildTiles();
    }

    // 4. Tile request re-arm — for still-missing tiles, every 250ms.
    if (now - _lastTileRequestMs >= TILE_REQUEST_INTERVAL_MS) {
        _lastTileRequestMs = now;
        requestVisibleTiles();
    }

    // 5. HUD refresh — every 500ms is plenty for these static labels.
    if (now - _lastHudRefreshMs >= HUD_REFRESH_MS) {
        _lastHudRefreshMs = now;
        updateHud();
    }
}

bool LvMapScreen::handleKey(const KeyEvent& event) {
    // 'c' / 'C' — re-enable follow-GPS mode (snap to current fix).
    // Bare 'c' is unused across all screens (hotkey bindings are
    // Ctrl+letter, all in main.cpp's HotkeyManager — none use bare 'c'),
    // so this won't collide.
    if (event.character == 'c' || event.character == 'C') {
        _followGPS = true;
        centerOnGpsIfAvailable();
        rebuildTiles();
        updateHud();
        if (_ui) _ui->lvStatusBar().showToast("Following GPS", 800);
        return true;
    }

    // '+' / '=' — zoom in
    if (event.character == '+' || event.character == '=') {
        zoomIn();
        rebuildTiles();
        return true;
    }
    // '-' / '_' — zoom out
    if (event.character == '-' || event.character == '_') {
        zoomOut();
        rebuildTiles();
        return true;
    }

    // Arrow keys — pan in 32px increments (alternative to trackball drag)
    if (event.up)    { panBy(0,  32); rebuildTiles(); return true; }
    if (event.down)  { panBy(0, -32); rebuildTiles(); return true; }
    if (event.left)  { panBy( 32, 0); rebuildTiles(); return true; }
    if (event.right) { panBy(-32, 0); rebuildTiles(); return true; }

    // Enter — zoom in (mirrors trackball click behavior)
    if (event.enter || event.character == '\n' || event.character == '\r') {
        zoomIn();
        rebuildTiles();
        return true;
    }

    // Esc / back — return to previous tab.
    if (event.character == 0x1B || event.del || event.character == 0x08) {
        if (_ui) {
            _ui->lvTabBar().setActiveTab(_prevTab);
            // setActiveTab triggers _tabCb which routes via lvTabScreens
            // back to the appropriate screen — no manual setScreen call
            // needed, and it keeps the tab bar's visual state in sync.
        }
        return true;
    }

    // '/' / ',' — normally global tab cycle (see main.cpp:2248-2260),
    // but we intercept and route back to the previous tab so the user
    // doesn't accidentally leave the map screen and land somewhere
    // unrelated.
    if (event.character == ',' || event.character == '/') {
        if (_ui) _ui->lvTabBar().setActiveTab(_prevTab);
        return true;
    }

    return false;
}

bool LvMapScreen::handleLongPress() {
    // Long-press on the map screen → zoom out (clamped). Returning true
    // prevents the global default in main.cpp from blanking the screen.
    zoomOut();
    rebuildTiles();
    return true;
}

// ---- Tile grid ----

void LvMapScreen::viewportOriginWorldPx(int64_t& outX, int64_t& outY) const {
    outX = _centerWorldX - VIEW_HALF_W;
    outY = _centerWorldY - VIEW_HALF_H;
}

void LvMapScreen::tileScreenPos(int32_t tx, int32_t ty,
                                int32_t& outX, int32_t& outY) const {
    int64_t ox, oy;
    viewportOriginWorldPx(ox, oy);
    outX = (int32_t)((int64_t)tx * (int64_t)TILE_PX - ox);
    outY = (int32_t)((int64_t)ty * (int64_t)TILE_PX - oy);
}

void LvMapScreen::rebuildTiles() {
    if (!_mapContainer) return;

    int64_t ox, oy;
    viewportOriginWorldPx(ox, oy);
    SlippyMath::TileXY tl = SlippyMath::worldPxToTile({ox, oy});

    // Visible tile range with one buffer tile on each edge. We use 3 cols
    // and 2 rows (6 slots total) regardless of zoom — at any zoom only
    // 1-3 tiles overlap the viewport, and LVGL clips the rest.
    int32_t txMin = tl.x - 1;
    int32_t tyMin = tl.y - 1;

    for (int row = 0; row < GRID_ROWS; ++row) {
        for (int col = 0; col < GRID_COLS; ++col) {
            int idx = row * GRID_COLS + col;
            TileSlot& s = _slots[idx];
            int32_t tx = txMin + col;
            int32_t ty = tyMin + row;

            int32_t sx, sy;
            tileScreenPos(tx, ty, sx, sy);

            // Move bg + img to the new screen position. Tiles outside
            // the viewport get negative or >VIEW_W coords — LVGL's
            // content-area parent clips them.
            if (s.bg) lv_obj_set_pos(s.bg, sx, sy);
            if (s.img) lv_obj_set_pos(s.img, sx, sy);

            s.tx = tx;
            s.ty = ty;
            s.tz = _zoom;

            // Try to attach a ready tile. If not ready yet, the placeholder
            // stays visible. requestVisibleTiles() will queue a load.
            const lv_img_dsc_t* dsc = nullptr;
            if (_tileCache) {
                dsc = _tileCache->getTileIfReady(MAPSET_NAME, _zoom, tx, ty);
            }
            if (dsc) {
                if (s.img && s.curDsc != dsc) {
                    lv_img_set_src(s.img, dsc);
                    s.curDsc = dsc;
                    lv_obj_clear_flag(s.img, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                // No tile yet — hide the img so the placeholder shows.
                // We avoid lv_img_set_src(nullptr) here because LVGL
                // logs a warning on unknown src type; toggling HIDDEN
                // produces the same visual effect without the warning.
                if (s.img) {
                    if (!lv_obj_has_flag(s.img, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
                    }
                    s.curDsc = nullptr;
                }
            }
        }
    }
}

void LvMapScreen::requestVisibleTiles() {
    if (!_tileCache) return;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        TileSlot& s = _slots[i];
        if (s.tz != _zoom) continue;  // slot is being repopulated this tick
        _tileCache->requestTile(MAPSET_NAME, s.tz, s.tx, s.ty,
                                 TileCache::Priority::PRIO_NORMAL);
    }
}

// ---- GPS marker / follow ----

void LvMapScreen::centerOnGpsIfAvailable() {
    if (!_gps || !_gps->hasLocationFix()) return;
    SlippyMath::WorldPx wp = SlippyMath::lonLatToWorldPx(
        _gps->longitude(), _gps->latitude(), _zoom);
    _centerWorldX = wp.x;
    _centerWorldY = wp.y;
}

void LvMapScreen::updateMarker() {
    if (!_marker) return;

    if (!_gps || !_gps->hasLocationFix()) {
        lv_obj_add_flag(_marker, LV_OBJ_FLAG_HIDDEN);
        if (_hudGps) lv_label_set_text(_hudGps, "GPS no fix");
        return;
    }

    // Project current GPS lat/lon to screen-space at the current zoom.
    SlippyMath::WorldPx wp = SlippyMath::lonLatToWorldPx(
        _gps->longitude(), _gps->latitude(), _zoom);
    int64_t ox, oy;
    viewportOriginWorldPx(ox, oy);
    int sx = (int)(wp.x - ox) - kMarkerSize / 2;
    int sy = (int)(wp.y - oy) - kMarkerSize / 2;

    // Clip the marker to the content area. If the marker would be drawn
    // outside the viewport, hide it — there's no value in showing a dot
    // that's not where the user expects it.
    if (sx < -kMarkerSize || sx >= VIEW_W ||
        sy < -kMarkerSize || sy >= VIEW_H) {
        lv_obj_add_flag(_marker, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(_marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(_marker, sx, sy);
    }

    if (_hudGps) {
        char buf[24];
        uint8_t fq = _gps->fixQuality();
        const char* mode = (fq >= 1) ? "3D" : "2D";
        snprintf(buf, sizeof(buf), "GPS %s %dsat", mode, _gps->satellites());
        lv_label_set_text(_hudGps, buf);
    }
}

// ---- HUD ----

void LvMapScreen::updateHud() {
    if (_hudZoom) {
        char buf[8];
        snprintf(buf, sizeof(buf), "z%d", _zoom);
        lv_label_set_text(_hudZoom, buf);
    }
    if (_hudFollow) {
        lv_label_set_text(_hudFollow, _followGPS ? "FOLLOW" : "MANUAL");
        lv_obj_set_style_text_color(_hudFollow,
            lv_color_hex(_followGPS ? Theme::SUCCESS : Theme::TEXT_SECONDARY), 0);
    }
    // _hudMapset is static (hardcoded for v1) — no update needed.
    // _hudGps is updated in updateMarker(); don't overwrite here.
}

// ---- Pan / zoom primitives ----

void LvMapScreen::panBy(int32_t dxPx, int32_t dyPx) {
    _centerWorldX += dxPx;
    _centerWorldY += dyPx;
    // Any manual pan disables follow-GPS. (The 'c' key re-arms it.)
    _followGPS = false;
}

void LvMapScreen::clampZoom() {
    if (_zoom < ZOOM_MIN) _zoom = ZOOM_MIN;
    if (_zoom > ZOOM_MAX) _zoom = ZOOM_MAX;
}

void LvMapScreen::zoomIn() {
    int prevZoom = _zoom;
    _zoom++;
    clampZoom();
    if (_zoom == prevZoom) return;  // already at max

    // Scale the center so the viewport center stays put. This is the
    // common slippy-map feel for keyboard/button zoom (no anchor cursor
    // available — map screen doesn't use a focus group).
    if (prevZoom >= 0 && _zoom > prevZoom) {
        double ratio = (double)(1ULL << _zoom) / (double)(1ULL << prevZoom);
        _centerWorldX = (int64_t)((double)_centerWorldX * ratio);
        _centerWorldY = (int64_t)((double)_centerWorldY * ratio);
    }

    _followGPS = false;
}

void LvMapScreen::zoomOut() {
    int prevZoom = _zoom;
    _zoom--;
    clampZoom();
    if (_zoom == prevZoom) return;

    if (prevZoom > 0 && _zoom < prevZoom) {
        double ratio = (double)(1ULL << _zoom) / (double)(1ULL << prevZoom);
        _centerWorldX = (int64_t)((double)_centerWorldX * ratio);
        _centerWorldY = (int64_t)((double)_centerWorldY * ratio);
    }

    _followGPS = false;
}