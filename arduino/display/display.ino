#include <SPI.h>
#include <DMD2.h>
#include <fonts/SystemFont5x7.h>

// Drives one P10 panel: either the score or the countdown clock. The server
// sends each line of text to show (newline terminated) and this sketch just
// draws it, so changing what's shown never needs a reflash.

// `make flash-clock` defines DISPLAY_CLOCK. In the Arduino IDE, uncomment this
// line before flashing the clock board.
// #define DISPLAY_CLOCK

#ifdef DISPLAY_CLOCK
#define DISPLAY_ROLE "CLOCK"
#define DEFAULT_TEXT "00.00"
#else
#define DISPLAY_ROLE "SCORE"
#define DEFAULT_TEXT "00000"
#endif

// Pin configurations for P10 display
#define PIN_DMD_nOE 10  // Output Enable (active low)
#define PIN_DMD_A 6     // Address A
#ifdef DISPLAY_CLOCK
#define PIN_DMD_B 9     // Address B (the clock board is wired to 9, not 7)
#else
#define PIN_DMD_B 7     // Address B
#endif
#define PIN_DMD_CLK 13  // Clock (SCK)
#define PIN_DMD_SCLK 8  // Scan Clock
#define PIN_DMD_DATA 11 // Data (MOSI)

// Define display parameters
#define DISPLAY_WIDTH 1  // Number of panels wide
#define DISPLAY_HEIGHT 1 // Number of panels high

// The server identifies every board by the HELLO line it prints at 115200 baud
// on boot. Text is then received at 9600 baud, the rate these boards have
// always run at alongside the display's scan interrupt.
#define HELLO_BAUDRATE 115200
#define BAUDRATE 9600

#define MAX_TEXT_LEN 16

// Initialize DMD object
SoftDMD dmd(DISPLAY_WIDTH, DISPLAY_HEIGHT, PIN_DMD_nOE, PIN_DMD_A, PIN_DMD_B, PIN_DMD_SCLK, PIN_DMD_CLK, PIN_DMD_DATA);

char incomingText[MAX_TEXT_LEN + 1];
uint8_t incomingLen = 0;
uint8_t shownLen = 0;

void showText(const char *text)
{
  // drawString also draws each character's background, so text of the same
  // length can be drawn straight over the old text without flicker. Clear
  // first only when the length changes so no stale characters are left.
  uint8_t len = strlen(text);
  if (len != shownLen)
  {
    dmd.clearScreen();
    shownLen = len;
  }
  dmd.drawString(1, 4, text);
}

void setup()
{
  Serial.begin(HELLO_BAUDRATE);
  Serial.println(F("HELLO " DISPLAY_ROLE));
  Serial.flush();
  Serial.begin(BAUDRATE);

  dmd.setBrightness(255);
  dmd.selectFont(System5x7);
  dmd.begin();
  showText(DEFAULT_TEXT);
}

void loop()
{
  while (Serial.available() > 0)
  {
    char c = Serial.read();
    if (c == '\n')
    {
      incomingText[incomingLen] = '\0';
      showText(incomingText);
      incomingLen = 0;
    }
    else if (c != '\r' && incomingLen < MAX_TEXT_LEN)
    {
      incomingText[incomingLen++] = c;
    }
  }
}
