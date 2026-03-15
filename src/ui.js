/*
 * ui.js — Norns screen renderer for Move's 128x64 OLED (tool mode)
 *
 * In tool mode, this module takes full control of Move's display and
 * receives all MIDI input. Screen data comes from the DSP plugin via
 * host_module_get_param("screen_data") as a 1-bit monochrome hex string.
 *
 * MIDI from Move hardware is forwarded to the DSP plugin, which writes it
 * to a FIFO for norns-input-bridge to translate into Norns encoder/key events.
 *
 * Knobs 1-3 (CC 71-73) → Norns encoders E1/E2/E3
 * Knob 4/5/6 taps (Notes 3/4/5) → Norns keys K1/K2/K3
 * Knob 7 tap (Note 6) → Restart Norns
 * Back (double-press) → Exit to tools menu
 */

var status = "starting";
var tickCount = 0;
var spinner = ["-", "/", "|", "\\"];
var backPressTime = 0;
var BACK_CONFIRM_MS = 1500;
var STARTUP_GUARD_TICKS = 300; /* ignore restart/exit for first 5 seconds */

/* ── Pad MIDI note mapping ──────────────────────────────── */
var padOctave = 3;          /* base octave (C3 = MIDI 48) */
var PAD_NOTE_START = 68;    /* first pad note from Move hardware */
var PAD_NOTE_END = 99;      /* last pad note */
var PAD_LED_ROOT = 125;     /* blue for root notes (C) */
var PAD_LED_OTHER = 118;    /* light grey for other notes */
var PAD_LED_OFF = 0;        /* black */
var padLedsInitialized = false;

/* ── Grid emulator state ────────────────────────────────── */
var gridMode = false;          /* false=MIDI keys, true=grid */
var gridQuadX = 0;             /* 0=left cols (0-7), 1=right cols (8-15) */
var gridQuadY = 0;             /* 0=bottom rows (0-3), 1=top rows (4-7) */
var MUTE_CC = 88;              /* Mute button toggles grid mode */
var GRID_COLORS = [
    /* [off, dim, med, bright] per quadrant */
    [0, 45, 46, 125],   /* Q1 (BL): blue family */
    [0, 17, 18,   8],   /* Q2 (BR): green family */
    [0,  1,  2, 127],   /* Q3 (TL): red family */
    [0, 81, 82, 120],   /* Q4 (TR): purple/white family */
];
var gridModeFlashTicks = 0;    /* brief OLED flash on mode switch */

/* ── Screen dithering controls ──────────────────────────── */
var ditherMode = 0;            /* 0=off, 1=row-invert, 2=word-invert, 3=floyd-steinberg, 4=bayer, 5=atkinson, 6=cursor, 7=hi-con */
var ditherThreshold = 3;       /* brightness cutoff 0-15 */
var DITHER_NAMES = ["OFF", "ROW INV", "WORD INV", "F-S DITH", "BAYER", "ATKINSON", "CURSOR", "HI-CON"];
var ditherFlashTicks = 0;      /* brief OLED flash on change */
var ditherFlashText = "";

console.log("[norns-ui] init loading");

globalThis.init = function() {
    console.log("[norns-ui] init called");
};

globalThis.tick = function() {
    tickCount++;

    try {
        clear_screen();
    } catch (e) {
        console.log("[norns-ui] clear_screen error: " + e);
        return;
    }

    try {
        var hexData = host_module_get_param("screen_data");
        if (hexData && hexData.length === 2048) {
            renderNornsScreen(hexData);
        } else {
            renderStatus();
        }
    } catch (e) {
        console.log("[norns-ui] tick error: " + e);
        renderStatus();
    }

    /* Poll grid LEDs in grid mode (~20Hz) */
    if (gridMode && tickCount % 3 === 0) {
        updateGridPadLEDs();
    }

    /* Show mode switch flash on OLED */
    if (gridModeFlashTicks > 0) {
        gridModeFlashTicks--;
        print(90, 0, gridMode ? "GRID" : "KEYS", 1);
    }

    /* Show dither setting flash on OLED */
    if (ditherFlashTicks > 0) {
        ditherFlashTicks--;
        print(0, 56, ditherFlashText, 1);
    }
};

function renderNornsScreen(hexData) {
    /* Decode 1-bit monochrome: 8 pixels per byte, MSB = leftmost pixel.
     * 128x64 / 8 = 1024 bytes = 2048 hex chars. */
    for (var y = 0; y < 64; y++) {
        for (var x = 0; x < 128; x++) {
            var byteIdx = y * 16 + Math.floor(x / 8);
            var hexIdx = byteIdx * 2;
            var byteVal = parseInt(hexData.substr(hexIdx, 2), 16);
            var bit = (byteVal >> (7 - (x % 8))) & 1;
            if (bit) {
                set_pixel(x, y, 1);
            }
        }
    }
}

function renderStatus() {
    if (tickCount % 60 === 0) {
        try {
            var s = host_module_get_param("status");
            if (s) status = s;
        } catch (e) {
            /* DSP not ready yet */
        }
    }

    print(0, 0, "NORNS", 1);

    var sp = spinner[Math.floor(tickCount / 10) % 4];
    if (status === "running" || status === "receiving") {
        print(0, 12, "Running " + sp, 1);
    } else if (status === "starting") {
        print(0, 12, "Starting " + sp, 1);
    } else {
        print(0, 12, status, 1);
    }

    print(0, 28, "Maiden:", 1);
    print(0, 38, " http://move.local", 1);
    print(0, 48, "   :5000", 1);
}

/* ── Pad MIDI helpers ────────────────────────────────────── */

function padToMidiNote(padNote) {
    /* Map Move pad note (68-99) to chromatic MIDI note.
     * Pads are 4 rows of 8, bottom-left = lowest note.
     * padIndex 0 = bottom-left, 31 = top-right. */
    var padIndex = padNote - PAD_NOTE_START;
    return (padOctave * 12) + padIndex;
}

function updatePadLEDs() {
    /* Light up pads: blue for root notes (C), grey for others */
    for (var i = 0; i < 32; i++) {
        var midiNote = (padOctave * 12) + i;
        var color;
        if (midiNote > 127) {
            color = PAD_LED_OFF;
        } else if (midiNote % 12 === 0) {
            color = PAD_LED_ROOT;  /* C notes = blue */
        } else {
            color = PAD_LED_OTHER; /* others = grey */
        }
        try {
            move_midi_internal_send([0x09, 0x90, PAD_NOTE_START + i, color]);
        } catch (e) {}
    }
}

/* ── Grid emulator helpers ──────────────────────────────── */

function getGridQuadrant() {
    return gridQuadY * 2 + gridQuadX;  /* 0=BL, 1=BR, 2=TL, 3=TR */
}

function brightnessToColor(brightness, quadrant) {
    var colors = GRID_COLORS[quadrant];
    if (brightness === 0) return colors[0];
    if (brightness <= 4)  return colors[1];
    if (brightness <= 10) return colors[2];
    return colors[3];
}

function updateGridPadLEDs() {
    var quadrant = getGridQuadrant();
    var colOff = gridQuadX * 8;
    var rowOff = gridQuadY * 4;
    try {
        var hexData = host_module_get_param("grid_leds");
        for (var row = 0; row < 4; row++) {
            for (var col = 0; col < 8; col++) {
                var gx = colOff + col;
                var gy = rowOff + row;
                var brightness = 0;
                if (hexData && hexData.length === 256) {
                    var idx = (gy * 16 + gx) * 2;
                    brightness = parseInt(hexData.substr(idx, 2), 16);
                }
                var color = brightnessToColor(brightness, quadrant);
                var padNote = PAD_NOTE_START + row * 8 + col;
                try {
                    move_midi_internal_send([0x09, 0x90, padNote, color]);
                } catch (e) {}
            }
        }
    } catch (e) {}
}

globalThis.onMidiMessageInternal = function(data) {
    if (!data || data.length < 3) return;

    var statusByte = data[0] & 0xF0;
    var d1 = data[1];
    var d2 = data[2];

    /* Log first few MIDI messages for debugging */
    if (tickCount < 600) {
        console.log("[norns-ui] MIDI: " + statusByte.toString(16) + " " + d1 + " " + d2);
    }

    /* Ignore navigation/restart during startup */
    if (tickCount < STARTUP_GUARD_TICKS) {
        /* Still forward knob CCs for encoders */
        if (statusByte === 0xB0 && d1 >= 71 && d1 <= 73) {
            try { host_module_send_midi(data, "internal"); } catch (e) {}
        }
        return;
    }

    /* Back button (CC 51): double-press to exit */
    if (statusByte === 0xB0 && d1 === 51 && d2 > 0) {
        var now = Date.now();
        if (backPressTime > 0 && (now - backPressTime) < BACK_CONFIRM_MS) {
            backPressTime = 0;
            if (typeof host_exit_module === "function") host_exit_module();
        } else {
            backPressTime = now;
        }
        return;
    }

    /* Knob 8 tap (Note 7): restart Norns (double-tap required) */
    if (statusByte === 0x90 && d1 === 7 && d2 > 0) {
        var now = Date.now();
        if (backPressTime > 0 && (now - backPressTime) < BACK_CONFIRM_MS) {
            backPressTime = 0;
            try {
                host_module_set_param("restart", "1");
            } catch (e) {
                console.log("[norns-ui] restart error: " + e);
            }
        } else {
            backPressTime = now;
        }
        return;
    }

    /* Initialize pad LEDs on first real MIDI message */
    if (!padLedsInitialized && tickCount > STARTUP_GUARD_TICKS) {
        padLedsInitialized = true;
        if (gridMode) { updateGridPadLEDs(); } else { updatePadLEDs(); }
    }

    /* Mute button (CC 88) → toggle grid/MIDI keys mode */
    if (statusByte === 0xB0 && d1 === MUTE_CC && d2 > 0) {
        gridMode = !gridMode;
        gridModeFlashTicks = 60;
        if (gridMode) { updateGridPadLEDs(); } else { updatePadLEDs(); }
        return;
    }

    /* Pads (notes 68-99) — mode-aware */
    if ((statusByte === 0x90 || statusByte === 0x80) &&
        d1 >= PAD_NOTE_START && d1 <= PAD_NOTE_END) {
        var padIndex = d1 - PAD_NOTE_START;
        var padRow = Math.floor(padIndex / 8);
        var padCol = padIndex % 8;

        if (gridMode) {
            /* Grid mode: map pad to grid coordinates */
            var gx = gridQuadX * 8 + padCol;
            var gy = gridQuadY * 4 + padRow;
            var state = (statusByte === 0x90 && d2 > 0) ? 1 : 0;
            try {
                host_module_set_param("grid_key", gx + " " + gy + " " + state);
            } catch (e) {}
        } else {
            /* MIDI keys mode: chromatic note mapping */
            var midiNote = padToMidiNote(d1);
            if (midiNote > 127) return;
            var vel = d2;
            var noteStatus = statusByte;
            if (statusByte === 0x90 && vel === 0) noteStatus = 0x80;
            try {
                var hex = noteStatus.toString(16) + " " +
                          midiNote.toString(16) + " " + vel.toString(16);
                host_module_set_param("midi_in", hex);
            } catch (e) {}
        }
        return;
    }

    /* Up arrow (CC 55) */
    if (statusByte === 0xB0 && d1 === 55 && d2 > 0) {
        if (gridMode) {
            gridQuadY = 1;
            updateGridPadLEDs();
        } else {
            if (padOctave < 8) { padOctave++; updatePadLEDs(); }
        }
        return;
    }

    /* Down arrow (CC 54) */
    if (statusByte === 0xB0 && d1 === 54 && d2 > 0) {
        if (gridMode) {
            gridQuadY = 0;
            updateGridPadLEDs();
        } else {
            if (padOctave > 0) { padOctave--; updatePadLEDs(); }
        }
        return;
    }

    /* Right arrow (CC 63) — grid quadrant navigation */
    if (statusByte === 0xB0 && d1 === 63 && d2 > 0) {
        if (gridMode) { gridQuadX = 1; updateGridPadLEDs(); }
        return;
    }

    /* Left arrow (CC 62) — grid quadrant navigation */
    if (statusByte === 0xB0 && d1 === 62 && d2 > 0) {
        if (gridMode) { gridQuadX = 0; updateGridPadLEDs(); }
        return;
    }

    /* Knob 6 (CC 76) — adjust dither threshold */
    if (statusByte === 0xB0 && d1 === 76) {
        var delta = 0;
        if (d2 >= 1 && d2 <= 63) delta = 1;
        else if (d2 >= 65) delta = -1;
        if (delta !== 0) {
            ditherThreshold = Math.max(0, Math.min(15, ditherThreshold + delta));
            try { host_module_set_param("dither_threshold", "" + ditherThreshold); } catch (e) {}
            ditherFlashText = "THRESH:" + ditherThreshold;
            ditherFlashTicks = 60;
        }
        return;
    }

    /* Knob 7 (CC 77) — cycle dither mode */
    if (statusByte === 0xB0 && d1 === 77) {
        var delta = 0;
        if (d2 >= 1 && d2 <= 63) delta = 1;
        else if (d2 >= 65) delta = -1;
        if (delta !== 0) {
            ditherMode = (ditherMode + delta + 8) % 8;
            try { host_module_set_param("dither_mode", "" + ditherMode); } catch (e) {}
            ditherFlashText = DITHER_NAMES[ditherMode];
            ditherFlashTicks = 60;
        }
        return;
    }

    /* Forward other MIDI to DSP plugin via set_param (host_module_send_midi
     * doesn't work in shadow/tool mode). DSP plugin parses and writes to FIFO. */
    try {
        var hex = (statusByte | (data[0] & 0x0F)).toString(16);
        hex += " " + d1.toString(16);
        hex += " " + d2.toString(16);
        host_module_set_param("midi_in", hex);
    } catch (e) {
        /* ignore */
    }
};

globalThis.onMidiMessageExternal = function(data) {
    /* Forward external MIDI to DSP plugin (for norns MIDI device input) */
    try {
        host_module_send_midi(data, "external");
    } catch (e) {
        /* ignore */
    }
};
