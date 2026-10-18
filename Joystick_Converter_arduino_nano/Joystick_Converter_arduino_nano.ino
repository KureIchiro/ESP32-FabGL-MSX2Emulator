// Arduino Nano (ATmega328P) Atari-style joystick -> ESP32 UART converter
// 1 controller only version

const uint8_t PIN_UP = 2;
const uint8_t PIN_DOWN = 3;
const uint8_t PIN_LEFT = 4;
const uint8_t PIN_RIGHT = 5;

const uint8_t PIN_BUTTONS[3] = {6, 7, 8};
const uint8_t PIN_TURBO[3] = {9, 10, 11};

const uint32_t SERIAL_INTERVAL_MS = 12;
const uint32_t TURBO_INTERVAL_MS = 40;

uint32_t lastSerialTick = 0;
uint32_t lastTurboTick = 0;
bool turboState = false;

uint8_t readController() {
  // Active low: ON=0, OFF=1. b7 (ID) is always 0 (Controller 1).
  uint8_t data = 0x7F;

  if (digitalRead(PIN_UP) == LOW) {
    data &= ~(1U << 0);
  }
  if (digitalRead(PIN_DOWN) == LOW) {
    data &= ~(1U << 1);
  }
  if (digitalRead(PIN_LEFT) == LOW) {
    data &= ~(1U << 2);
  }
  if (digitalRead(PIN_RIGHT) == LOW) {
    data &= ~(1U << 3);
  }

  for (uint8_t button = 0; button < 3; ++button) {
    const bool pressed = digitalRead(PIN_BUTTONS[button]) == LOW;
    const bool rapidFire = digitalRead(PIN_TURBO[button]) == LOW;
    // Button 2 toggles in the opposite phase (compatible with the STM32 version).
    const bool pulseOn = (button == 1) ? !turboState : turboState;

    if (pressed && (!rapidFire || pulseOn)) {
      data &= ~(1U << (button + 4));
    }
  }

  return data;
}

void setup() {
  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DOWN, INPUT_PULLUP);
  pinMode(PIN_LEFT, INPUT_PULLUP);
  pinMode(PIN_RIGHT, INPUT_PULLUP);

  for (uint8_t button = 0; button < 3; ++button) {
    pinMode(PIN_BUTTONS[button], INPUT_PULLUP);
    pinMode(PIN_TURBO[button], INPUT_PULLUP);
  }

  Serial.begin(115200);
}

void loop() {
  const uint32_t now = millis();

  if (now - lastTurboTick >= TURBO_INTERVAL_MS) {
    turboState = !turboState;
    lastTurboTick = now;
  }

  if (now - lastSerialTick >= SERIAL_INTERVAL_MS) {
    Serial.write(readController());
    lastSerialTick = now;
  }
}
