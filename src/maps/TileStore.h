#pragma once

// =============================================================================
// TileStore.h — path building + chunked SD access for slippy-map PNG tiles
// =============================================================================
//
// The on-disk layout (confirmed against the user's actual SD card, matches
// Meshtastic MUI's MUIMapBuilder convention) lives at the SD card ROOT, not
// under a /maps/ prefix, and nests x/y/z (NOT the more common z/x/y order):
//      /<mapset>/<x>/<y>/<z>.png
// e.g. /Basemapsxyz-OSM/512/340/10.png
//
// `mapset` is a free-form style name (e.g. "Basemapsxyz-OSM", "Esri-Satellite")
// selectable in the UI. `x`, `y`, `z` are tile-coordinate integers (y=0 is the
// north pole — see SlippyMath.h for the TMS-y-flip troubleshooting note).
//
// TileStore deliberately does NOT cache, decode, or render anything. It is a
// thin SD facade used by TileCache, which owns the decode state machine.
// All methods take an SDStore& for the actual SD access; the class is purely
// path/namespace glue so it's testable without an SDStore argument too.
// =============================================================================

#include <Arduino.h>
#include <SD.h>
#include "storage/SDStore.h"

class TileStore {
public:
    // Build the absolute path string for a tile. The returned String is owned
    // by the caller; it is short-lived (just enough for the SD file open).
    // Layout: /<mapset>/<x>/<y>/<z>.png (SD root, x/y/z order — see header note).
    static String tilePath(const char* style, int z, int x, int y) {
        String p = "/";
        p += style;
        p += '/';
        p += x;
        p += '/';
        p += y;
        p += '/';
        p += z;
        p += ".png";
        return p;
    }

    // True if the tile file exists on SD. Used by the negative-cache check
    // when we haven't yet recorded a miss for this (z,x,y).
    static bool tileExists(SDStore& sd, const char* style, int z, int x, int y) {
        String p = tilePath(style, z, x, y);
        return sd.exists(p.c_str());
    }

    // Open a tile file for incremental chunked reading. Caller MUST close().
    // Returns an invalid File if SD is not ready or the file doesn't exist.
    static File openTile(SDStore& sd, const char* style, int z, int x, int y) {
        String p = tilePath(style, z, x, y);
        return sd.openFile(p.c_str());
    }

    // Size in bytes of the tile file, or 0 if it can't be opened/measured.
    static size_t tileSize(SDStore& sd, const char* style, int z, int x, int y) {
        File f = openTile(sd, style, z, x, y);
        if (!f) return 0;
        size_t s = f.size();
        f.close();
        return s;
    }
};
