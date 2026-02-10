# Triple-Buffer LVGL Architektur für ESP32-S3

## Display: 720×720 RGB565 Parallel (16-bit, 24 MHz PCLK)

## Architektur

```
┌─────────────────────────────────────────────────────────────────┐
│ PSRAM (8 MB)                                                    │
│                                                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │  Work Buffer  │  │  Back Buffer  │  │ Front Buffer  │         │
│  │   (~1 MB)     │  │   (~1 MB)     │  │   (~1 MB)     │         │
│  │              │  │              │  │              │          │
│  │  LVGL rendr. │  │  GDMA Ziel   │  │  LCD_CAM     │          │
│  │  hier rein   │  │              │  │  liest hier  │          │
│  └──────┬───────┘  └──────▲───────┘  └──────┬───────┘          │
│         │                 │                 │                   │
│         │    GDMA Copy    │    Ptr Swap     │                   │
│         └─────────────────┘    ◄────────────┘                   │
│                                                                 │
│  Verbleibend: ~5 MB für App-Daten, Bilder, Fonts              │
└─────────────────────────────────────────────────────────────────┘
                                                    │
                                           LCD_CAM DMA
                                                    │
                                                    ▼
                                        ┌──────────────────┐
                                        │  720×720 Display  │
                                        │  16-bit Parallel   │
                                        └──────────────────┘
```

## Frame-Pipeline

```
Zeit ──────────────────────────────────────────────────────►

Frame N:
CPU:     ├── LVGL render (work_buf) ──┤
GDMA:                                  ├── copy work→back ──┤
LCD_CAM: ├────── zeigt front_buf ──────────────────────────────────┤
                                                             swap!

Frame N+1:
CPU:                                                         ├── LVGL render ──┤
GDMA:                                                                           ├── copy ──┤
LCD_CAM:                                                     ├── zeigt neuen front ────────────┤
                                                                                          swap!
```

## Warum Triple-Buffer nötig ist

### Problem mit Double-Buffer:
LVGL schreibt **inkrementell** - nur dirty regions pro Flush.
Wenn Back Buffer direkt angezeigt würde während LVGL noch rendert,
sieht man halb-fertige Frames (Tearing/Flickering).

### Lösung Triple-Buffer:
- **Work Buffer**: LVGL's privater Arbeitsbereich. Wird NIE angezeigt.
  LVGL baut hier über mehrere Flushes den kompletten Frame zusammen.
- **Back Buffer**: Empfängt die fertige Kopie vom Work Buffer.
  Wird erst nach vollständiger Kopie zum Front Buffer.
- **Front Buffer**: LCD_CAM liest kontinuierlich von hier.
  Wird nur per Pointer-Swap getauscht → kein Tearing.

## Geschätzte Performance

| Phase                | Dauer    | Blockiert CPU? |
|----------------------|----------|----------------|
| LVGL Render (dirty)  | 5–15 ms  | Ja             |
| GDMA Copy (1 MB)     | ~17 ms   | Nein           |
| LCD Refresh (24 MHz) | ~22 ms   | Nein           |
| **Gesamt (pipelined)** | **~25–35 ms** | |
| **Erwartete FPS**    | **28–40 FPS** | |

## Optimierungsmöglichkeiten

1. **PSRAM auf 120 MHz overclocken** → GDMA Copy schneller (~12 ms statt 17 ms)
2. **Nur dirty regions kopieren** statt ganzen Buffer → deutlich schneller
3. **LVGL auf Core 1, GDMA auf Core 0** → echte Parallelität
4. **Bounce Buffer** für LCD_CAM → reduziert PSRAM Bus-Contention

## Wichtige Hinweise

- Buffer müssen **64-Byte aligned** sein (Cache-Line Alignment)
- GDMA Callback läuft im **ISR Kontext** → keine blockierenden Aufrufe
- `lv_disp_drv.direct_mode = 1` ist **essentiell** → LVGL schreibt direkt
  an die richtigen Pixel-Positionen im Work Buffer
- `esp_lvgl_port` wird **NICHT** verwendet → eigener flush_cb
