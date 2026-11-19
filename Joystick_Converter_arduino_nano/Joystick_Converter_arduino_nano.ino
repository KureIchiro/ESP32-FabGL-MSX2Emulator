// Arduino Nano (ATmega328P) Atari-style joystick -> ESP32 UART converter
// 1 controller only version

const uint8_t PIN_UP = 2;
const uint8_t PIN_DOWN = 3;
const uint8_t PIN_LEFT = 4;
const uint8_t PIN_RIGHT = 5;

const uint8_t PIN_BUTTONS[3] = {6, 7, 8};
const uint8_t PIN_TURBO[3] = {9, 10, 11};

const uint8_t PIN_BUTTON4 = 12;
const uint8_t PIN_TURBO4 = A0;
const uint8_t PIN_MODE = A1;

const uint32_t SERIAL_INTERVAL_MS = 12;
const uint32_t TURBO_INTERVAL_MS = 40;
const uint32_t MODE_HOLD_MS = 1000;
const uint32_t LED_INTERVAL_3BUTTON_MS = 1000;
const uint32_t LED_INTERVAL_4BUTTON_MS = 500;

uint32_t lastSerialTick = 0;
uint32_t lastTurboTick = 0;
bool turboState = false;

bool fourButtonMode = false;
bool modePressed = false;
bool modeToggled = false;
uint32_t modePressTick = 0;
bool ledOn = false;
uint32_t lastLedTick = 0;

uint8_t readController() {
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

  if (fourButtonMode) {
    const bool pressed = digitalRead(PIN_BUTTON4) == LOW;
    const bool rapidFire = digitalRead(PIN_TURBO4) == LOW;

    if (!(pressed && (!rapidFire || !turboState))) {
      data |= 1U << 7;
    }
  }

  return data;
}

void setLed(bool on, uint32_t now) {
  ledOn = on;
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
  lastLedTick = now;
}

void updateMode(uint32_t now) {
  if (digitalRead(PIN_MODE) == LOW) {
    if (!modePressed) {
      modePressed = true;
      modeToggled = false;
      modePressTick = now;
    } else if (!modeToggled && now - modePressTick >= MODE_HOLD_MS) {
      fourButtonMode = !fourButtonMode;
      modeToggled = true;
      setLed(true, now);
    }
  } else {
    modePressed = false;
  }
}

void updateLed(uint32_t now) {
  const uint32_t interval = fourButtonMode ? LED_INTERVAL_4BUTTON_MS : LED_INTERVAL_3BUTTON_MS;

  if (now - lastLedTick >= interval) {
    setLed(!ledOn, now);
  }
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
  pinMode(PIN_BUTTON4, INPUT_PULLUP);
  pinMode(PIN_TURBO4, INPUT_PULLUP);
  pinMode(PIN_MODE, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  setLed(true, millis());

  Serial.begin(115200);
}

void loop() {
  const uint32_t now = millis();

  if (now - lastTurboTick >= TURBO_INTERVAL_MS) {
    turboState = !turboState;
    lastTurboTick = now;
  }

  updateMode(now);
  updateLed(now);

  if (now - lastSerialTick >= SERIAL_INTERVAL_MS) {
    Serial.write(readController());
    lastSerialTick = now;
  }
}
