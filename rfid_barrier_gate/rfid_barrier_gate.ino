/*
 * =====================================================================================
 *   RFID-Controlled Worm Gear Barrier Gate System
 *   Target MCU: Arduino Nano (ATmega328P) / Uno
 *   Actuator:   12V DC Worm Gear Motor (5840-31ZY / Wiper Motor)
 *   Driver:     BTS7960 43A High-Power H-Bridge
 *   Power:      12V 7Ah SLA Battery -> LM2596 Buck Converter (5.0V Logic Rail)
 *   Position:   2x Micro Limit Switches with Roller Levers (0° Closed & 90° Open)
 *   Access:     MFRC522 RFID Reader (13.56 MHz SPI Module)
 *   Mechanics:  100cm ACP Barrier Arm pivoted at 20cm mark (80cm boom, no counterweight)
 *
 *   REQUIRED ARDUINO LIBRARIES:
 *     - "MFRC522" by GithubCommunity / Miguel Balboa
 *     - "SPI" (Built-in)
 * =====================================================================================
 */

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>

// =====================================================================================
//  PIN ASSIGNMENTS
// =====================================================================================

// Physical Position Tracking (Micro Limit Switches with Roller Levers)
// Wired: NO -> Arduino Pin, COM -> GND (Internal INPUT_PULLUP active LOW)
constexpr uint8_t PIN_LIMIT_OPEN   = 2;  // Limit switch at 90° (Barrier fully raised)
constexpr uint8_t PIN_LIMIT_CLOSED = 3;  // Limit switch at 0°  (Barrier fully lowered)

// BTS7960 43A Motor Driver
// Hardware Note: Connect both BTS7960 L_EN and R_EN directly to the Nano 5V pin.
constexpr uint8_t PIN_MOTOR_LPWM   = 5;  // BTS7960 LPWM (Forward / Gate Open)
constexpr uint8_t PIN_MOTOR_RPWM   = 6;  // BTS7960 RPWM (Reverse / Gate Close)

// RC522 RFID Transceiver (SPI Interface)
// Hardware Note: VCC MUST be connected to Nano 3.3V ONLY! (5V will destroy the module)
constexpr uint8_t PIN_RC522_RST    = 9;   // RC522 Reset Pin
constexpr uint8_t PIN_RC522_SS     = 10;  // RC522 Slave Select / SDA Pin
// Hardware SPI Pins on ATmega328P: MOSI = D11, MISO = D12, SCK = D13

// Audio-Visual Feedback Indicators
constexpr uint8_t PIN_LED_GREEN    = 4;   // Green LED (Access Granted) via 220Ω resistor
constexpr uint8_t PIN_LED_RED      = 7;   // Red LED (Standby / Access Denied) via 220Ω resistor
constexpr uint8_t PIN_BUZZER       = 8;   // 5V Active Piezo Buzzer

// =====================================================================================
//  TUNING & TIMING PARAMETERS
// =====================================================================================
constexpr uint8_t  MOTOR_SPEED_PWM  = 220;    // Motor run speed (0-255, ~86% duty cycle, high lift torque)
constexpr uint16_t GATE_HOLD_MS     = 5000;   // Dwell duration before barrier auto-closes (5s)
constexpr uint16_t MOTOR_TIMEOUT_MS = 10000;  // Safety cutoff: stops motor if switch isn't hit in 10s
constexpr uint8_t  RAMP_STEP        = 10;     // PWM increment per soft-start step
constexpr uint8_t  RAMP_INTERVAL_MS = 15;     // Delay per step for smooth acceleration

// =====================================================================================
//  AUTHORIZED RFID DATABASE (Mifare Classic 1K — 4-Byte UIDs)
// =====================================================================================
constexpr size_t UID_LENGTH = 4;

// Add your authorized physical card / keyfob hexadecimal UIDs here:
const uint8_t AUTHORIZED_UIDS[][UID_LENGTH] = {
    {0xDE, 0xAD, 0xBE, 0xEF}, // Master / Security Tag
    {0xA1, 0xB2, 0xC3, 0xD4}, // Staff Fob
    {0x73, 0x4B, 0x92, 0x1A}  // Resident Vehicle Fob
};

constexpr size_t AUTHORIZED_COUNT = sizeof(AUTHORIZED_UIDS) / sizeof(AUTHORIZED_UIDS[0]);

// =====================================================================================
//  HARDWARE OBJECTS
// =====================================================================================
MFRC522 rfid(PIN_RC522_SS, PIN_RC522_RST);

// =====================================================================================
//  LOW-LEVEL MOTOR FUNCTIONS (BTS7960 H-BRIDGE)
// =====================================================================================

inline void motorStop() {
    analogWrite(PIN_MOTOR_LPWM, 0);
    analogWrite(PIN_MOTOR_RPWM, 0);
}

inline void motorForward(uint8_t speed) {
    analogWrite(PIN_MOTOR_RPWM, 0);
    analogWrite(PIN_MOTOR_LPWM, speed);
}

inline void motorReverse(uint8_t speed) {
    analogWrite(PIN_MOTOR_LPWM, 0);
    analogWrite(PIN_MOTOR_RPWM, speed);
}

inline bool isGateOpenSwitchTriggered() {
    return digitalRead(PIN_LIMIT_OPEN) == LOW; // Active LOW with INPUT_PULLUP
}

inline bool isGateClosedSwitchTriggered() {
    return digitalRead(PIN_LIMIT_CLOSED) == LOW; // Active LOW with INPUT_PULLUP
}

/**
 * Soft-start acceleration:
 * Slowly ramps PWM from 0 up to target speed to prevent mechanical shock
 * on the flange coupling, worm gear teeth, and pillow block bearing.
 */
void softStart(bool forward) {
    for (uint8_t pwm = 0; pwm < MOTOR_SPEED_PWM; pwm += RAMP_STEP) {
        if (forward) {
            motorForward(pwm);
            if (isGateOpenSwitchTriggered()) {
                motorStop();
                return;
            }
        } else {
            motorReverse(pwm);
            if (isGateClosedSwitchTriggered()) {
                motorStop();
                return;
            }
        }
        delay(RAMP_INTERVAL_MS);
    }

    if (forward) {
        motorForward(MOTOR_SPEED_PWM);
    } else {
        motorReverse(MOTOR_SPEED_PWM);
    }
}

/**
 * Opens barrier arm: Drives motor forward until 90° limit switch activates.
 * Includes a safety timeout failsafe.
 */
void openBarrier() {
    if (isGateOpenSwitchTriggered()) {
        Serial.println(F(">> Gate already in OPEN position."));
        return;
    }

    Serial.println(F(">> Actuating: Raising barrier arm..."));
    softStart(true);

    unsigned long startTime = millis();
    while (!isGateOpenSwitchTriggered()) {
        if (millis() - startTime > MOTOR_TIMEOUT_MS) {
            motorStop();
            Serial.println(F("!! SAFETY TIMEOUT: Open limit switch not reached! Motor stopped."));
            return;
        }
    }

    motorStop();
    // Worm gear is mechanically self-locking — no holding current needed!
    Serial.println(F(">> Gate fully OPEN (90° switch reached)."));
}

/**
 * Closes barrier arm: Drives motor in reverse until 0° limit switch activates.
 * Includes a safety timeout failsafe.
 */
void closeBarrier() {
    if (isGateClosedSwitchTriggered()) {
        Serial.println(F(">> Gate already in CLOSED position."));
        return;
    }

    Serial.println(F(">> Actuating: Lowering barrier arm..."));
    softStart(false);

    unsigned long startTime = millis();
    while (!isGateClosedSwitchTriggered()) {
        if (millis() - startTime > MOTOR_TIMEOUT_MS) {
            motorStop();
            Serial.println(F("!! SAFETY TIMEOUT: Closed limit switch not reached! Motor stopped."));
            return;
        }
    }

    motorStop();
    Serial.println(F(">> Gate fully CLOSED (0° switch reached)."));
}

// =====================================================================================
//  FEEDBACK HELPERS (LEDS & BUZZER)
// =====================================================================================

void setFeedbackIdle() {
    digitalWrite(PIN_LED_RED, HIGH);   // Solid Red = Barrier down / Standby
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_BUZZER, LOW);
}

void signalAccessGranted() {
    digitalWrite(PIN_LED_RED, LOW);
    digitalWrite(PIN_LED_GREEN, HIGH); // Solid Green = Passage permitted

    // Single pleasant confirmation chime
    digitalWrite(PIN_BUZZER, HIGH);
    delay(150);
    digitalWrite(PIN_BUZZER, LOW);
}

void signalAccessDenied() {
    digitalWrite(PIN_LED_GREEN, LOW);

    // 3 rapid warning flashes & alert beeps
    for (uint8_t i = 0; i < 3; i++) {
        digitalWrite(PIN_LED_RED, HIGH);
        digitalWrite(PIN_BUZZER, HIGH);
        delay(100);
        digitalWrite(PIN_LED_RED, LOW);
        digitalWrite(PIN_BUZZER, LOW);
        delay(100);
    }
    digitalWrite(PIN_LED_RED, HIGH); // Return to idle state
}

// =====================================================================================
//  AUTHENTICATION LOGIC
// =====================================================================================

bool verifyUID(const byte* scannedUid, byte length) {
    if (length != UID_LENGTH) {
        return false;
    }

    for (size_t i = 0; i < AUTHORIZED_COUNT; i++) {
        bool match = true;
        for (size_t b = 0; b < UID_LENGTH; b++) {
            if (scannedUid[b] != AUTHORIZED_UIDS[i][b]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

void printUID(const byte* scannedUid, byte length) {
    Serial.print(F("Scanned Card UID: "));
    for (byte i = 0; i < length; i++) {
        if (scannedUid[i] < 0x10) Serial.print(F("0"));
        Serial.print(scannedUid[i], HEX);
        if (i < length - 1) Serial.print(F(":"));
    }
    Serial.println();
}

// =====================================================================================
//  SETUP ROUTINE
// =====================================================================================

void setup() {
    Serial.begin(9600);
    while (!Serial && millis() < 3000); // Wait for serial monitor or 3s timeout

    Serial.println(F("=========================================================="));
    Serial.println(F("   RFID Barrier Gate — 12V Worm Gear Access System        "));
    Serial.println(F("   Controller: Arduino Nano | Driver: BTS7960 43A         "));
    Serial.println(F("   Positioning: Roller Limit Switches (0° / 90°)          "));
    Serial.println(F("=========================================================="));

    // 1. Initialize Limit Switch Inputs with Internal Pull-Ups
    pinMode(PIN_LIMIT_OPEN, INPUT_PULLUP);
    pinMode(PIN_LIMIT_CLOSED, INPUT_PULLUP);

    // 2. Initialize BTS7960 Motor Driver PWM Pins
    pinMode(PIN_MOTOR_LPWM, OUTPUT);
    pinMode(PIN_MOTOR_RPWM, OUTPUT);
    motorStop();

    // 3. Initialize Audio-Visual Feedback Pins
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    setFeedbackIdle();

    // 4. Initialize Hardware SPI & RC522 RFID Reader
    SPI.begin();
    rfid.PCD_Init();
    delay(50);
    rfid.PCD_DumpVersionToSerial();

    // 5. Initial Homing Routine: Ensure gate starts in the closed position
    if (!isGateClosedSwitchTriggered()) {
        Serial.println(F(">> Homing Routine: Barrier not in closed position. Closing now..."));
        closeBarrier();
    } else {
        Serial.println(F(">> System Initialized: Barrier verified at 0° (Closed)."));
    }

    Serial.println(F(">> Ready for RFID scan. Tap card on reader..."));
}

// =====================================================================================
//  MAIN LOOP
// =====================================================================================

void loop() {
    // 1. Check for card presence
    if (!rfid.PICC_IsNewCardPresent()) {
        return;
    }

    // 2. Read serial UID from card
    if (!rfid.PICC_ReadCardSerial()) {
        return;
    }

    // 3. Print scanned UID for debugging / enrollment
    printUID(rfid.uid.uidByte, rfid.uid.size);

    // 4. Validate scanned UID against database
    if (verifyUID(rfid.uid.uidByte, rfid.uid.size)) {
        Serial.println(F(">> [ACCESS GRANTED]: Authorized user detected."));
        signalAccessGranted();

        // Open the barrier arm until the 90° limit switch triggers
        openBarrier();

        // Hold gate open for the configured dwell duration
        Serial.print(F(">> Gate holding open for "));
        Serial.print(GATE_HOLD_MS);
        Serial.println(F(" ms..."));
        delay(GATE_HOLD_MS);

        // Lower the barrier arm until the 0° limit switch triggers
        closeBarrier();
        setFeedbackIdle();
        Serial.println(F(">> Barrier cycle complete. Returning to IDLE standby."));
    } else {
        Serial.println(F(">> [ACCESS DENIED]: Unauthorized card scanned!"));
        signalAccessDenied();
        setFeedbackIdle();
    }

    // 5. Halt PICC & stop crypto encryption to prevent rapid duplicate scans
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    // Cooldown delay before accepting the next RFID tag
    delay(800);
}
