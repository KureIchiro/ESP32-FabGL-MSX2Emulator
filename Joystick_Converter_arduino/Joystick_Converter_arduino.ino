
struct ControllerPins {
  uint8_t up;
  uint8_t down;
  uint8_t left;
  uint8_t right;
  uint8_t buttons[3];
  uint8_t turbo[3];
  uint8_t id;
};

const ControllerPins controllers[] = {
  {22, 23, 24, 25, {26, 27, 28}, {29, 30, 31}, 0},
  {32, 33, 34, 35, {36, 37, 38}, {39, 40, 41}, 1}
};

const uint8_t PIN_BUTTON4 = 42;
const uint8_t PIN_TURBO4 = 43;
const uint8_t PIN_MODE = 44;

const uint32_t SERIAL_INTERVAL_MS = 12;
const uint32_t TURBO_INTERVAL_MS = 40;
const uint32_t MODE_HOLD_MS = 1000;
const uint32_t LED_INTERVAL_2P_MS = 1000;
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

uint8_t readController(const ControllerPins &pins) {
  uint8_t data = 0xFF;

  if (digitalRead(pins.up) == LOW) {
    data &= ~(1U << 0);
  }
  if (digitalRead(pins.down) == LOW) {
    data &= ~(1U << 1);
  }
  if (digitalRead(pins.left) == LOW) {
    data &= ~(1U << 2);
  }
  if (digitalRead(pins.right) == LOW) {
    data &= ~(1U << 3);
  }

  for (uint8_t button = 0; button < 3; ++button) {
    const bool pressed = digitalRead(pins.buttons[button]) == LOW;
    const bool turboEnabled = digitalRead(pins.turbo[button]) == LOW;
    const bool pulseOn = (button == 1) ? !turboState : turboState;

    if (pressed && (!turboEnabled || pulseOn)) {
      data &= ~(1U << (button + 4));
    }
  }

  if (fourButtonMode) {
    const bool pressed = digitalRead(PIN_BUTTON4) == LOW;
    const bool turboEnabled = digitalRead(PIN_TURBO4) == LOW;

    if (pressed && (!turboEnabled || !turboState)) {
      data &= ~(1U << 7);
    }
  } else if (pins.id != 0) {
    data |= 1U << 7;
  } else {
    data &= ~(1U << 7);
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
  const uint32_t interval = fourButtonMode ? LED_INTERVAL_4BUTTON_MS : LED_INTERVAL_2P_MS;

  if (now - lastLedTick >= interval) {
    setLed(!ledOn, now);
  }
}

void setup() {
  for (const ControllerPins &pins : controllers) {
    pinMode(pins.up, INPUT_PULLUP);
    pinMode(pins.down, INPUT_PULLUP);
    pinMode(pins.left, INPUT_PULLUP);
    pinMode(pins.right, INPUT_PULLUP);

    for (uint8_t button = 0; button < 3; ++button) {
      pinMode(pins.buttons[button], INPUT_PULLUP);
      pinMode(pins.turbo[button], INPUT_PULLUP);
    }
  }
  pinMode(PIN_BUTTON4, INPUT_PULLUP);
  pinMode(PIN_TURBO4, INPUT_PULLUP);
  pinMode(PIN_MODE, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  setLed(true, millis());

  Serial1.begin(115200);
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
    if (fourButtonMode) {
      Serial1.write(readController(controllers[0]));
    } else {
      const uint8_t frame[] = {
        readController(controllers[0]),
        readController(controllers[1])
      };
      Serial1.write(frame, sizeof(frame));
    }
    lastSerialTick = now;
  }
}
