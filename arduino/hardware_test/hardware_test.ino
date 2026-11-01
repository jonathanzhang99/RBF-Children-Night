#define FASTLED_ALLOW_INTERRUPTS 0

#include <FastLED.h>
#include <Arduino.h>

// Checks the game board's wiring without any game logic. Lights each LED strip
// in turn (then all of them), and every second prints whether each IR receiver
// pin looks idle, stuck or noisy. Shoot a target to see its pin pick up hits.

// Same pins as game.ino
#define LED_TYPE WS2812
#define COLOR_ORDER GRB
#define NUM_LEDS 16
#define LED_DATA_PIN_1 6
#define LED_DATA_PIN_2 7
#define LED_DATA_PIN_3 8
#define LED_DATA_PIN_4 9
// Kept low so the test works even with the LEDs running off USB power
#define BRIGHTNESS 32

const uint8_t ledPins[] = {LED_DATA_PIN_1, LED_DATA_PIN_2, LED_DATA_PIN_3, LED_DATA_PIN_4};
const uint8_t irPins[] = {2, 3, 4, 5};
const uint8_t numTargets = 4;
const CRGB colors[] = {CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::Yellow};

CRGB leds[numTargets][NUM_LEDS];

// Phases 0-3 light one strip each, phase 4 lights all of them
uint8_t phase = 0;
unsigned long phaseStart = 0;
unsigned long samples = 0;
unsigned long lowSamples[numTargets];

void showPhase()
{
  for (uint8_t i = 0; i < numTargets; i++)
  {
    CRGB color = (phase == i) ? colors[i] : (phase == numTargets) ? CRGB::Red
                                                                  : CRGB::Black;
    fill_solid(leds[i], NUM_LEDS, color);
  }
  FastLED.show();
}

void report()
{
  if (phase < numTargets)
  {
    Serial.print(F("LEDS: strip "));
    Serial.print(phase + 1);
    Serial.print(F(" (pin "));
    Serial.print(ledPins[phase]);
    Serial.print(F(") only"));
  }
  else
  {
    Serial.print(F("LEDS: all strips red"));
  }

  Serial.print(F(" | IR:"));
  for (uint8_t i = 0; i < numTargets; i++)
  {
    Serial.print(F(" pin "));
    Serial.print(irPins[i]);
    Serial.print(F("="));
    if (lowSamples[i] == 0)
    {
      Serial.print(F("idle"));
    }
    else if (lowSamples[i] == samples)
    {
      Serial.print(F("STUCK_LOW"));
    }
    else
    {
      Serial.print(lowSamples[i] * 100 / samples);
      Serial.print(F("%_low"));
    }
  }
  Serial.println();
}

void setup()
{
  Serial.begin(115200);
  Serial.println(F("HARDWARE TEST"));

  FastLED.addLeds<LED_TYPE, LED_DATA_PIN_1, COLOR_ORDER>(leds[0], NUM_LEDS);
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN_2, COLOR_ORDER>(leds[1], NUM_LEDS);
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN_3, COLOR_ORDER>(leds[2], NUM_LEDS);
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN_4, COLOR_ORDER>(leds[3], NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);

  for (uint8_t i = 0; i < numTargets; i++)
  {
    pinMode(irPins[i], INPUT);
  }

  phaseStart = millis();
  showPhase();
}

void loop()
{
  samples++;
  for (uint8_t i = 0; i < numTargets; i++)
  {
    if (digitalRead(irPins[i]) == LOW)
    {
      lowSamples[i]++;
    }
  }

  if (millis() - phaseStart >= 1000)
  {
    report();
    samples = 0;
    memset(lowSamples, 0, sizeof(lowSamples));
    phase = (phase + 1) % (numTargets + 1);
    phaseStart = millis();
    showPhase();
  }
}
