#include <U8x8lib.h>

// Hardware I2C OLED configuration (SSD1306 128x64)
U8X8_SSD1306_128X64_NONAME_HW_I2C u8x8(/* reset=*/ U8X8_PIN_NONE);

// Define terminal window size (16 chars wide x 8 lines high)
#define U8LOG_WIDTH 16
#define U8LOG_HEIGHT 8

uint8_t u8log_buffer[U8LOG_WIDTH * U8LOG_HEIGHT];
U8X8LOG u8x8log;

// Test dataset variables
char *c_str = "1. C-Style Str";
String arduino_str = "2. Arduino String";
int int_val = 2026;
float float_val = 36.5432;
uint8_t hex_val = 0xFE;
int loop_count = 1;

void setup() {
  u8x8.begin();
  delay(1000);
  u8x8.setFont(u8x8_font_chroma48medium8_r);

  // Initialize U8X8LOG terminal buffer
  u8x8log.begin(u8x8, U8LOG_WIDTH, U8LOG_HEIGHT, u8log_buffer);
  
  // Redraw Mode: 0 = Update screen only on newline ('\n'), 1 = Update screen per character
  u8x8log.setRedrawMode(1);

  u8x8log.print("=== CLI DEMO ===\n");
  u8x8log.print("Starting Test...\n");
  delay(2000);
}

void loop() {
  // 1. Clear screen using Form Feed ('\f') & display loop iteration count
  u8x8log.print("\f"); 
  u8x8log.print("--- Loop #");
  u8x8log.print(loop_count++);
  u8x8log.print(" ---\n");
  delay(1000);

  // 2. C-Style String (char pointer)
  u8x8log.println(c_str);
  delay(800);

  // 3. Arduino String Object
  u8x8log.println(arduino_str);
  delay(800);

  // 4. Integer (int) and Floating-point (float with 2 decimal precision)
  u8x8log.print("Int: ");
  u8x8log.println(int_val);
  
  u8x8log.print("Float: ");
  u8x8log.println(float_val, 2); // Output: "36.54"
  delay(1000);

  // 5. Hexadecimal (HEX) and Binary (BIN) format representations
  u8x8log.print("HEX: 0x");
  u8x8log.println(hex_val, HEX);  // Output: "FE"

  u8x8log.print("BIN: ");
  u8x8log.println(hex_val, BIN);  // Output: "11111110"
  delay(1000);

  // 6. Direct byte/ASCII output using write()
  u8x8log.print("Write: ");
  u8x8log.write('O');
  u8x8log.write('K');
  u8x8log.write('!');
  u8x8log.write('\n'); // Manual newline
  
  // Delay 3 seconds before starting the next loop iteration
  delay(3000);
}
