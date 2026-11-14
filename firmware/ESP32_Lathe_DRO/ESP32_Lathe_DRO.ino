/*
=======================================================================================
PROJECT:        ESP32 Lathe DRO (Digital Read Out) - High Performance Multi-Axis System
VERSION:        v1.0 (Production Standard - Technical Documentation Header)
AUTHOR:         Anton CNC (https://www.youtube.com/@boessi)
PLATFORM:       ESP32 (Arduino Framework)
COMPILER:       PlatformIO / Arduino IDE
DATE:           Oct 2026
=======================================================================================

■ LICENSE AND DISCLAIMER
------------------------
This software is provided as free and open-source software "as is", without any 
warranty of any kind, express or implied. The author shall not be held liable for 
any damage, malfunction, data loss, financial loss, or any other consequences 
arising from the use of this software. Use of this software is entirely at your own 
risk. This software is intended for educational, hobby, and experimental use, 
especially in machine tool environments. Proper safety precautions must always be 
observed. The user is responsible for verifying correct wiring, signal levels, and 
machine operation at all times. By using this software, you agree to these terms.

=======================================================================================

■ 1. SYSTEM ARCHITECTURE OVERVIEW
---------------------------------
• CORE 1 (LOOP): Real-time processing of quadrature signals via PCNT (Pulse Counter) units.
  Math engine calculates positions, tool offsets, and diameter modes.
• CORE 0 (NETWORK): AsyncWebServer and WebSocket task. Handles UI communication.
• SYNCHRONIZATION: Mutex/Semaphore protection for global variables shared between cores.
  Ensure any modification to shared variables is protected by critical sections.
=======================================================================================

■ 2. CRITICAL SOFTWARE & TOOLCHAIN REQUIREMENTS
----------------------------------------------
• BOARD MANAGER CORE: ESP32 by Espressif Systems v2.0.14  <-- CRITICAL RECOMMENDATION
  NOTE: Do NOT upgrade to Core v3.x (e.g., v3.3.8). Version 3.x introduces breaking 
  changes that completely remove the legacy hardware PCNT driver API used here and 
  causes panic crashes/instabilities within the AsyncTCP network stack.

• VERIFIED LIBRARIES USED:
  - WiFi.h              (Internal ESP32 Core)
  - AsyncTCP.h         (v1.1.4 - Optimized for ESP32 v2.x Core)
  - ESPAsyncWebServer.h (v3.1.0 - Stable Asynchronous Web Engine)
  - driver/pcnt.h      (Native ESP32 Hardware PCNT Low-Level Driver)

=======================================================================================

■ 3. CORE ARCHITECTURE & SYSTEM FEATURES
---------------------------------------
• Hardware Counting (PCNT): Offloads raw quadrature pulse tracking to the ESP32's 
  dedicated Hardware Pulse Counter peripheral. Decoding happens entirely within the 
  silicon across 4x Quadrature Mode (evaluating every edge of Phase A & B). For a 
  5µm magnetic tape, this delivers a fixed resolution of exactly 200 pulses/mm, 
  bypassing software pin-interrupt overhead and keeping CPU load at 0%.
    
• Native Low-Level High-Priority ISR: The PCNT silicon uses 16-bit signed hardware 
  registers, creating a strict boundary at -32,768 to +32,767 counts. At 200 pulses/mm, 
  the counter hits its 30,000 threshold (`PCNT_LIMIT`) at exactly 150 mm of travel.
  To prevent register freezes or overflow data corruption, the firmware deploys the
  native Espressif low-level driver (`pcnt_isr_service_install`) with the 
  `ESP_INTR_FLAG_IRAM` flag. This anchors the overflow routine inside ultra-fast IRAM, 
  instantly cutting through any background operations (like Wi-Fi or WebSockets) in 
  a fraction of a microsecond to flush the hardware block into 32-bit RAM storage.
    
• Process Isolation & Multicore Tasking: Managed under FreeRTOS across both cores:
  - [ CORE 0 ]: Runs the local Wi-Fi Access Point, hosts the AsyncWebServer, maintains 
    AsyncWebSocket data channels, interprets smartphone/tablet inputs (responsive layout, 
    tuned for Android/Pixel 9 as well as iPad), and transmits JSON telemetry data at a 
    throttled 20 Hz (every 50ms) whenever a tracked value changed by more than 0.005mm, to 
    ensure smooth rendering without network lag. The JSON packet always contains all four 
    axes (X/Z/U/W) regardless of axis_mode; the client-side UI is what decides which rows 
    to actually display.
  - HEARTBEAT: Independent of axis movement, a broadcast is forced at least once per 
    second even while the machine sits idle. This lets the client's connection watchdog 
    (see Section 6) tell "nothing is moving" apart from "the link is down" - without it, a 
    dropped connection while the lathe is stationary would go unnoticed until the operator 
    moved an axis again.
  - [ CORE 1 ]: Runs the deterministic system math engine (`loop()`). Continuously polls 
    combined hardware/software counts, pulls offsets from the indexed tool matrices based 
    on the active profile, handles lathe-specific modes, and writes coordinates into shared 
    RAM via a tight 5ms Mutex barrier (`positionMutex`).
      
• Advanced EMC / Signal Conditioning (Industrial Grade):
  - Physical Hardware: Outfitted with an upstream 74LVC14A Hex Inverting Schmitt Trigger 
    IC to condition noisy, degraded signals from magnetic scales or glass scales into clean, 
    sharp 3.3V TTL square waves, safeguarding the ESP32 GPIO inputs (5V tolerant logic conversion).
  - Digital Filter: Hardware glitch filtering (`pcnt_set_filter_value`) is active on all 
    channels. Configured to a safety value of 100 clock cycles. Any noise spike shorter 
    than 1,250 ns (1.25 µs) is completely discarded by the silicon, providing immense EMV 
    immunity against workshop motor interference without sacrificing tracking speed (up to 4,000 mm/s).

• NVS Integrity & Sanity Validation (Anti-Crash Barrier):
  - The firmware implements an active memory validation pass during the initialization stage. 
    Upon loading tool offset configuration blocks from Flash (NVS), every floating-point variable 
    is filtered via a hardware-level `isnan()` check. If corrupted memory blocks, uninitialized 
    sectors, or true 'NaN' (Not a Number) states are detected, the system intercepts the fault, 
    discards the corruption, and enforces a safe fallback state of 0.00mm. This strictly prevents 
    cascading mathematical invalidation inside the Core 1 execution thread.

=======================================================================================

■ 4. HARDWARE & INTERFACE CONNECTIONS (SUB-D 9 PINOUT)
------------------------------------------------------
The system expects clean TTL level signals (Channel A / Channel B) from magnetic reader 
heads or optical glass scales. The physical wiring uses standard DB9/SUB-D connectors:

  Pin 2  → GND (Signal Ground 0V)
  Pin 4  → GND (Shield / Core Wire Ground)
  Pin 6  → Channel A (TTL signal routed to 74LVC14A)
  Pin 7  → +5V Power Supply for Linear Scales
  Pin 8  → Channel B (TTL signal routed to 74LVC14A)

NOTE: Pins 2 and 4 are tied together as common Ground. Always verify voltage levels before 
connecting hardware. Swapping Channel A and Channel B will invert the physical counting direction.

=======================================================================================

■ 5. KEY VARIABLE DICTIONARY
--------------------------
• global_total_x/z/u/w (volatile int32_t): 32-bit accumulators updated in IRAM by the 
  high-priority ISR to track consolidated hardware overflow blocks of +/- 30,000 counts.
  Pre-loaded during setup() with saved NVS positions to guarantee absolute state restore.
• shared_x/z/u/w (float): Final calculated millimeter coordinates (32-bit IEEE-754). Computed 
  by Core 1 and read by Core 0 under strict binary Mutex protection. Maintains absolute 
  0.01mm resolution without precision loss for distances up to 83,886.08 mm (83.8 meters).
• offset_..._ABS[10] / offset_..._INC[10] (float arrays): 10-element multidimensional 
  offset matrices tracking separate absolute machine datums and relative incremental 
  workspace profiles uniquely for 10 independent tools (T1 to T10). Guarded by boot-up validation.
• current_tool (int): Active tool profile index (0 to 9, represented as T1-T10 in UI).
  NOTE: T1 serves as the 'Master Tool'. In ABS mode, all offsets are stored relative 
  to T1's datum. Initializing T1 after a system reboot/reset ensures a consistent 
  calibration basis for all other tools (T2-T10). 
  In INC mode, offsets act as independent, transient coordinate shifts and do not 
  rely on the T1-master geometry.
• X/Z/U/W_DIR (int): Multipliers holding 1 or -1 to control physical axis counting 
  polarity. Saved permanently into Non-Volatile Flash Storage (NVS / Preferences).
• diameter_mode (bool): Lathe cross-slide flag. Doubles the displayed X-axis readout 
  to represent workpiece diameter rather than physical radius travel when set to true.
• is_inc_mode_x/z/u/w (bool): Per-axis workspace flags. Each axis toggles independently between 
  ABS (false) and INC (true), letting e.g. Z run in chain-dimension mode while X stays absolute.
• axis_mode (int): UI footprint configuration profile (2, 3, or 4 axes). Retained in NVS.
• unit_inch (bool): Display unit preference (false=mm, true=inch). Retained in NVS. Purely 
  a rendering choice for the client - calibration (`counts_per_mm`) always stays metric.
• positionMutex (SemaphoreHandle_t): RTOS kernel lock guard protecting cross-core variables.

=======================================================================================

■ 6. OPERATING & USER INSTRUCTIONS
--------------------------------
• Connection: Connect your smartphone (e.g. Pixel 9) or tablet to Wi-Fi AP 
  "AntonCNC_ESP32-DRO" (Pass: 12345678). Open Chrome/Safari and navigate to 
  http://192.168.4.1 to launch the real-time web interface. Portrait orientation is 
  recommended on phones (more vertical space for the axis rows).
• Connection Status: A small dot next to the title shows live link health - green means 
  telemetry is arriving normally, red means no packet has been received for >3 seconds 
  (WLAN dropped, ESP32 rebooted, etc.). When red, the X/Z/U/W values dim and a banner 
  reading "CONNECTION LOST" appears at the very top. Tap that banner to reload the page. 
  There is intentionally NO automatic reconnect - a silent auto-retry could mask a real 
  problem, so the indicator stays red until you deliberately reload.
• Tool Selection: Tap [ T1 ] to [ T10 ] in the bottom toolbar to switch active tool 
  profiles. The DRO instantly recalculates positions using the selected tool's independent 
  offset array. NOTE: T1 is the 'Master/Reference Tool' - in ABS mode, every other tool's 
  offset is stored relative to T1's datum, so (re-)touching off T1 after a reboot/reset is 
  what re-establishes a consistent calibration basis for T2-T10.
• Zeroing [ Z ]: Tap red [ Z ] next to an axis --> Confirm via [ YES ] overlay. Clears or 
  zeroes the coordinate *only* for the currently active tool profile.
• Value Entry [ I ]: Tap orange [ I ] to open the touch numpad. Input desired coordinate 
  dimension (supports negative entry using +/-) and tap [ SET ]. Applies to the active tool.
• NVS FLASH SAVE: Press and hold the blue [ SAVE ] button for about 1 second until the
  confirmation tone sounds (a short tap does nothing). This commits all 10 tool offset
  configurations, the active tool index, the per-axis ABS/INC state, and - critically -
  the raw hardware scale counter states to NVS. Upon reboot, the system restores its
  coordinate space as of the last SAVE. (Axis direction, unit and axis count are written
  to NVS immediately when changed; scale resolutions via [ SAVE RESOLUTIONS ].)
• Per-Axis ABS/INC Workspace: Tap the axis letter itself (X/Z/U/W). Green = ABS 
  (absolute, referenced to the tool's touch-off datum), yellow = INC (incremental chain- 
  dimensioning). Each axis switches completely independently - e.g. Z can run in INC mode 
  for a series of chained cuts while X stays in ABS mode showing the true diameter.
• Radius/Diameter [ R / D ]: Tap [ R ] on the X-axis row. Switches layout into Diameter mode 
  [ D ] (turns yellow), multiplying the display x2 to read true part diameters directly.
• Compound Slide Helper [ ∠ ]: Tap the [ ∠ ] button on the U-axis row (taper/thread turning 
  helper). Opens an overlay showing the current compound slide angle (tap to edit via the 
  numpad, 0-90 deg, plus a separate +/- button for swivel direction), the raw U reading, 
  and the resulting Delta X (radius) / Delta Z the compound movement projects onto the real 
  machine axes - each independently zeroable. "X combined" / "Z combined" show the resulting 
  true tool tip position (current tool's X/Z plus the compound deltas). Everything here is 
  calculated client-side only and is NOT saved anywhere - angle, sign, and both zero offsets 
  reset to 0/+ on every page reload or ESP32 restart, by design. On tablets, press and hold 
  the "COMPOUND SLIDE" title to drag the overlay out of the way.
• Display Unit: Open Settings (see below) to switch the readout between MM (2 decimals) and 
  INCH (4 decimals / ten-thousandths). This choice is persisted in NVS. Calibration itself 
  (Linear Scale Resolution) always stays in mm/µm regardless of this setting - only the 
  on-screen numbers convert.
• Sound & Feedback System: Confirms user inputs via discrete frequencies:
  - SET     → short high-frequency click
  - ZERO    → deeper confirmation click
  - MODE    → medium tone
  - NUMPAD  → short click with subtle pitch variation
  - CONNECTION LOST → a single low, drawn-out warning tone (not repeated)
• Settings Menu [ ⚙ ]: Press and hold the gear icon in the header for 0.8 seconds to 
  open it (the same deliberate long-press safety pattern used elsewhere, to prevent an 
  accidental open while operating the lathe). Once open, a quick tap on the gear - or the 
  [ CLOSE ] button at the very bottom of the panel - closes it again. Sections, top to 
  bottom:
  - Display Unit: MM / INCH toggle, see above.
  - Displayed Axes: [ 2X ] / [ 3X ] / [ 4X ] - press and hold for 1.0 second to reallocate 
    active PCNT units and change the UI layout. Moved here because it is a one-time setup 
    choice, not something changed during normal operation.
  - Axis Direction: One direction indicator (⟲) per currently active axis. Press and hold 
    for 0.8 seconds to invert that axis' counting polarity (written to NVS immediately).
    Also a one-time setup choice, so it lives here rather than cluttering the main screen.
  - Linear Scale Resolutions: Each axis can be independently calibrated (default: 200 
    pulses/mm for 5µm scales). Choose from presets (5µm or 1µm) or select "Custom..." to 
    input specific pulses/mm values manually for micro-calibration. [ SAVE RESOLUTIONS ] 
    commits new scaling factors to NVS and applies them instantly, no reboot required.
=======================================================================================

■ 7. ANDROID / PIXEL 9 / IPAD APP MODE OPTIMIZATION (CRITICAL FOR MACHINE USE)
----------------------------------------------------------
The UI is fluid/responsive (clamp()/vw-based), tuned for the Pixel 9's ~412x915 CSS-pixel
portrait viewport, but scales to other Android phones, to landscape, and - via its own
dedicated breakpoint (~700-1400 CSS-px, both orientations) - to iPads as well, with
larger touch targets and an enlarged Compound Slide overlay that can be dragged by its
title bar (see Section 6).
To turn the web interface into a fullscreen standalone app without browser UI:
1. Open http://192.168.4.1 in Chrome (Android) or Safari (iPad) on the device.
2. Tap the browser menu and select "Add to Home screen" / "Install app".
3. Launch the DRO exclusively via the new Home Screen icon for a minimal-chrome window
    (<meta name="mobile-web-app-capable" content="yes"> and theme-color support this on
    Android; <meta name="apple-mobile-web-app-capable" content="yes"> covers iPad).
4. SCREEN AWAKE LOCK: To prevent the display from sleeping during machining operations, 
    go to Settings → Display → Screen/Auto-Lock timeout and set it to the maximum value 
    (or enable Android Developer Options → "Stay awake" while charging).
5. OPTIONAL: Enable Android "Pin app" / "App pinning" or iPadOS "Guided Access" 
    (Settings → Accessibility) to lock the device securely into the DRO application.

=======================================================================================

■ 8. HARDWARE TRACKING VALIDATION TEST
------------------------------------
• To verify that your physical Schmitt-Trigger layout and high-speed IRAM ISR are running 
  without step losses: Pin the axis carriage against a rigid mechanical stop or gauge block. 
  Zero the display. Rapidly crank or move the axis past 200mm to intentionally trigger
  a register overflow at the 150mm limit. Return the axis to the same mechanical
  stop. The display must return precisely to 0.00, proving 100% signal and tracking integrity.
=======================================================================================
*/

#include <WiFi.h>               // Wi-Fi connectivity and access point management
#include <AsyncTCP.h>           // Asynchronous TCP stack for high-performance networking
#include <ESPAsyncWebServer.h>  // Async web server to handle WebSocket and UI requests
#include "driver/pcnt.h"        // ESP32 hardware pulse counter driver for quadrature inputs
#include <Preferences.h>        // Non-volatile storage (NVS) for persistent user settings
#include <cmath>                // Standard math library for calculations
                                // Math library used specifically for isnan() validation of NVS data

// ===================================================================================
// ■ BLOCK: HARDWARE PINOUTS & SYSTEM CONSTANTS
// ===================================================================================
#define X_PIN_A GPIO_NUM_32
#define X_PIN_B GPIO_NUM_33
#define U_PIN_A GPIO_NUM_27
#define U_PIN_B GPIO_NUM_14
#define Z_PIN_A GPIO_NUM_25
#define Z_PIN_B GPIO_NUM_26
#define W_PIN_A GPIO_NUM_19   
#define W_PIN_B GPIO_NUM_18   

#define PCNT_LIMIT 30000 // Threshold for PCNT hardware counter limit (must be < 32767 for signed int16)

const char* ssid = "AntonCNC_ESP32-DRO";  // SSID for the DRO Wi-Fi Access Point
const char* password = "12345678";        // Password for the Wi-Fi Access Point connection

AsyncWebServer server(80);  // HTTP server instance listening on standard port 80
AsyncWebSocket ws("/ws");
Preferences prefs;

// ===================================================================================
// ■ BLOCK: CROSS-CORE DATA ASSET SYNCHRONIZATION
// ===================================================================================
SemaphoreHandle_t positionMutex;  // Mutex to protect shared variables during cross-core access
TaskHandle_t NetworkTaskHandle;   // Handle to manage and monitor the network/webserver task
float shared_x = 0.0, shared_u = 0.0, shared_z = 0.0, shared_w = 0.0; // Final calculated mm coordinates (Core 1 -> Core 0)
volatile int32_t global_total_x = 0;  // Raw accumulated pulse counts for X-axis (updated by ISR)
volatile int32_t global_total_u = 0;
volatile int32_t global_total_z = 0;
volatile int32_t global_total_w = 0;

int current_tool = 0;
// ABSolutive offsets relative to Master Tool (T1 datum)
float offset_x_ABS[10] = {0}, offset_u_ABS[10] = {0}, offset_z_ABS[10] = {0}, offset_w_ABS[10] = {0};
// INCremental offsets for transient, per-tool coordinate shifts
float offset_x_INC[10] = {0}, offset_u_INC[10] = {0}, offset_z_INC[10] = {0}, offset_w_INC[10] = {0};

// Dynamic axis resolutions (Default: 200 pulses/mm)
float counts_per_mm[4] = {200.0f, 200.0f, 200.0f, 200.0f}; 

bool diameter_mode = true;  // Flag: True = Diameter mode (x2 display), False = Radius mode 
bool is_inc_mode_x = false, is_inc_mode_z = false, is_inc_mode_u = false, is_inc_mode_w = false; // Per-axis: True = Incremental, False = Absolute
bool unit_inch = false;     // Flag: True = Display values in inch (converted client-side), False = mm. 
                             // Calibration (counts_per_mm) always stays metric - this only tags the JSON so all connected clients render consistently after a reload.
int axis_mode = 3;          // Configuration: Active axis count (2, 3, or 4 axes)
// ISR flag: Triggers immediate UI refresh beyond standard interval
volatile bool force_extended_update = false;

int X_DIR = 1;  // Direction multiplier for X-axis (1 = Normal, -1 = Inverted)
int U_DIR = 1;
int Z_DIR = 1;
int W_DIR = 1;

// ===================================================================================
// ■ BLOCK: HARDWARE INTERRUPT SERVICE ROUTINE (ISR)
// ===================================================================================

// NOTE: IRAM_ATTR places this code into Internal RAM (IRAM). Mandatory because 
// flash-memory access is disabled during Wi-Fi operations; ISR in Flash would crash.
void IRAM_ATTR pcnt_intr_handler(void *arg) {
    pcnt_unit_t unit = (pcnt_unit_t)(uint32_t) arg;
    uint32_t status = 0;
    pcnt_get_event_status(unit, &status);

    switch (unit) {
        case PCNT_UNIT_0:
            if (status & PCNT_EVT_H_LIM) global_total_x += PCNT_LIMIT;
            if (status & PCNT_EVT_L_LIM) global_total_x -= PCNT_LIMIT;
            break;
        case PCNT_UNIT_1:
            if (status & PCNT_EVT_H_LIM) global_total_z += PCNT_LIMIT;
            if (status & PCNT_EVT_L_LIM) global_total_z -= PCNT_LIMIT;
            break;
        case PCNT_UNIT_2:
            if (status & PCNT_EVT_H_LIM) global_total_u += PCNT_LIMIT;
            if (status & PCNT_EVT_L_LIM) global_total_u -= PCNT_LIMIT;
            break;
        case PCNT_UNIT_3:
            if (status & PCNT_EVT_H_LIM) global_total_w += PCNT_LIMIT;
            if (status & PCNT_EVT_L_LIM) global_total_w -= PCNT_LIMIT;
            break;
        default: break;
    }
}

// Configures quadrature decoding on specific pins and enables overflow interrupts
void setupPCNT(pcnt_unit_t unit, int A, int B, int32_t initial_counts) {
    pinMode(A, INPUT_PULLUP);
    pinMode(B, INPUT_PULLUP);
    pcnt_config_t c = {};
    c.pulse_gpio_num = A;
    c.ctrl_gpio_num = B;
    c.unit = unit;
    c.channel = PCNT_CHANNEL_0;
    c.pos_mode = PCNT_COUNT_INC;
    c.neg_mode = PCNT_COUNT_DEC;
    c.lctrl_mode = PCNT_MODE_REVERSE;
    c.hctrl_mode = PCNT_MODE_KEEP;
    c.counter_h_lim = PCNT_LIMIT;
    c.counter_l_lim = -PCNT_LIMIT;     
    pcnt_unit_config(&c);

    c.pulse_gpio_num = B;
    c.ctrl_gpio_num = A;
    c.channel = PCNT_CHANNEL_1;
    c.pos_mode = PCNT_COUNT_DEC;       
    c.neg_mode = PCNT_COUNT_INC;
    pcnt_unit_config(&c);

    pcnt_set_filter_value(unit, 100);
    pcnt_filter_enable(unit);
    pcnt_event_enable(unit, PCNT_EVT_H_LIM);
    pcnt_event_enable(unit, PCNT_EVT_L_LIM);
    pcnt_counter_clear(unit);
    pcnt_counter_resume(unit);
}

// Merges hardware register value with global accumulation to get 32-bit position
// Returns the current 32-bit position by merging the 16-bit hardware register 
// with the software-managed overflow accumulation.
// Note: hw_counter is int16_t (hardware limit), global_total is int32_t (extended range).
int32_t get_absolute_counts(pcnt_unit_t unit, volatile int32_t &global_total) {
    int16_t hw_counter;
    pcnt_get_counter_value(unit, &hw_counter);  // Fetch current raw count from PCNT register
    return global_total + hw_counter;           // Return full 32-bit absolute position
}


/*
======================================================================================= 
WEBSOCKET JSON INTERFACE DEFINITION
----------------------------------------
• PROTOCOL: { "x", "z", "u", "w": float, "axes": int, "mode": "D"/"R", 
              "ref_x/z/u/w": "INC"/"ABS" (per axis), "dir_x/z/u/w": int, "tool": int, 
              "res_x/z/u/w": float, "unit": "MM"/"IN" }
• The client-side (HTML/JS) parses these keys to update the UI dashboard in real-time.
======================================================================================= 
*/

// Transmits the complete system state as a JSON packet to all connected WebSocket clients.
// Called at a defined interval (or via force_extended_update) to synchronize the UI.
void sendFullStatusUpdate() {
    char jsonBuffer[400];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
             "{\"x\":%.2f,\"z\":%.2f,\"u\":%.2f,\"w\":%.2f,\"axes\":%d,\"mode\":\"%s\",\"ref_x\":\"%s\",\"ref_z\":\"%s\",\"ref_u\":\"%s\",\"ref_w\":\"%s\",\"dir_x\":%d,\"dir_z\":%d,\"dir_u\":%d,\"dir_w\":%d,\"tool\":%d,\"res_x\":%.1f,\"res_z\":%.1f,\"res_u\":%.1f,\"res_w\":%.1f,\"unit\":\"%s\"}",
             shared_x, shared_z, shared_u, shared_w, axis_mode, diameter_mode ? "D" : "R",
             is_inc_mode_x ? "INC" : "ABS", is_inc_mode_z ? "INC" : "ABS", is_inc_mode_u ? "INC" : "ABS", is_inc_mode_w ? "INC" : "ABS",
             X_DIR, Z_DIR, U_DIR, W_DIR, current_tool,
             counts_per_mm[0], counts_per_mm[2], counts_per_mm[1], counts_per_mm[3], unit_inch ? "IN" : "MM");
    ws.textAll(jsonBuffer);
}

// ===================================================================================
// ■ BLOCK: ASYNCHRONOUS WEBSOCKET USER COMMAND INTERPRETER
// ===================================================================================
/*
 * Event handler for WebSocket communication:
 * - WS_EVT_CONNECT: Triggers an immediate update for new clients, ensuring the UI 
 *   initializes instantly instead of waiting for the next scheduled broadcast interval.
 * - WS_EVT_DATA: Receives and processes commands from the web interface 
 *   (e.g., setting offsets, toggling modes) as a JSON payload.
 */
void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        force_extended_update = true;
    }
    else if (type == WS_EVT_DATA) {
        String msg;
        for (size_t i = 0; i < len; i++) msg += (char)data[i];
        // Attempt to acquire the mutex within 10ms to ensure thread-safe access to shared coordinates
        // Using xSemaphoreTake ensures that the shared variables are accessed safely by one task at a time, 
        // preventing data corruption (race conditions).
        if (xSemaphoreTake(positionMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            // HOT PATH OPTIMIZATION:
            // Frequent commands are placed at the top of the if-else chain. 
            // This reduces average execution time by minimizing unnecessary string comparisons 
            // for the most commonly received WebSocket messages.

            // --- HOT PATH --------------------------------------------------------------------------------  
            // --- ZERO COMMANDS ---> 
            if (msg == "ZERO_X") {
                // Calculate current position: (raw counts * direction) / resolution
                float p = (get_absolute_counts(PCNT_UNIT_0, global_total_x) * X_DIR) / counts_per_mm[0];
                // If in Incremental mode, only update the offset for the current tool
                if(is_inc_mode_x) offset_x_INC[current_tool] = -p;
                else {
                    // In Absolute mode, check if we are on the base tool (0)
                    if (current_tool == 0) {
                        // Calculate shift required to move tool 0 to current zero position
                        float delta = -p - offset_x_ABS[0];
                        // Apply this shift globally to all stored tool offsets to maintain consistency
                        for(int i = 0; i < 10; i++) {
                          offset_x_ABS[i] += delta;
                        }
                    } else {
                      // Otherwise, update only the specific tool's absolute offset
                      offset_x_ABS[current_tool] = -p;
                    }
                }
            }
            else if (msg == "ZERO_U") {
                float p = (get_absolute_counts(PCNT_UNIT_2, global_total_u) * U_DIR) / counts_per_mm[1];
                if(is_inc_mode_u) offset_u_INC[current_tool] = -p;
                else {
                    if (current_tool == 0) {
                        float delta = -p - offset_u_ABS[0];
                        // Apply this shift globally to all stored tool offsets to maintain consistency
                        for(int i = 0; i < 10; i++) offset_u_ABS[i] += delta;
                    } else offset_u_ABS[current_tool] = -p;
                }
            }
            else if (msg == "ZERO_Z") {
                float p = (get_absolute_counts(PCNT_UNIT_1, global_total_z) * Z_DIR) / counts_per_mm[2];
                if(is_inc_mode_z) offset_z_INC[current_tool] = -p;
                else {
                    if (current_tool == 0) {
                        float delta = -p - offset_z_ABS[0];
                        for(int i = 0; i < 10; i++) offset_z_ABS[i] += delta;
                    } else offset_z_ABS[current_tool] = -p;
                }
            }
            else if (msg == "ZERO_W") {
                float p = (get_absolute_counts(PCNT_UNIT_3, global_total_w) * W_DIR) / counts_per_mm[3];
                if(is_inc_mode_w) offset_w_INC[current_tool] = -p;
                else {
                    if (current_tool == 0) {
                        float delta = -p - offset_w_ABS[0];
                        for(int i = 0; i < 10; i++) offset_w_ABS[i] += delta;
                    } else offset_w_ABS[current_tool] = -p;
                }
            }

            // --- SET COMMANDS ---
            else if (msg.startsWith("SET_X:")) {
                // Extract target value from message (after "SET_X:")
                float v = msg.substring(6).toFloat();
                // Calculate current physical position in mm
                float p = (get_absolute_counts(PCNT_UNIT_0, global_total_x) * X_DIR) / counts_per_mm[0];
                // Apply diameter compensation if active (halve the input value)
                float target_val = (diameter_mode ? (v / 2.0f) : v);
                // If in Incremental mode, update only the current tool's incremental offset
                if(is_inc_mode_x) offset_x_INC[current_tool] = target_val - p;
                else {
                    // In Absolute mode, check if we are using the base tool
                    if (current_tool == 0) {
                        float delta = (target_val - p) - offset_x_ABS[0];
                        // Apply shift to all tool offsets to keep them relative to the new zero
                        for(int i = 0; i < 10; i++) offset_x_ABS[i] += delta;
                    } else {
                      // If not tool 0, update only this tool's specific absolute offset
                      offset_x_ABS[current_tool] = target_val - p;
                    }
                }
            }
            else if (msg.startsWith("SET_U:")) {
                float v = msg.substring(6).toFloat();
                float p = (get_absolute_counts(PCNT_UNIT_2, global_total_u) * U_DIR) / counts_per_mm[1];
                if(is_inc_mode_u) offset_u_INC[current_tool] = v - p;
                else {
                    if (current_tool == 0) {
                        float delta = (v - p) - offset_u_ABS[0];
                        for(int i = 0; i < 10; i++) offset_u_ABS[i] += delta;
                    } else offset_u_ABS[current_tool] = v - p;
                }
            }
            else if (msg.startsWith("SET_Z:")) {
                float v = msg.substring(6).toFloat();
                float p = (get_absolute_counts(PCNT_UNIT_1, global_total_z) * Z_DIR) / counts_per_mm[2];
                if(is_inc_mode_z) offset_z_INC[current_tool] = v - p;
                else {
                    if (current_tool == 0) {
                        float delta = (v - p) - offset_z_ABS[0];
                        for(int i = 0; i < 10; i++) offset_z_ABS[i] += delta;
                    } else offset_z_ABS[current_tool] = v - p;
                }
            }
            else if (msg.startsWith("SET_W:")) {
                float v = msg.substring(6).toFloat();
                float p = (get_absolute_counts(PCNT_UNIT_3, global_total_w) * W_DIR) / counts_per_mm[3];
                if(is_inc_mode_w) offset_w_INC[current_tool] = v - p;
                else {
                    if (current_tool == 0) {
                        float delta = (v - p) - offset_w_ABS[0];
                        for(int i = 0; i < 10; i++) offset_w_ABS[i] += delta;
                    } else offset_w_ABS[current_tool] = v - p;
                }
            }
            
           // --- MEDIUM PATH: UI and System Mode Toggles -------------------------------------------------
            else if (msg.startsWith("SET_TOOL:")) {
                // Parse the tool index from the command string
                int t = msg.substring(9).toInt();
                // Validate range (0-9) before updating; trigger update to refresh UI state
                if (t >= 0 && t < 10) { 
                    current_tool = t; 
                    // Force an immediate status update: This ensures that the web interface 
                    // synchronizes its tool-specific display elements (like active offsets) 
                    // instantly with the server's new state, avoiding UI latency.
                    force_extended_update = true; 
                }
            }
            // Toggle between Radius (R) and Diameter (D) measurement modes
            else if (msg == "MODE") diameter_mode = !diameter_mode;
            // Toggle between Absolute (ABS) and Incremental (INC) reference systems - per axis
            else if (msg == "TOGGLE_REF_X") is_inc_mode_x = !is_inc_mode_x;
            else if (msg == "TOGGLE_REF_Z") is_inc_mode_z = !is_inc_mode_z;
            else if (msg == "TOGGLE_REF_U") is_inc_mode_u = !is_inc_mode_u;
            else if (msg == "TOGGLE_REF_W") is_inc_mode_w = !is_inc_mode_w;
            // Switch the display unit preference (mm/inch). Calibration stays metric internally;
            // this only changes how values are rendered client-side and is persisted in NVS.
            else if (msg == "SET_UNIT_MM") { unit_inch = false; prefs.putBool("u_inch", false); force_extended_update = true; }
            else if (msg == "SET_UNIT_INCH") { unit_inch = true; prefs.putBool("u_inch", true); force_extended_update = true; }


          // --- COLD PATH -------------------------------------------------------------------------------- 
            // --- SAVE RESOLUTIONS ---
            else if (msg.startsWith("SAVE_RES:")) {
                // Extract semicolon-separated values string 
                // (e.g., "200.0;1000.0;200.0;200.0") representing counts_per_mm for X, Y, Z, A
                String values = msg.substring(9);
                // Find the positions of the semicolon delimiters within the string.
                // These indices are used to define the boundaries of each individual value.
                int idx1 = values.indexOf(';');
                int idx2 = values.indexOf(';', idx1 + 1);
                int idx3 = values.indexOf(';', idx2 + 1);
                // Ensure all three delimiters were found, which confirms the string contains all four expected values.
                if(idx1 != -1 && idx2 != -1 && idx3 != -1) {
                    counts_per_mm[0] = values.substring(0, idx1).toFloat();
                    counts_per_mm[1] = values.substring(idx1 + 1, idx2).toFloat();
                    counts_per_mm[2] = values.substring(idx2 + 1, idx3).toFloat();
                    counts_per_mm[3] = values.substring(idx3 + 1).toFloat();
                    // Persist resolutions to NVS to survive device reboots
                    prefs.putFloat("r_x", counts_per_mm[0]);
                    prefs.putFloat("r_u", counts_per_mm[1]);
                    prefs.putFloat("r_z", counts_per_mm[2]);
                    prefs.putFloat("r_w", counts_per_mm[3]);
                    // Force a full UI update because resolution changes affect all displayed positions,
                    // ensuring the browser immediately reflects the new scaling factors.
                    force_extended_update = true;
                }
            }

            // --- SAVE PREFS (GLOBAL TOOLS/POSITIONS) ---
            // Persists current coordinate offsets, encoder counts, and UI states to NVS.
            else if (msg == "SAVE_PREFS") {
                // Buffer for generating unique NVS key names (max 16 chars required).
                // We iterate through 10 tool slots (0-9), each having 8 parameters.
                // Example keys generated for tool index 5:
                // 1. "x_abs_5"  2. "u_abs_5"  3. "z_abs_5"  4. "w_abs_5"
                // 5. "x_inc_5"  6. "u_inc_5"  7. "z_inc_5"  8. "w_inc_5"
                // This loop generates 80 unique keys (10 tools * 8 parameters) in total.
                char k[16]; 
                // Iterate through all 10 possible tool slots to save their individual coordinate offsets
                for(int i = 0; i < 10; i++) {
                    sprintf(k, "x_abs_%d", i); prefs.putFloat(k, offset_x_ABS[i]);
                    sprintf(k, "u_abs_%d", i); prefs.putFloat(k, offset_u_ABS[i]);
                    sprintf(k, "z_abs_%d", i); prefs.putFloat(k, offset_z_ABS[i]);
                    sprintf(k, "w_abs_%d", i); prefs.putFloat(k, offset_w_ABS[i]);
                    sprintf(k, "x_inc_%d", i); prefs.putFloat(k, offset_x_INC[i]);
                    sprintf(k, "u_inc_%d", i); prefs.putFloat(k, offset_u_INC[i]);
                    sprintf(k, "z_inc_%d", i); prefs.putFloat(k, offset_z_INC[i]); 
                    sprintf(k, "w_inc_%d", i); prefs.putFloat(k, offset_w_INC[i]);
                }
                // Save hardware encoder base values to maintain position across power cycles
                prefs.putInt("raw_cnt_x", get_absolute_counts(PCNT_UNIT_0, global_total_x));
                prefs.putInt("raw_cnt_u", get_absolute_counts(PCNT_UNIT_2, global_total_u));
                prefs.putInt("raw_cnt_z", get_absolute_counts(PCNT_UNIT_1, global_total_z));
                prefs.putInt("raw_cnt_w", get_absolute_counts(PCNT_UNIT_3, global_total_w));
                // Save current UI state to restore user settings after reboot
                prefs.putInt("last_tool", current_tool);
                prefs.putBool("inc_x", is_inc_mode_x);
                prefs.putBool("inc_z", is_inc_mode_z);
                prefs.putBool("inc_u", is_inc_mode_u);
                prefs.putBool("inc_w", is_inc_mode_w);
            }

            // Update and persist the direction multipliers for each axis.
            // These values (typically 1 or -1) define whether a counter increments 
            // or decrements when an axis moves in a specific direction.
            else if (msg.startsWith("DIR_X:")) { X_DIR = msg.substring(6).toInt(); prefs.putInt("dx", X_DIR); }
            else if (msg.startsWith("DIR_U:")) { U_DIR = msg.substring(6).toInt(); prefs.putInt("du", U_DIR); }
            else if (msg.startsWith("DIR_Z:")) { Z_DIR = msg.substring(6).toInt(); prefs.putInt("dz", Z_DIR); }
            else if (msg.startsWith("DIR_W:")) { W_DIR = msg.substring(6).toInt(); prefs.putInt("dw", W_DIR); }

            // Configures the number of active axes to be displayed on the UI (e.g., 2, 3, or 4 axes).
            // This allows the interface to dynamically adjust its layout and filter data 
            // based on the specific machine configuration.
            // The selection is saved to NVS and an update is forced to refresh the frontend immediately. 
            else if (msg.startsWith("SET_AXIS_MODE:")) {
                axis_mode = msg.substring(14).toInt();
                prefs.putInt("ax_mode", axis_mode);
                force_extended_update = true; 
            }
            xSemaphoreGive(positionMutex);  // Always release the mutex after the critical section
        }
        sendFullStatusUpdate();
    }
}

// ===================================================================================
// ■ BLOCK: TELEMETRY BROADCASTER COMPONENT (PINNED TO CORE 0)
// ===================================================================================
// PURPOSE: This task runs on Core 0 to offload network operations from the main logic.
// It acts as the bridge between the high-frequency measurement system (Core 1) and
// the web-based user interface.
//
// DATA FLOW: 
// 1. Snapshot: Pulls current axis positions from shared variables (Thread-safe via Mutex).
// 2. Comparison: Compares current state against cached previous state (Delta-Check).
// 3. Transmission: Formats data into a JSON packet and broadcasts it via WebSocket 
//    to all connected web clients only when changes occur or updates are forced.
// ===================================================================================
void NetworkTaskCode(void * pvParameters) {
    WiFi.softAP(ssid, password);
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.begin();
    // Cache variables to track previous state; initialized to -9999 to ensure the first update is always sent
    float lx = -9999, lu = -9999, lz = -9999, lw = -9999;
    char jsonBuffer[400];
    // Heartbeat: forces a broadcast at least once per second even without axis movement, so the
    // client can tell "machine idle" apart from "connection lost" (see connection watchdog in JS).
    unsigned long lastHeartbeat = 0;
    const unsigned long HEARTBEAT_INTERVAL_MS = 1000;
    while (1) {
        ws.cleanupClients();  // Removes stale WebSocket connections
        // Only broadcast if at least one client is connected to save resources
        if (ws.count() > 0) { 
            float local_x, local_u, local_z, local_w;

            // Thread-safe data transfer: 
            // The position variables (shared_x, shared_u, etc.) are updated by a high-frequency 
            // interrupt/task. To prevent a "Data Race" (where the telemetry task reads a variable 
            // while it is being partially overwritten by the update task), we use a Mutex. 
            // xSemaphoreTake() locks access, ensuring we get a consistent, atomic snapshot 
            // of all axis positions before releasing the lock with xSemaphoreGive().
            if (xSemaphoreTake(positionMutex, portMAX_DELAY) == pdTRUE) {
                local_x = shared_x; local_u = shared_u; local_z = shared_z; local_w = shared_w;
                xSemaphoreGive(positionMutex);
            }

            // Delta-Check: Only send data if positions have changed significantly (> 0.005mm)
            // This bandwidth-optimization ensures we don't spam the network with tiny sensor jitter
            bool changed = (abs(local_x - lx) > 0.005 || abs(local_z - lz) > 0.005);
            if (axis_mode >= 3) changed |= (abs(local_u - lu) > 0.005);
            if (axis_mode == 4) changed |= (abs(local_w - lw) > 0.005);
            // Force refresh: If requested (e.g., after settings change), reset cache to trigger immediate broadcast
            if (force_extended_update) { lx = -9999; lu = -9999; lz = -9999; lw = -9999; changed = true; }

            // Heartbeat: even with zero movement, send at least one update per interval so the
            // client's connection watchdog sees continuous traffic while the machine is simply idle.
            bool heartbeatDue = (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS);

            if (changed || force_extended_update || heartbeatDue) {
                force_extended_update = false;
                lastHeartbeat = millis();

                // Build JSON telemetry packet dynamically
                // Compile current machine state into a JSON object to update the web interface
                snprintf(jsonBuffer, sizeof(jsonBuffer),
                         "{\"x\":%.2f,\"z\":%.2f,\"u\":%.2f,\"w\":%.2f,\"axes\":%d,\"mode\":\"%s\",\"ref_x\":\"%s\",\"ref_z\":\"%s\",\"ref_u\":\"%s\",\"ref_w\":\"%s\",\"dir_x\":%d,\"dir_z\":%d,\"dir_u\":%d,\"dir_w\":%d,\"tool\":%d,\"res_x\":%.1f,\"res_z\":%.1f,\"res_u\":%.1f,\"res_w\":%.1f,\"unit\":\"%s\"}",
                         local_x, local_z, local_u, local_w, axis_mode, diameter_mode ? "D" : "R",
                         is_inc_mode_x ? "INC" : "ABS", is_inc_mode_z ? "INC" : "ABS", is_inc_mode_u ? "INC" : "ABS", is_inc_mode_w ? "INC" : "ABS",
                         X_DIR, Z_DIR, U_DIR, W_DIR, current_tool,
                         counts_per_mm[0], counts_per_mm[2], counts_per_mm[1], counts_per_mm[3], unit_inch ? "IN" : "MM");
                
                // Broadcast telemetry to all connected WebSocket clients
                ws.textAll(jsonBuffer);
                // Update cache with new values for next delta-comparison
                lx = local_x; lu = local_u; lz = local_z; lw = local_w;
            }
        }
        // Task delay (50ms = 20Hz update rate) to balance responsiveness and CPU load
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ===================================================================================
// ■ BLOCK: INITIALIZATION
// ===================================================================================
void setup() {
    // Create mutex to synchronize access to position variables between hardware-tasks and network-task
    positionMutex = xSemaphoreCreateMutex();

/*
    =======================================================================================
     NVS/PREFERENCES INITIALIZATION
    --------------------------------
    • Retrieves machine calibration and user state from Non-Volatile Storage (NVS).
    • Defaults (e.g., 200.0f) ensure the machine remains functional even after 
      a factory reset or the first time the firmware is flashed.
    =======================================================================================
    */

    prefs.begin("dro", false);  // Open namespace "dro" in read/write mode

    // Load axis direction configurations (1: normal, -1: inverted)
    X_DIR = prefs.getInt("dx", 1);
    U_DIR = prefs.getInt("du", 1);
    Z_DIR = prefs.getInt("dz", 1);
    W_DIR = prefs.getInt("dw", 1);

    // Load UI and operational states
    axis_mode = prefs.getInt("ax_mode", 3);
    current_tool = prefs.getInt("last_tool", 0);
    diameter_mode = true; 
    is_inc_mode_x = prefs.getBool("inc_x", false);
    is_inc_mode_z = prefs.getBool("inc_z", false);
    is_inc_mode_u = prefs.getBool("inc_u", false);
    is_inc_mode_w = prefs.getBool("inc_w", false);
    unit_inch = prefs.getBool("u_inch", false); // Display unit preference (mm=false / inch=true). Calibration itself always stays metric.

    // Load physical calibration factors (counts per millimeter).
    // The second parameter (e.g., 200.0f) is the fallback default used ONLY if 
    // no value is currently stored in NVS; otherwise, the stored value is retrieved.
    counts_per_mm[0] = prefs.getFloat("r_x", 200.0f);
    counts_per_mm[1] = prefs.getFloat("r_u", 200.0f);
    counts_per_mm[2] = prefs.getFloat("r_z", 200.0f);
    counts_per_mm[3] = prefs.getFloat("r_w", 200.0f);
    
    // Restore raw hardware encoder counts to resume position tracking seamlessly.
    // The second parameter (0) is the fallback default, used only if no previous 
    // count is found in NVS (e.g., on first boot). If a value is stored, it 
    // will be retrieved and used to restore the machine's absolute position.
    global_total_x = prefs.getInt("raw_cnt_x", 0);
    global_total_u = prefs.getInt("raw_cnt_u", 0);
    global_total_z = prefs.getInt("raw_cnt_z", 0);
    global_total_w = prefs.getInt("raw_cnt_w", 0);

    // Iterate through all 10 tool slots to restore coordinate offsets from NVS.
    // Each tool has 8 associated parameters (4 ABS, 4 INC).
    // The loop uses sprintf to generate keys dynamically (e.g., "x_abs_0" to "w_inc_9").
    for(int i = 0; i < 10; i++) {
        char k[16];

        // Retrieve and validate: 
        // 1. prefs.getFloat(k, 0.0f): Tries to load the value; uses 0.0f as a fallback if the key is missing.
        // 2. isnan(...): Checks if the retrieved value is "Not A Number". If the NVS data is corrupted, 
        //    we force a reset to 0.0f to ensure the machine doesn't behave erratically.
        sprintf(k, "x_abs_%d", i); offset_x_ABS[i] = prefs.getFloat(k, 0.0f); if (isnan(offset_x_ABS[i])) offset_x_ABS[i] = 0.0f;
        sprintf(k, "u_abs_%d", i); offset_u_ABS[i] = prefs.getFloat(k, 0.0f); if (isnan(offset_u_ABS[i])) offset_u_ABS[i] = 0.0f;
        sprintf(k, "z_abs_%d", i); offset_z_ABS[i] = prefs.getFloat(k, 0.0f); if (isnan(offset_z_ABS[i])) offset_z_ABS[i] = 0.0f;
        sprintf(k, "w_abs_%d", i); offset_w_ABS[i] = prefs.getFloat(k, 0.0f); if (isnan(offset_w_ABS[i])) offset_w_ABS[i] = 0.0f;
        sprintf(k, "x_inc_%d", i); offset_x_INC[i] = prefs.getFloat(k, 0.0f); if (isnan(offset_x_INC[i])) offset_x_INC[i] = 0.0f;
        sprintf(k, "u_inc_%d", i); offset_u_INC[i] = prefs.getFloat(k, 0.0f); if (isnan(offset_u_INC[i])) offset_u_INC[i] = 0.0f;
        sprintf(k, "z_inc_%d", i); offset_z_INC[i] = prefs.getFloat(k, 0.0f); if (isnan(offset_z_INC[i])) offset_z_INC[i] = 0.0f;
        sprintf(k, "w_inc_%d", i); offset_w_INC[i] = prefs.getFloat(k, 0.0f); if (isnan(offset_w_INC[i])) offset_w_INC[i] = 0.0f;
    }

    /*
    =======================================================================================
    HARDWARE MAPPING & SIGNAL INTEGRITY
    ----------------------------------------
    • PCNT Unit 0: X-Axis | Unit 1: Z-Axis | Unit 2: Y-Axis | Unit 3: A-Axis
    • SIGNAL CONDITIONING: 
      All quadrature inputs (Phase A/B) are processed via onboard Schmitt-Trigger logic.
      This provides essential hysteresis to filter out EMI/noise from the machine environment 
      and ensures clean, reliable transitions for the PCNT hardware units.
    PURPOSE: 
     Initializes the ESP32's internal Pulse Counter (PCNT) hardware units to track 
     axis movement. Unlike software polling, this offloads the signal processing to 
     dedicated hardware, ensuring no pulses are missed regardless of CPU load.

     SIGNAL INTEGRITY & RELIABILITY:
     1. Hardware Mapping: Each axis is bound to a specific PCNT unit and physical pins.
     2. Signal Conditioning: Inputs are assumed to be buffered (Schmitt-Trigger) to 
        prevent noise-induced jitter, which is critical for accurate positioning.
     3. Interrupt Architecture: 
        - Installs a high-speed Interrupt Service Routine (ISR) in IRAM for sub-microsecond 
          response times.
        - The ISR handles counter overflows to support virtually unlimited travel 
          distance without resetting the count.
        - Enables real-time interrupts for all axes to ensure instantaneous 
          tracking of every encoder transition (Phase A/B quadrature decoding).
    =======================================================================================
    */

    setupPCNT(PCNT_UNIT_0, X_PIN_A, X_PIN_B, global_total_x);
    setupPCNT(PCNT_UNIT_1, Z_PIN_A, Z_PIN_B, global_total_z);
    setupPCNT(PCNT_UNIT_2, U_PIN_A, U_PIN_B, global_total_u);
    setupPCNT(PCNT_UNIT_3, W_PIN_A, W_PIN_B, global_total_w);
    
    // Register the Interrupt Service Routine (ISR) service to handle counter overflows
    // and threshold events with minimal latency (IRAM placement for speed).
    pcnt_isr_service_install(ESP_INTR_FLAG_IRAM);

    // Assign specific handler functions to each PCNT unit
    pcnt_isr_handler_add(PCNT_UNIT_0, pcnt_intr_handler, (void*) PCNT_UNIT_0);
    pcnt_isr_handler_add(PCNT_UNIT_1, pcnt_intr_handler, (void*) PCNT_UNIT_1);
    pcnt_isr_handler_add(PCNT_UNIT_2, pcnt_intr_handler, (void*) PCNT_UNIT_2);
    pcnt_isr_handler_add(PCNT_UNIT_3, pcnt_intr_handler, (void*) PCNT_UNIT_3);
    
    // Enable interrupts to allow the system to respond instantly to axis movement
    pcnt_intr_enable(PCNT_UNIT_0); 
    pcnt_intr_enable(PCNT_UNIT_1); 
    pcnt_intr_enable(PCNT_UNIT_2); 
    pcnt_intr_enable(PCNT_UNIT_3);


    /*
    =======================================================================================
     WEB SERVER INITIALIZATION & UI ARCHITECTURE
    ---------------------------------------------------------------------------------------
     - The root handler "/" serves the static HTML/CSS/JS frontend once upon connection.
     
     - DYNAMIC UPDATES:
       1. Data Feed (Downstream): Uses WebSockets to stream lightweight JSON telemetry 
          from the ESP32 to the browser, updating the UI without page reloads.
       2. User Interaction (Upstream): When a user clicks a button (e.g., "Zero Axis", 
          "Change Tool"), the JS frontend sends a specific command string back to 
          the ESP32 via the same WebSocket connection.
       3. Command Execution: The `onWsEvent` handler processes these commands, 
          updates global variables (or triggers NVS saves), and triggers an immediate 
          UI refresh (force_extended_update).
    =======================================================================================
    */
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send_P(200, "text/html", R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#111111">
<meta name="format-detection" content="telephone=no">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover">
<style>
/* ===================================================================================
   RESPONSIVE LAYOUT FOR SMARTPHONES (optimiert für Pixel 9, ~412x915 CSS-Px Portrait).
   Statt fixer Pixelbreiten (iPad) werden clamp()/vw-basierte, flüssige Größen genutzt,
   damit die UI auch bei kleineren/größeren Android-Displays und in Landscape passt.
   =================================================================================== */
:root {
  --tap: 60px; /* Mindest-Touch-Ziel, in Portrait großzügiger bemessen */
}
* { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
html, body { overscroll-behavior: none; }
body { background:#111; color:#0f0; font-family:monospace; -webkit-touch-callout:none; -webkit-user-select:none; user-select:none; touch-action:none; overflow:hidden; position:fixed; width:100%; height:100%; margin:0; }
/* Portrait-Basislayout: Titel in eigener Zeile, Buttons darunter */
header { display: flex; flex-direction: row; align-items: center; justify-content: space-between; gap: 10px; padding: 10px 14px; background: #1a1a1a; border-bottom: 1px solid #333; }
.cfg-btn { font-size: clamp(0.95em, 3.4vw, 1.3em); padding: 8px 12px; margin-left: 0; background: #333; color: #888; border: 1px solid #444; border-radius: 4px; min-height: var(--tap); }
.cfg-btn.active { background: #ff9900; color: #000; font-weight: bold; border-color: #ff9900; }
.gear-icon { display: inline-flex; align-items: center; justify-content: center; cursor: pointer; color: #ff9900; font-size: clamp(1.6em, 6vw, 2.4em); width: var(--tap); height: var(--tap); flex-shrink: 0; transition: transform 0.2s, color 0.2s; }
.gear-icon.active { color: #fff; transform: rotate(45deg); }
/* Per-axis ABS/INC badge, sized/positioned like the direction circle right next to it */
/* Axis letter itself is now the ABS/INC switch: green = ABS, yellow = INC */
.axisletter { cursor: pointer; color: #00cc00; }
.axisletter.inc { color: #ffcc00; }

.container { display: grid; grid-template-columns: minmax(34px, 11vw) 1fr auto; padding: 10px 8px; padding-bottom: 215px; row-gap: 14px; column-gap: 6px; align-items: center; }
//.axislabel { font-size: clamp(2.2em, 13vw, 3.8em); font-weight: bold;}

.axislabel {
    display: flex;
    flex-direction: column;
    align-items: center;
    font-size: clamp(2.2em, 13vw, 3.8em);
    font-weight: bold;
    line-height: 1;
}

.value { font-size: clamp(1.4em, 12vw, 3.3em); text-align: right; font-weight: bold; padding-right: 6px; display: block; letter-spacing: -1px; overflow: hidden; }
.value.inc { color: #ffcc00; }

/* CONNECTION STATUS: small dot next to the title, full banner, and dimming of the live values */
#conn-dot { display: inline-block; width: 0.45em; height: 0.45em; border-radius: 50%; margin-left: 10px; vertical-align: middle; background: #0f0; box-shadow: 0 0 6px #0f0; }
body.conn-lost #conn-dot { background: #f00; box-shadow: 0 0 8px #f00; }
#conn-banner { display: none; position: fixed; top: 0; left: 0; right: 0; z-index: 10002; background: #a30000; color: #fff; text-align: center; font-weight: bold; font-size: clamp(0.85em, 3.4vw, 1.1em); padding: 8px 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.5); cursor: pointer; }
body.conn-lost #conn-banner { display: block; }
body.conn-lost .value, body.conn-lost .compound-value-readonly { color: #666 !important; opacity: 0.55; transition: opacity 0.3s, color 0.3s; }
body.conn-lost .container { outline: 2px solid #a30000; outline-offset: -2px; }

/* DISPLAY UNIT TOGGLE (Settings panel) */
.unit-toggle-btn { font-size: 1.0em; padding: 8px 18px; background: #222; color: #888; border: 1px solid #555; border-radius: 4px; font-family: monospace; font-weight: bold; min-height: var(--tap); }
.unit-toggle-btn.active { background: #ff9900; color: #000; border-color: #ff9900; }

/* INCH DISPLAY MODE: values gain 2 extra decimal digits (0.0000") - trim size slightly so they
   keep fitting the same layout that was tuned for 2-decimal mm values. */
body.unit-inch .value { font-size: clamp(1.2em, 10vw, 2.7em); letter-spacing: -1.5px; }
body.unit-inch .compound-row, body.unit-inch .compound-result-row { grid-template-columns: 1fr 128px var(--tap); }
body.unit-inch .compound-value, body.unit-inch .compound-value-readonly { font-size: clamp(1.05em, 4.6vw, 1.5em); }
body.unit-inch .compound-result-value { font-size: clamp(1.2em, 5vw, 1.7em); }
.controls { display: flex; gap: 6px; align-items: center; justify-content: flex-end; width: auto; position: relative; flex-shrink: 0; }
.ctrl-spacer { width: var(--tap); flex-shrink: 0; }
button.input-trigger-btn { font-size: clamp(1.2em, 4.5vw, 1.8em); width: var(--tap); height: var(--tap); background: #222; color: #ff9900; border: 1px solid #444; border-radius: 6px; font-weight: bold; }
button.zero-btn { font-size: clamp(1.2em, 4.5vw, 1.8em); width: var(--tap); height: var(--tap); background: #c00; color: #fff; border: none; border-radius: 6px; font-weight: bold; }
.confirm-overlay { display: none; position: absolute; top: 0; left: 0; right: 0; bottom: 0; background: rgba(15,15,15,0.97); border: 2px solid #ff3333; border-radius: 6px; z-index: 10000; align-items: center; justify-content: space-between; padding: 0 6px; gap: 4px; }
.confirm-overlay span { color: #fff; font-size: clamp(0.75em, 2.6vw, 1em); font-weight: bold; }
.confirm-overlay button { background: #c00; color: white; border: none; font-size: clamp(0.85em, 3vw, 1.1em); font-weight: bold; height: 38px; width: 60px; border-radius: 4px; }
#mode { font-size: clamp(1.5em, 5.5vw, 2.2em); font-weight: bold; text-align: center; background: #222; border-radius: 8px; width: var(--tap); height: var(--tap); display: flex; align-items: center; justify-content: center; border: 1px solid #444; }
#compound-toggle { font-size: clamp(1.5em, 5.5vw, 2.2em); font-weight: bold; text-align: center; background: #222; color: #00cfff; border-radius: 8px; width: var(--tap); height: var(--tap); display: flex; align-items: center; justify-content: center; border: 1px solid #444; }
#compound-toggle.active { background: #006c85; color: #fff; border-color: #00cfff; }
/* Richtungsumkehr-Symbol: eigener runder Touch-Bereich statt winzigem Text */
.dir { display: inline-flex; align-items: center; justify-content: center; color: #444; cursor: pointer; font-size: 1.4rem; width: 48px; height: 48px; border-radius: 50%; background: rgba(255,255,255,0.06); vertical-align: middle; }
.dir.inv { color: #0f0; background: rgba(0,255,0,0.12); }
.row-wrapper { display: contents; }
body.mode-2 .container { row-gap: clamp(24px, 12vh, 60px); padding-top: clamp(16px, 6vh, 40px); }
body.mode-3 .container { row-gap: clamp(14px, 6vh, 32px); padding-top: 10px; }
body.mode-4 .container { row-gap: clamp(8px, 3vh, 18px); padding-top: 6px; }



#numpad { display: none; position: fixed; top: 50%; left: 50%; transform: translate(-50%, -50%); background: rgba(20,20,20,0.98); padding: 16px; border: 3px solid #0f0; border-radius: 16px; box-shadow: 0 0 35px rgba(0,255,0,0.4); z-index: 10001; width: min(92vw, 360px); max-height: 90vh; overflow-y: auto; }
#numpad-title { color: #ff9900; font-size: clamp(1.1em, 4.5vw, 1.6em); font-weight: bold; text-align: center; margin-bottom: 10px; }
#numpad-display { background: #000; color: #fff; font-size: clamp(1.8em, 8vw, 2.6em); text-align: right; padding: 8px 12px; margin-bottom: 10px; border: 1px solid #333; border-radius: 6px; min-height: 1.2em; }
.np-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
#numpad button { width: 100%; aspect-ratio: 1.4 / 1; min-height: 56px; font-size: clamp(1.4em, 6vw, 2em); background:#252525; color: #fff; border: 1px solid #444; border-radius: 8px; }
#numpad .np-exit { background: #800c0c; }
#numpad .np-clear { background: #444; }
#numpad .np-set { background: #048004; font-size: clamp(1.1em, 4.5vw, 1.6em); }

/* COMPOUND SLIDE OVERLAY (angle-based Delta X / Delta Z helper for taper & thread turning) */
#compound-overlay { display: none; position: fixed; top: 50%; left: 50%; transform: translate(-50%, -50%); background: rgba(15,15,20,0.98); border: 3px solid #00cfff; border-radius: 16px; box-shadow: 0 0 30px rgba(0,207,255,0.35); z-index: 10000; padding: 18px 20px; width: min(90vw, 420px); max-height: 90vh; overflow-y: auto; color: #fff; font-family: monospace; }
#compound-title { color: #00cfff; font-size: clamp(1.1em, 4.5vw, 1.5em); font-weight: bold; text-align: center; margin-bottom: 14px; letter-spacing: 1px; }
.compound-row { display: grid; grid-template-columns: 1fr 105px var(--tap); align-items: center; gap: 10px; margin-bottom: 12px; }
.compound-label { font-size: clamp(1.05em, 3.8vw, 1.25em); color: #aaa; }
.compound-value { font-size: clamp(1.3em, 5.5vw, 1.8em); color: #0f0; font-weight: bold; text-align: right; cursor: pointer; }
.compound-value-readonly { font-size: clamp(1.3em, 5.5vw, 1.8em); color: #0f0; font-weight: bold; text-align: right; }
.compound-spacer { width: var(--tap); height: var(--tap); }
#compound-sign-btn { width: var(--tap); height: var(--tap); font-size: clamp(1.3em, 4.5vw, 1.7em); background: #222; color: #ff9900; border: 1px solid #444; border-radius: 6px; font-weight: bold; }
.compound-zero-btn { width: var(--tap); height: var(--tap); font-size: clamp(1.1em, 4vw, 1.5em); background: #c00; color: #fff; border: none; border-radius: 6px; font-weight: bold; }
.compound-result-row { display: grid; grid-template-columns: 1fr 105px var(--tap); align-items: center; gap: 10px; margin-bottom: 10px; padding: 8px 10px; background: rgba(0,207,255,0.08); border: 1px solid rgba(0,207,255,0.35); border-radius: 8px; }
.compound-result-value { font-size: clamp(1.4em, 6vw, 2em); color: #00cfff; font-weight: bold; text-align: right; grid-column: 2 / 4; }
#compound-close-btn { width: 100%; height: var(--tap); margin-top: 8px; font-size: clamp(1.05em, 4vw, 1.3em); background: #333; color: #fff; border: 1px solid #555; border-radius: 8px; font-weight: bold; }

#toolbar { display: grid; grid-template-columns: repeat(5, 1fr); gap: 8px; }
.tool-fixed { width: 100%; font-size: clamp(1em, 4.2vw, 1.4em); font-weight: bold; height: 62px; background: #222; color: #888; border: 1px solid #444; border-radius: 8px; cursor: pointer; }
.tool-fixed.active { background: #2ecc71; color: #000; font-weight: bold; border-color: #2ecc71; box-shadow: 0 0 10px rgba(46, 204, 113, 0.5); }
.save-wide { grid-column: 1 / -1; width: 100%; height: 62px; font-size: clamp(1.1em, 4.6vw, 1.5em); background: #2980b9; color: #fff; border: 1px solid #3498db; border-radius: 8px; cursor: pointer; font-weight: bold; }
.save-wide:active { background: #ff9900; border-color: #ff9900; color: #000; }

/* SETTINGS OVERLAY MENU WITH SMART DROPDOWN LISTS */
#settings-panel { display: none; position: fixed; top: 76px; bottom: 222px; left: 6px; right: 6px; background: rgba(20,20,20,0.98); border: 2px solid #ff9900; border-radius: 8px; z-index: 99; padding: 14px; box-shadow: 0 0 25px rgba(255,153,0,0.3); color: #fff; overflow-y: auto;}
.settings-row { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center; gap: 8px; margin-bottom: 16px; font-size: clamp(0.95em, 3.6vw, 1.3em); }
.settings-control-group { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
.settings-row select { font-size: 1.0em; padding: 6px; background: #222; color: #fff; border: 1px solid #555; border-radius: 4px; font-family: monospace; min-height: var(--tap); }
.settings-row input { font-size: 1.0em; width: 100px; background: #000; color: #ff9900; border: 1px solid #444; border-radius: 4px; text-align: right; padding: 5px; font-family: monospace; min-height: var(--tap); }
.settings-action-container { display: flex; gap: 12px; justify-content: center; margin-top: 24px; flex-wrap: wrap; }
.btn-settings-save { font-size: clamp(0.95em, 3.6vw, 1.2em); font-weight: bold; padding: 12px 20px; background: #27ae60; color: #fff; border: 1px solid #2ecc71; border-radius: 6px; cursor: pointer; min-height: var(--tap); }
.btn-settings-cancel { font-size: clamp(0.95em, 3.6vw, 1.2em); font-weight: bold; padding: 12px 20px; background: #c0392b; color: #fff; border: 1px solid #e74c3c; border-radius: 6px; cursor: pointer; min-height: var(--tap); }
.settings-divider { border: none; border-top: 2px solid #ff9900; margin: 24px 0 16px 0; }

/* LANDSCAPE-FALLBACK: falls das Pixel 9 quer gehalten wird, kompakter darstellen */
@media (orientation: landscape) and (max-height: 480px) {
  header { padding: 4px 8px; }
  .cfg-btn { padding: 4px 10px; min-height: 38px; font-size: clamp(0.85em, 2.6vw, 1.1em); }

  .container { row-gap: 6px; padding: 4px 8px; padding-bottom: 66px; }
  .value { font-size: clamp(1.6em, 8vw, 2.6em); }
  .axislabel { font-size: clamp(1.1em, 4.5vw, 1.6em); flex-direction: row; align-items: center;}
  .ctrl-spacer { width: 68px; }

  /* I, Z und der D/R-Button (#mode) ca. 70% breiter als im Basis-Portrait-Maß */
  button.input-trigger-btn, button.zero-btn, #mode, #compound-toggle { width: 68px; height: 46px; font-size: clamp(1.1em, 3.6vw, 1.4em); }

  /* Toolbar zurück auf eine horizontal scrollbare Zeile, Buttons ca. 70% breiter */
  #toolbar { display: flex; flex-wrap: nowrap; overflow-x: auto; -webkit-overflow-scrolling: touch; scrollbar-width: none; bottom: 4px !important; gap: 6px; }
  #toolbar::-webkit-scrollbar { display: none; }
  .tool-fixed { flex: 0 0 auto; width: 72px; height: 46px; font-size: clamp(0.85em, 2.4vw, 1.05em); }
  .save-wide { grid-column: auto; flex: 0 0 auto; width: auto; min-width: 130px; height: 46px; font-size: clamp(0.9em, 2.6vw, 1.15em); }

  #settings-panel { top: 46px; bottom: 58px; }

  /* Numpad-Overlay im Landscape ca. halbiert - vorher zu groß für die 412px Bildhöhe */
  #numpad { width: min(60vw, 230px); padding: 8px; }
  #numpad-title { font-size: clamp(0.8em, 2.6vw, 1em); margin-bottom: 4px; }
  #numpad-display { font-size: clamp(1.1em, 4.5vw, 1.4em); padding: 5px 7px; margin-bottom: 5px; }
  .np-grid { gap: 4px; }
  #numpad button { min-height: 28px; font-size: clamp(0.75em, 2.8vw, 0.95em); }
  #numpad .np-set { font-size: clamp(0.65em, 2.4vw, 0.85em); }

  body.unit-inch .value { font-size: clamp(1.35em, 6.8vw, 2.2em); letter-spacing: -1.5px; }
}

/* ===================================================================================
   TABLET-FALLBACK (iPad mini bis iPad Pro 12.9", Portrait & Landscape, ~700-1400 CSS-Px).
   Auf einem Tablet ist deutlich mehr Fläche vorhanden als auf dem Pixel 9 - Header und
   Toolbar passen bequem in je eine Zeile, Schrift/Touch-Ziele dürfen großzügiger sein.
   =================================================================================== */
@media (min-width: 700px) and (max-width: 1400px) and (min-height: 500px) {
  :root { --tap: 74px; }

  header { padding: 10px 20px; }
  .cfg-btn { font-size: clamp(1.1em, 2vw, 1.5em); padding: 10px 18px; }

  .container { padding: 20px 30px; padding-bottom: 260px; row-gap: clamp(20px, 4vh, 45px); column-gap: 16px; }
  body.mode-2 .container { row-gap: clamp(40px, 10vh, 90px); padding-top: clamp(20px, 6vh, 50px); }
  body.mode-3 .container { row-gap: clamp(24px, 6vh, 55px); padding-top: 16px; }
  body.mode-4 .container { row-gap: clamp(14px, 3vh, 30px); padding-top: 10px; }

  .axislabel { font-size: clamp(3em, 9vw, 7em); flex-direction: row; align-items: center; }
  .value { font-size: clamp(3em, 9vw, 7em); padding-right: 15px; }
  .ctrl-spacer { width: var(--tap); }
  button.input-trigger-btn, button.zero-btn, #mode { font-size: clamp(1.6em, 3vw, 2.2em); width: var(--tap); height: var(--tap); }
  .confirm-overlay span { font-size: clamp(1em, 1.8vw, 1.3em); }
  .confirm-overlay button { font-size: clamp(1.1em, 2vw, 1.4em); height: 50px; width: 90px; }

  #numpad { width: min(52vw, 420px); padding: 18px; }
  #numpad-title { font-size: clamp(1.2em, 2vw, 1.5em); }
  #numpad-display { font-size: clamp(1.8em, 3.4vw, 2.2em); padding: 10px 14px; }
  #numpad button { min-height: 56px; font-size: clamp(1.3em, 2.2vw, 1.6em); }
  #numpad .np-set { font-size: clamp(1.1em, 1.8vw, 1.4em); }

  /* Toolbar passt bequem in eine Zeile - kein Umbruch/Scroll nötig */
  #toolbar { display: flex; flex-wrap: nowrap; gap: 10px; }
  .tool-fixed { flex: 1 1 0; width: auto; height: 78px; font-size: clamp(1.2em, 1.8vw, 1.6em); }
  .save-wide { grid-column: auto; flex: 1 1 0; width: auto; height: 78px; font-size: clamp(1.3em, 2vw, 1.8em); }

  #settings-panel { top: 110px; bottom: 130px; padding: 24px; }
  .settings-row { font-size: clamp(1.2em, 2vw, 1.6em); }
  .btn-settings-save, .btn-settings-cancel { font-size: clamp(1.2em, 2vw, 1.5em); padding: 16px 28px; }

  /* Compound-Slide-Overlay: deutlich größer, darf auf dem iPad ruhig die Toolbar überlagern */
  #compound-overlay { width: min(78vw, 640px); padding: 30px 34px; }
  #compound-title { font-size: clamp(1.5em, 2.6vw, 2em); margin-bottom: 22px; cursor: grab; }
  #compound-title:active { cursor: grabbing; }
  .compound-row, .compound-result-row { grid-template-columns: 1fr 200px var(--tap); margin-bottom: 20px; gap: 16px; }
  .compound-label { font-size: clamp(1.3em, 2.2vw, 1.7em); }
  .compound-value, .compound-value-readonly { font-size: clamp(1.9em, 3.4vw, 2.5em); }
  .compound-result-value { font-size: clamp(2.2em, 4vw, 3em); }
  #compound-sign-btn, .compound-zero-btn, .compound-spacer { width: var(--tap); height: var(--tap); font-size: clamp(1.6em, 2.8vw, 2em); }
  #compound-close-btn { height: var(--tap); font-size: clamp(1.3em, 2.4vw, 1.7em); }

  /* iPad + Inch: braucht mehr Platz als das Handy, aber weniger als die reine mm-iPad-Spalte */
  body.unit-inch .value { font-size: clamp(2.2em, 7.5vw, 5.5em); }
  body.unit-inch .compound-row, body.unit-inch .compound-result-row { grid-template-columns: 1fr 165px var(--tap); }
  body.unit-inch .compound-value, body.unit-inch .compound-value-readonly { font-size: clamp(1.5em, 2.8vw, 2em); }
  body.unit-inch .compound-result-value { font-size: clamp(1.7em, 3.2vw, 2.3em); }
}
</style>
</head>
<body>
<header>
  <span style="font-size:clamp(1.4em,5.4vw,2.0em); color:#777; font-weight:bold; white-space:nowrap;">Anton's Lathe DRO<span id="conn-dot"></span></span>
  <span class="gear-icon" id="settings-toggle-btn">⚙</span>
</header>
<div id="conn-banner" onclick="location.reload()">CONNECTION LOST - VALUES MAY BE OUTDATED - TAP TO RELOAD</div>

<div id="settings-panel">
  <h2 style="color:#ff9900; border-bottom:1px solid #ff9900; padding-bottom:10px; margin-top:0;">Display Unit</h2>
  <div class="settings-row">
    <span>Length values shown as:</span>
    <div class="settings-control-group">
      <button class="unit-toggle-btn" id="unit-btn-mm" onclick="setDisplayUnit(false)">MM</button>
      <button class="unit-toggle-btn" id="unit-btn-inch" onclick="setDisplayUnit(true)">INCH</button>
    </div>
  </div>

  <h2 style="color:#ff9900; border-bottom:1px solid #ff9900; padding-bottom:10px; margin-top:26px;">Displayed Axes</h2>
  <div class="settings-row">
    <span>Number of axes (hold to confirm):</span>
    <div class="settings-control-group">
      <button class="cfg-btn" id="b_2">2X</button>
      <button class="cfg-btn" id="b_3">3X</button>
      <button class="cfg-btn" id="b_4">4X</button>
    </div>
  </div>

  <h2 style="color:#ff9900; border-bottom:1px solid #ff9900; padding-bottom:10px; margin-top:26px;">Axis Direction</h2>
  <div class="settings-row">
    <span>X-direction:</span>
    <div class="settings-control-group"><span id="dirx" class="dir">⟲</span></div>
  </div>
  <div class="settings-row">
    <span>Z-direction:</span>
    <div class="settings-control-group"><span id="dirz" class="dir">⟲</span></div>
  </div>
  <div id="dir-row-u" class="settings-row">
    <span>U-direction:</span>
    <div class="settings-control-group"><span id="diru" class="dir">⟲</span></div>
  </div>
  <div id="dir-row-w" class="settings-row">
    <span>W-direction:</span>
    <div class="settings-control-group"><span id="dirw" class="dir">⟲</span></div>
  </div>
  <p style="color:#888; font-size:1.0em; margin-top:10px;">Press and hold a direction indicator to invert that axis' counting polarity.</p>

  <h2 style="color:#ff9900; border-bottom:1px solid #ff9900; padding-bottom:10px; margin-top:26px;">Linear Scale Resolutions (Pulses/mm)</h2>
  
  <div class="settings-row">
    <span>Axis X Resolution:</span>
    <div class="settings-control-group">
      <select id="sel-res-x" onchange="syncSelectToInput('x')">
        <option value="200.0">5µm (200.0)</option>
        <option value="1000.0">1µm (1000.0)</option>
        <option value="custom">Custom...</option>
      </select>
      <input type="number" step="0.1" id="res-in-x" oninput="detectCustomInput('x')">
    </div>
  </div>
  
  <div id="res-row-u" class="settings-row">
    <span>Axis U Resolution:</span>
    <div class="settings-control-group">
      <select id="sel-res-u" onchange="syncSelectToInput('u')">
        <option value="200.0">5µm (200.0)</option>
        <option value="1000.0">1µm (1000.0)</option>
        <option value="custom">Custom...</option>
      </select>
      <input type="number" step="0.1" id="res-in-u" oninput="detectCustomInput('u')">
    </div>
  </div>
  
  <div class="settings-row">
    <span>Axis Z Resolution:</span>
    <div class="settings-control-group">
      <select id="sel-res-z" onchange="syncSelectToInput('z')">
        <option value="200.0">5µm (200.0)</option>
        <option value="1000.0">1µm (1000.0)</option>
        <option value="custom">Custom...</option>
      </select>
      <input type="number" step="0.1" id="res-in-z" oninput="detectCustomInput('z')">
    </div>
  </div>
  
  <div id="res-row-w" class="settings-row">
    <span>Axis W Resolution:</span>
    <div class="settings-control-group">
      <select id="sel-res-w" onchange="syncSelectToInput('w')">
        <option value="200.0">5µm (200.0)</option>
        <option value="1000.0">1µm (1000.0)</option>
        <option value="custom">Custom...</option>
      </select>
      <input type="number" step="0.1" id="res-in-w" oninput="detectCustomInput('w')">
    </div>
  </div>
  
  <p style="color:#888; font-size:1.1em; margin-top:20px;">Use the dropdown for quick selection, or type directly into the input fields for manual micro-calibration adjustments. Scale resolution is always calibrated in mm/µm, independent of the display unit above.</p>
  
  <div class="settings-action-container">
    <button class="btn-settings-save" onclick="saveResolutionsOnly()">SAVE RESOLUTIONS</button>
  </div>

  <hr class="settings-divider">
  <div class="settings-action-container">
    <button class="btn-settings-cancel" onclick="closeSettingsMenu()">CLOSE</button>
  </div>
</div>

<div class="container">
  <div class="axislabel"><span id="labelx" class="axisletter" onclick="toggleRef('X')">X</span></div><div class="value" id="x">0.00</div><div class="controls"><button class="input-trigger-btn" onclick="openNumpad('X')">I</button><button class="zero-btn" onclick="showZeroConfirm('X')">Z</button><div id="confirm-X" class="confirm-overlay"><span>sure?</span><button onclick="confirmZero('X')">YES</button></div><div id="mode" onclick="toggleMode()">R</div></div>
  <div class="axislabel"><span id="labelz" class="axisletter" onclick="toggleRef('Z')">Z</span></div><div class="value" id="z">0.00</div><div class="controls"><button class="input-trigger-btn" onclick="openNumpad('Z')">I</button><button class="zero-btn" onclick="showZeroConfirm('Z')">Z</button><div id="confirm-Z" class="confirm-overlay"><span>sure?</span><button onclick="confirmZero('Z')">YES</button></div><div class="ctrl-spacer"></div></div>
  <div id="u-row" class="row-wrapper"><div class="axislabel"><span id="labelu" class="axisletter" onclick="toggleRef('U')">U</span></div><div class="value" id="u">0.00</div><div class="controls"><button class="input-trigger-btn" onclick="openNumpad('U')">I</button><button class="zero-btn" onclick="showZeroConfirm('U')">Z</button><div id="confirm-U" class="confirm-overlay"><span>sure?</span><button onclick="confirmZero('U')">YES</button></div><div id="compound-toggle" onclick="openCompoundOverlay()">∠</div></div></div>
  <div id="w-row" class="row-wrapper"><div class="axislabel"><span id="labelw" class="axisletter" onclick="toggleRef('W')">W</span></div><div class="value" id="w">0.00</div><div class="controls"><button class="input-trigger-btn" onclick="openNumpad('W')">I</button><button class="zero-btn" onclick="showZeroConfirm('W')">Z</button><div id="confirm-W" class="confirm-overlay"><span>sure?</span><button onclick="confirmZero('W')">YES</button></div><div class="ctrl-spacer"></div></div></div>
</div>

<div id="numpad">
  <div id="numpad-title">SET VALUE: AXIS <span id="np-target-axis">-</span></div><div id="numpad-display"></div>
  <div class="np-grid">
    <button onclick="np(7)">7</button> <button onclick="np(8)">8</button> <button onclick="np(9)">9</button>
    <button onclick="np(4)">4</button> <button onclick="np(5)">5</button> <button onclick="np(6)">6</button>
    <button onclick="np(1)">1</button> <button onclick="np(2)">2</button> <button onclick="np(3)">3</button>
    <button onclick="npToggleSign()">±</button> <button onclick="np(0)">0</button> <button onclick="np('.')">.</button>
    <button class="np-exit" onclick="npExit()">X</button><button class="np-clear" onclick="npClear()">C</button><button class="np-set" onclick="executeGlobalSet()">SET</button>
  </div>
</div>

<div id="compound-overlay">
  <div id="compound-title">COMPOUND SLIDE</div>
  <div class="compound-row">
    <span class="compound-label">Angle</span>
    <span class="compound-value" id="compound-angle" onclick="openNumpad('ANGLE')">0.00</span>
    <button id="compound-sign-btn" onclick="toggleCompoundSign()">+</button>
  </div>
  <div class="compound-row">
    <span class="compound-label">U</span>
    <span class="compound-value-readonly" id="compound-u">0.00</span>
    <div class="compound-spacer"></div>
  </div>
  <div class="compound-row">
    <span class="compound-label">Delta X (radius)</span>
    <span class="compound-value-readonly" id="compound-dx">0.00</span>
    <button class="compound-zero-btn" onclick="zeroCompoundDelta('x')">Z</button>
  </div>
  <div class="compound-row">
    <span class="compound-label">Delta Z</span>
    <span class="compound-value-readonly" id="compound-dz">0.00</span>
    <button class="compound-zero-btn" onclick="zeroCompoundDelta('z')">Z</button>
  </div>
  <div class="compound-result-row">
    <span class="compound-label">X combined (dia.)</span>
    <span class="compound-result-value" id="compound-x-result">0.00</span>
  </div>
  <div class="compound-result-row">
    <span class="compound-label">Z combined</span>
    <span class="compound-result-value" id="compound-z-result">0.00</span>
  </div>
  <button id="compound-close-btn" onclick="closeCompoundOverlay()">CLOSE</button>
</div>

<div id="toolbar" style="position:fixed;bottom:8px;left:8px;right:8px;z-index:9999;">
  <button class="tool-fixed" id="t0" onclick="selectTool(0)">T1</button>
  <button class="tool-fixed" id="t1" onclick="selectTool(1)">T2</button>
  <button class="tool-fixed" id="t2" onclick="selectTool(2)">T3</button>
  <button class="tool-fixed" id="t3" onclick="selectTool(3)">T4</button>
  <button class="tool-fixed" id="t4" onclick="selectTool(4)">T5</button>
  <button class="tool-fixed" id="t5" onclick="selectTool(5)">T6</button>
  <button class="tool-fixed" id="t6" onclick="selectTool(6)">T7</button>
  <button class="tool-fixed" id="t7" onclick="selectTool(7)">T8</button>
  <button class="tool-fixed" id="t8" onclick="selectTool(8)">T9</button>
  <button class="tool-fixed" id="t9" onclick="selectTool(9)">T10</button>
  <!-- <button class="save-wide" id="save-btn" onclick="triggerNVSSave()">SAVE</button> -->
  <button class="save-wide" id="save-btn">SAVE</button>
</div>


<!-- 
 =======================================================================================
  CLIENT-SIDE CONTROLLER (FRONTEND LOGIC)
 ---------------------------------------------------------------------------------------
  PURPOSE: 
  Handles the dynamic UI, user interactions, and WebSocket communication.
  
  CORE RESPONSIBILITIES:
  1. WebSocket Management: Maintains a persistent link to the ESP32 for real-time 
     telemetry (JSON) and bi-directional command transmission. No automatic reconnect 
     by design - see Connection Watchdog below.
  2. UI Rendering: 
     - Updates the text displays for X/Z/U/W based on machine mode and the selected 
       display unit (mm/inch, converted client-side; see formatLength()).
     - Dynamically shows/hides axes rows (and their matching Settings entries) based on 
       the ESP32's configured axis_mode.
  3. Interaction Handling:
     - Implements "smart" logic for buttons (long-press for config, short-press for commands).
     - Manages the settings overlay and input validation for resolution/calibration.
  4. Compound Slide Helper: Client-side-only trigonometry (see computeCompound()) that 
     projects the U-axis (compound slide) reading, at an operator-entered angle, onto the 
     real X/Z machine axes - for taper and thread turning. Nothing here is persisted.
  5. Connection Watchdog: Tracks time since the last received packet (heartbeat or real 
     update) and marks the link as lost after ~3s of silence, or immediately on a socket 
     close/error event. Dims the readout and shows a tap-to-reload banner.
  6. Audio Feedback: Provides tactile-like confirmation sounds (Oscillator-based) 
     for every user interaction.
 =======================================================================================
-->
<script>
let ws=new WebSocket("ws://"+location.host+"/ws");
let X_dir=1,U_dir=1,Z_dir=1,W_dir=1,currentMode="R",targetAxis=null,currentAxes=null,numpadValue="",audioCtx,pressTimer,configTimer,lastData=null,confirmTimers={},activeTool=0,saveTimer,settingsTimer;
let compoundAngleDeg=0,compoundSign=1,compoundDeltaXZeroOffset=0,compoundDeltaZZeroOffset=0,compoundRawDeltaX=0,compoundRawDeltaZ=0;
let unitInch=false,connLost=false,connLastMessageTime=Date.now();
// Plays a short confirmation tone via the Web Audio oscillator (f = frequency in Hz, d = duration in seconds).
function s(f,d){audioCtx=audioCtx||new(AudioContext||webkitAudioContext)();let o=audioCtx.createOscillator(),g=audioCtx.createGain();o.connect(g);g.connect(audioCtx.destination);o.frequency.value=f;g.gain.value=0.15;o.start();o.stop(audioCtx.currentTime+d);}

// Converts a millimeter value to the currently selected display unit and formats it.
// Calibration/protocol always stay in mm - this is purely a display-layer conversion.
function formatLength(mmValue){
    if(unitInch)return (mmValue/25.4).toFixed(4);
    return mmValue.toFixed(2);
}

// Connection watchdog: no automatic reconnect on purpose (explicit choice - a silent auto-retry
// could mask a real problem). Once lost, the indicator stays red/dimmed until the page is reloaded.
function markConnectionLost(){
    if(connLost)return;
    connLost=true;
    document.body.classList.add("conn-lost");
    s(220,0.4);
}
ws.onclose=function(){markConnectionLost();};
ws.onerror=function(){markConnectionLost();};
setInterval(()=>{
    if(!connLost && (Date.now()-connLastMessageTime>3000)){markConnectionLost();}
},500);

// Sends the persisted display-unit preference (mm/inch) to the ESP32 (saved in NVS).
function setDisplayUnit(isInch){
    ws.send(isInch?"SET_UNIT_INCH":"SET_UNIT_MM");
    s(900,0.04);
}
// Shows the inline "sure?" confirmation bubble for one axis' Zero button; hides any other
// axis' open bubble first, and auto-dismisses this one after 2s if not confirmed.
function showZeroConfirm(axis){s(700,0.03);['X','U','Z','W'].forEach(ax=>{let el=document.getElementById("confirm-"+ax);if(el)el.style.display="none";});let overlay=document.getElementById("confirm-"+axis);if(overlay){overlay.style.display="flex";clearTimeout(confirmTimers[axis]);confirmTimers[axis]=setTimeout(()=>{overlay.style.display="none";},2000);}}
// User tapped YES on the confirmation bubble - sends ZERO_<axis> and hides the bubble.
function confirmZero(axis){let overlay=document.getElementById("confirm-"+axis);if(overlay)overlay.style.display="none";ws.send("ZERO_"+axis);s(500,0.07);}
// Toggles one axis between ABS (green) and INC (yellow) - triggered by tapping the axis letter.
function toggleRef(axis){ws.send("TOGGLE_REF_"+axis);s(850,0.04);}
// Recolors an axis letter (or Settings badge) green/yellow to reflect its current ABS/INC state.
// Recolors an axis letter AND its value readout green/yellow to reflect ABS/INC state.
// axis is the lowercase id suffix (x/z/u/w) shared by the letter ("label"+axis) and the value (axis).
function updateAxisRefColor(axis, ref){
    let isInc=(ref==="INC");
    document.getElementById("label"+axis).classList.toggle("inc", isInc);
    document.getElementById(axis).classList.toggle("inc", isInc);
}
// Switches the active tool profile (idx 0-9 = T1-T10); pitch of the confirmation tone rises with the index.
function selectTool(idx){ws.send("SET_TOOL:"+idx);s(1000+idx*100,0.05);}
// Persists tool offsets, per-axis ABS/INC + direction state, active tool, and raw hardware counters to NVS.
function triggerNVSSave(){ws.send("SAVE_PREFS");s(1500,0.25);}

// SMART DROPDOWN INTERACTION LOGIC
function setDropdownValueOrCustom(axis, val) {
    let selectEl = document.getElementById("sel-res-" + axis);
    let inputEl = document.getElementById("res-in-" + axis);
    inputEl.value = val;
    
    // Prüfen, ob der reingeladene Wert mit einem Standard-Dropdown-Eintrag übereinstimmt
    if (val === 200.0 || val === 1000.0) {
        selectEl.value = val.toFixed(1);
    } else {
        selectEl.value = "custom";
    }
}

// Dropdown changed to a preset value - copies it into the manual input field.
function syncSelectToInput(axis) {
    let selectEl = document.getElementById("sel-res-" + axis);
    let inputEl = document.getElementById("res-in-" + axis);
    if (selectEl.value !== "custom") {
        inputEl.value = parseFloat(selectEl.value);
    }
    s(1000, 0.02);
}

// Manual input field changed - flips the dropdown to "Custom..." unless it matches a known preset.
function detectCustomInput(axis) {
    let selectEl = document.getElementById("sel-res-" + axis);
    let inputEl = document.getElementById("res-in-" + axis);
    let val = parseFloat(inputEl.value);
    if (val === 200.0 || val === 1000.0) {
        selectEl.value = val.toFixed(1);
    } else {
        selectEl.value = "custom";
    }
}

// Settings Overlay Management
function openSettingsMenu(){
    if(!lastData) return;
    setDropdownValueOrCustom('x', lastData.res_x);
    setDropdownValueOrCustom('u', lastData.res_u);
    setDropdownValueOrCustom('z', lastData.res_z);
    setDropdownValueOrCustom('w', lastData.res_w);
    
    document.getElementById("settings-panel").style.display="block";
    document.getElementById("settings-toggle-btn").classList.add("active");
    s(750,0.06);
}
function closeSettingsMenu(){
    document.getElementById("settings-panel").style.display="none";
    document.getElementById("settings-toggle-btn").classList.remove("active");
    s(550,0.05);
}
function saveResolutionsOnly(){
    let rx=document.getElementById("res-in-x").value;
    let ru=document.getElementById("res-in-u").value;
    let rz=document.getElementById("res-in-z").value;
    let rw=document.getElementById("res-in-w").value;
    if(rx>0 && ru>0 && rz>0 && rw>0){
        ws.send("SAVE_RES:"+rx+";"+ru+";"+rz+";"+rw);
        s(1400,0.15);
        closeSettingsMenu();
    }
}

// Refreshes the X/Z/U/W text values from the last received packet, formatted in the current display unit.
function forceRenderDisplays(){if(!lastData)return;x.innerHTML=formatLength(lastData.x);z.innerHTML=formatLength(lastData.z);if(lastData.axes>=3)u.innerHTML=formatLength(lastData.u);if(lastData.axes===4)w.innerHTML=formatLength(lastData.w);}
// Generic long-press helper: starts a hold-timer via startCallback() on touchstart; endCallback()
// runs on touchend regardless of whether the hold completed (caller decides what "released early"
// vs. "hold completed" means, typically by checking/clearing its own timer inside the callbacks).
function bindLongPress(elementId,startCallback,endCallback){let el=document.getElementById(elementId);if(!el)return;el.addEventListener("touchstart",function(e){e.preventDefault();startCallback();},{passive:false});el.addEventListener("touchend",function(e){e.preventDefault();endCallback();},{passive:false});}

window.addEventListener("DOMContentLoaded",()=>{
    
    // Connection-lost banner: reload on tap. Bound as a real touch handler (not just the
    // onclick attribute) because Safari/iOS tends to suppress the synthesized click event
    // when touch-action:none is active on body, unlike Android/Chrome. onclick stays as a
    // fallback for non-touch input.
    document.getElementById("conn-banner").addEventListener("touchend", function(e){
        e.preventDefault();
        location.reload();
    }, {passive:false});

    // Compound-overlay drag-to-move (title, tablet breakpoint only - see compoundDragStart)
    let compoundTitleEl = document.getElementById("compound-title");
    compoundTitleEl.addEventListener("touchstart", compoundDragStart, {passive:false});
    compoundTitleEl.addEventListener("touchmove", compoundDragMove, {passive:false});
    compoundTitleEl.addEventListener("touchend", compoundDragEnd, {passive:false});
    compoundTitleEl.addEventListener("touchcancel", compoundDragEnd, {passive:false});

    // Long-press: Invert axis direction
    // Wait 800ms before sending the DIR_X/Z/U/W command to the ESP32
    ['x', 'u', 'z', 'w'].forEach(ax => {      // Loop through each axis identifier
        bindLongPress("dir" + ax, () => {     // Bind the long-press event to the button ID (e.g., "dirx")
            pressTimer = setTimeout(() => {   // Start timer to prevent accidental triggering
                s(700, 0.07);                 // Trigger acoustic feedback (frequency 700Hz, duration 0.07s)

                // 1. Identify the global direction variable (e.g., X_dir, U_dir...)
                let varName = ax.toUpperCase() + "_dir";
                
                // 2. Invert the value (toggle between 1 and -1)
                window[varName] = (window[varName] === 1) ? -1 : 1;
                
                // 3. Reference the UI element (matching onmessage implementation)
                let el = window["dir" + ax]; 
                
                // 4. Toggle the CSS class (same logic as in onmessage)
                el.classList.toggle("inv", window[varName] === -1);

                // Send inverted direction command to ESP32
                //ws.send("DIR_" + ax.toUpperCase() + ":" + (-window[ax.toUpperCase() + "_dir"]));
                ws.send("DIR_" + ax.toUpperCase() + ":" + window[varName]);
            }, 800);    // 800ms hold time required
        }, () => clearTimeout(pressTimer));   // Clear timer if button is released early
    });

    // Long-press Buttons [2X] [3]X [4X]
    [2, 3, 4].forEach(num => {
        bindLongPress("b_" + num, () => {
            configTimer = setTimeout(() => {
                s(800, 0.15);
                ws.send("SET_AXIS_MODE:" + num);
            }, 1000);
        }, () => clearTimeout(configTimer));
    });

    // Long-press: Securely open the settings menu
    // Wait 800ms hold time to prevent accidental clicks on the lathe
    let settingsLongPressFired = false;
    bindLongPress("settings-toggle-btn", () => {
        settingsLongPressFired = false;
        settingsTimer = setTimeout(() => { // Start the timer for long-press
            settingsLongPressFired = true; // Mark: this touch cycle already opened the menu
            s(700, 0.07);                  // Play acoustic feedback
            openSettingsMenu();            // Trigger the function
        }, 800);                           // Define the 800ms threshold
    }, () => {
        clearTimeout(settingsTimer); // Cancel timer if released early
        if (settingsLongPressFired) {
            // Finger is being lifted right after the long-press just opened the menu -
            // this is the SAME gesture, not a new tap, so don't immediately close it again.
            settingsLongPressFired = false;
            return;
        }
        // Quick tap (timer never fired) closes the menu if it was already open from before.
        if (document.getElementById("settings-panel").style.display === "block") {
            closeSettingsMenu();
        }
    });

    // Long-press Save-Button
    bindLongPress("save-btn",()=>{
        saveTimer=setTimeout(()=>{
          s(1500,0.25);
          ws.send("SAVE_PREFS");
        },1000);
    },()=>clearTimeout(saveTimer));

    // Tapping anywhere outside the axis controls / settings closes any still-open
    // Zero-confirmation bubble, so it doesn't linger if the user changes their mind.
    document.body.addEventListener("touchstart",function(e){
        if(!e.target.closest('.controls')&&!e.target.closest('#settings-toggle-btn')&&!e.target.closest('#settings-panel')){
            ['X','U','Z','W'].forEach(ax=>{let el=document.getElementById("confirm-"+ax);if(el)el.style.display="none";});
        }
    },{passive:true});
});

// TOUCH NUMPAD - shared by all axes (X/Z/U/W) and the compound-slide ANGLE field.
// openNumpad() sets targetAxis; np()/npClear()/npToggleSign() build up numpadValue as a string;
// executeGlobalSet() interprets and applies it depending on what targetAxis currently is.
function openNumpad(axis){targetAxis=axis;numpadValue="";document.getElementById("np-target-axis").innerText=axis;document.getElementById("numpad-display").innerText="0";document.getElementById("numpad").style.display="block";s(900,0.03);}
function closeNumpad(){targetAxis=null;numpadValue="";document.getElementById("numpad").style.display="none";}
function updateNpDisplay(){document.getElementById("numpad-display").innerText=numpadValue||"0";}
function np(v){if(!targetAxis)return;s(1200,0.02);numpadValue+=v;updateNpDisplay();}
function npClear(){s(600,0.04);numpadValue="";updateNpDisplay();}
function npExit(){s(550,0.05);closeNumpad();}
function npToggleSign(){if(!targetAxis)return;s(1100,0.02);numpadValue=numpadValue.startsWith("-")?numpadValue.substring(1):"-"+numpadValue;updateNpDisplay();}
// Applies the typed numpad value. Two cases:
// - targetAxis === "ANGLE": client-side only (compound slide), magnitude is clamped to 0-90 deg
//   and never sent to the ESP32 - the separate +/- button controls sign, not the numpad's own sign.
// - targetAxis === 'X'/'Z'/'U'/'W': sent to the ESP32 as SET_<axis>:<mm value>. If the display is
//   currently in inch mode, the typed value is converted inch->mm first (protocol always uses mm).
function executeGlobalSet(){if(!targetAxis||!numpadValue)return;if(targetAxis==="ANGLE"){let val=Math.abs(parseFloat(numpadValue));if(isNaN(val))val=0;if(val>90)val=90;compoundAngleDeg=val;document.getElementById("compound-angle").innerText=compoundAngleDeg.toFixed(2);computeCompound();closeNumpad();s(1400,0.06);return;}let mmVal=parseFloat(numpadValue);if(isNaN(mmVal))mmVal=0;if(unitInch)mmVal=mmVal*25.4;ws.send("SET_"+targetAxis+":"+mmVal);closeNumpad();s(1400,0.06);}

// Applies the 2/3/4-axis layout: sets the body mode-N class, highlights the active button
// (relocated into Settings > Displayed Axes), and shows/hides the U/W rows plus their
// matching Settings entries (resolution + direction) together.
function updateLayout(axes){currentAxes=axes;document.body.classList.remove("mode-2","mode-3","mode-4");document.body.classList.add("mode-"+axes);document.getElementById("b_2").classList.toggle("active",axes===2);document.getElementById("b_3").classList.toggle("active",axes===3);document.getElementById("b_4").classList.toggle("active",axes===4);document.getElementById("u-row").style.display=(axes>=3)?"contents":"none";document.getElementById("w-row").style.display=(axes===4)?"contents":"none";document.getElementById("res-row-u").style.display=(axes>=3)?"flex":"none";document.getElementById("res-row-w").style.display=(axes===4)?"flex":"none";document.getElementById("dir-row-u").style.display=(axes>=3)?"flex":"none";document.getElementById("dir-row-w").style.display=(axes===4)?"flex":"none";}

ws.onmessage = e => {
    let d = JSON.parse(e.data); // Parse incoming JSON data
    lastData = d;               // Store last received data
    connLastMessageTime = Date.now(); // Watchdog: any packet (incl. heartbeat) counts as "alive"

    // Sync display unit preference (persisted server-side in NVS)
    unitInch = (d.unit === "IN");
    document.body.classList.toggle("unit-inch", unitInch);
    document.getElementById("unit-btn-mm").classList.toggle("active", !unitInch);
    document.getElementById("unit-btn-inch").classList.toggle("active", unitInch);

    // Update layout if axes configuration changed
    if (d.axes !== currentAxes) updateLayout(d.axes);
    forceRenderDisplays();      // Refresh DRO displays

    // Update operation mode and color
    currentMode = d.mode;
    mode.innerHTML = currentMode;
    mode.style.color = (currentMode === "D") ? "#ff0" : "#0f0";

    // Update per-axis ABS/INC badges
    updateAxisRefColor("x", d.ref_x);
    updateAxisRefColor("z", d.ref_z);
    updateAxisRefColor("u", d.ref_u);
    updateAxisRefColor("w", d.ref_w);

    // Update axis direction variables
    X_dir = d.dir_x;
    U_dir = d.dir_u;
    Z_dir = d.dir_z;
    W_dir = d.dir_w;

    // Toggle CSS class for visual direction indicators
    dirx.classList.toggle("inv", X_dir === -1);
    diru.classList.toggle("inv", U_dir === -1);
    dirz.classList.toggle("inv", Z_dir === -1);
    dirw.classList.toggle("inv", W_dir === -1);

    // Update active tool indicator
    for (let i = 0; i < 10; i++) {
        document.getElementById("t" + i).classList.toggle("active", d.tool === i);
    }

    computeCompound();
};


// Toggles the X-axis between Radius (R) and Diameter (D, x2 reading) display mode.
function toggleMode(){ws.send("MODE");s(750,0.05);}

// Compound slide (angle) helper - fully client-side, nothing sent to / stored on the ESP32.
// Angle is entered as magnitude only (0-90 deg) via the numpad; direction is a separate sign toggle.
// Delta X / Delta Z are derived from the current U (compound slide) reading and can be zeroed
// independently of U's own zero point. Combined X/Z show the resulting tool tip position.
// Sign logic: the +/- button represents the sign of the swivel angle itself. Delta Z = U*cos(angle)
// always follows U's own sign (cos is an even function - swivel side doesn't affect it), while
// Delta X = sign*U*sin(angle) does flip with the button (sin is an odd function).
function computeCompound(){
    if(!lastData)return;
    let rad=compoundAngleDeg*Math.PI/180;
    compoundRawDeltaX=compoundSign*lastData.u*Math.sin(rad);
    compoundRawDeltaZ=lastData.u*Math.cos(rad);
    let deltaX=compoundRawDeltaX-compoundDeltaXZeroOffset;
    let deltaZ=compoundRawDeltaZ-compoundDeltaZZeroOffset;
    let radiusTool=(currentMode==="D")?(lastData.x/2):lastData.x;
    let combinedX=2*(radiusTool+deltaX);
    let combinedZ=lastData.z+deltaZ;
    document.getElementById("compound-u").innerText=formatLength(lastData.u);
    document.getElementById("compound-dx").innerText=formatLength(deltaX);
    document.getElementById("compound-dz").innerText=formatLength(deltaZ);
    document.getElementById("compound-x-result").innerText=formatLength(combinedX);
    document.getElementById("compound-z-result").innerText=formatLength(combinedZ);
}
// Freezes the current raw Delta X or Delta Z as that axis' own zero offset (independent of U's zero).
function zeroCompoundDelta(axis){
    if(axis==="x")compoundDeltaXZeroOffset=compoundRawDeltaX;else compoundDeltaZZeroOffset=compoundRawDeltaZ;
    s(500,0.07);
    computeCompound();
}
// Flips the swivel-direction sign (+/-), which only affects Delta X (see computeCompound comment above).
function toggleCompoundSign(){
    compoundSign=-compoundSign;
    document.getElementById("compound-sign-btn").innerText=(compoundSign===1)?"+":"\u2212";
    s(750,0.05);
    computeCompound();
}
// Opens the overlay, always re-centered (any previous drag position from a tablet session is discarded).
function openCompoundOverlay(){
    let overlay=document.getElementById("compound-overlay");
    overlay.style.top="50%";overlay.style.left="50%";overlay.style.transform="translate(-50%, -50%)";
    overlay.style.display="block";
    document.getElementById("compound-toggle").classList.add("active");
    computeCompound();
    s(900,0.03);
}
function closeCompoundOverlay(){
    document.getElementById("compound-overlay").style.display="none";
    document.getElementById("compound-toggle").classList.remove("active");
}

// Drag-to-move via touch-and-hold on the overlay title - iPad/tablet only (matches the
// tablet CSS breakpoint). On phones (Pixel 9) moving the overlay is not useful, so it stays disabled.
let compoundDragActive=false,compoundDragOffsetX=0,compoundDragOffsetY=0;
// True when the viewport matches the same breakpoint the CSS uses for the iPad/tablet layout.
function isTabletBreakpoint(){return window.matchMedia("(min-width: 700px) and (max-width: 1400px) and (min-height: 500px)").matches;}
function compoundDragStart(e){
    if(!isTabletBreakpoint())return;
    e.preventDefault();
    let overlay=document.getElementById("compound-overlay");
    let rect=overlay.getBoundingClientRect();
    overlay.style.left=rect.left+"px";overlay.style.top=rect.top+"px";overlay.style.transform="none";
    let touch=e.touches[0];
    compoundDragOffsetX=touch.clientX-rect.left;compoundDragOffsetY=touch.clientY-rect.top;
    compoundDragActive=true;
}
function compoundDragMove(e){
    if(!compoundDragActive)return;
    e.preventDefault();
    let overlay=document.getElementById("compound-overlay");
    let touch=e.touches[0];
    let newLeft=Math.max(0,Math.min(window.innerWidth-overlay.offsetWidth,touch.clientX-compoundDragOffsetX));
    let newTop=Math.max(0,Math.min(window.innerHeight-overlay.offsetHeight,touch.clientY-compoundDragOffsetY));
    overlay.style.left=newLeft+"px";overlay.style.top=newTop+"px";
}
function compoundDragEnd(){compoundDragActive=false;}
// Suppresses the browser's default touch behavior (pull-to-refresh, rubber-band scrolling)
// when the touch starts directly on empty background (body) or the axis grid, so the app
// keeps feeling like a fixed kiosk display rather than a scrollable web page.
document.addEventListener("touchstart",function(e){if(e.target.tagName==="BODY"||e.target.classList.contains("container")){e.preventDefault();}},{passive:false});
</script>
</body>
</html>
        )HTML");
    });

    /*
    =======================================================================================
     TASK SCHEDULING: NETWORK BROADCASTER
    ---------------------------------------------------------------------------------------
     Spawns the NetworkTask on Core 0 to ensure that web communications and telemetry
     broadcasting do not interfere with the high-priority hardware counting on Core 1.
     - Stack Size (8192): Allocated for WebSocket buffer and JSON string processing.
     - Priority (1): Low priority to ensure hardware-level tasks are never starved.
    =======================================================================================
    */
    xTaskCreatePinnedToCore(NetworkTaskCode, "NetworkTask", 8192, NULL, 1, &NetworkTaskHandle, 0);
}




/* ===================================================================================
  BLOCK: DETERMINISTIC MATH ENGINE PROCESSING LOOP (RUNNING ENDLESSLY ON CORE 1)
 -----------------------------------------------------------------------------------
  PURPOSE: 
  Calculates the physical position in millimeters from raw encoder pulses.
  This loop runs as fast as possible on Core 1 to ensure minimal latency 
  between hardware movement and data availability.

  PIPELINE: 
  1. Retrieval: Fetches raw counter values from PCNT hardware units.
  2. Offset Application: Selects between Absolute/Incremental offsets per tool.
  3. Scaling/Calibration: Converts counts to mm using `counts_per_mm` factors 
     and applies the axis direction (DIR) to handle mounting orientation.
  4. Mode Handling: Applies Diameter scaling (x2) if in turning/diameter mode.
  5. Thread-Safe Export: Publishes results to shared variables via Mutex.
 ===================================================================================
*/

void loop() {
    // 1. Fetch raw hardware counts
    int32_t tx = get_absolute_counts(PCNT_UNIT_0, global_total_x);
    int32_t tz = get_absolute_counts(PCNT_UNIT_1, global_total_z);
    int32_t tu = get_absolute_counts(PCNT_UNIT_2, global_total_u);
    int32_t tw = get_absolute_counts(PCNT_UNIT_3, global_total_w);

    // 2. Determine currently active offset based on Mode (INC/ABS, per axis) and Tool selection
    float current_offset_x = is_inc_mode_x ? offset_x_INC[current_tool] : offset_x_ABS[current_tool];
    float current_offset_u = is_inc_mode_u ? offset_u_INC[current_tool] : offset_u_ABS[current_tool];
    float current_offset_z = is_inc_mode_z ? offset_z_INC[current_tool] : offset_z_ABS[current_tool];
    float current_offset_w = is_inc_mode_w ? offset_w_INC[current_tool] : offset_w_ABS[current_tool];

    // 3. Math Engine: Apply scaling factors and directional inversion
    float cal_x = (tx * X_DIR) / counts_per_mm[0] + current_offset_x;
    if (diameter_mode) cal_x *= 2.0f;
    
    float cal_z = (tz * Z_DIR) / counts_per_mm[2] + current_offset_z;
    float cal_u = (tu * U_DIR) / counts_per_mm[1] + current_offset_u;
    float cal_w = (tw * W_DIR) / counts_per_mm[3] + current_offset_w;

    // 4. Thread-Safe Export: Update shared variables for the NetworkTask (Broadcaster)
    // We use a short timeout (5ms) to ensure the loop doesn't stall if the NetworkTask is busy
    if (xSemaphoreTake(positionMutex, int(5)) == pdTRUE) {
        shared_x = cal_x; shared_u = cal_u; shared_z = cal_z; shared_w = cal_w;
        xSemaphoreGive(positionMutex); 
    }
    // Slight delay to prevent CPU starvation and allow task switching
    delay(1);
}