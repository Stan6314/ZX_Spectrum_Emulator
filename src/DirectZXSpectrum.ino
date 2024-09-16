/* DirectZXSpectrum is 8-bit computer emulator based on the FabGL library
 * -> see http://www.fabglib.org/ or FabGL on GitHub.
 *  
 * For proper operation, an ESP32 module with a VGA monitor 
 * and a PS2 keyboard connected according to the 
 * diagram on the website www.fabglib.org is required.
 * 
 * Cassette recorder is emulated using SPIFFS. The tape is represented 
 * by a file "filename.SNA" uploaded by ESP32 Filesystem Uploader in Arduino IDE. 
 * Only 49179 bytes long SNA file is accepted.
 * 
 * DirectZXSpectrum is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or any later version.
 * DirectZXSpectrum is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY.
 * Stan Pechal, 2024
 * Version 1.0
*/
#include "fabgl.h"
#include "emudevs/Z80.h"          // For processor
#include "devdrivers/MCP23S17.h"    // For expansion ports
#include "SPIFFS.h"

fabgl::VGADirectController DisplayController;
fabgl::PS2Controller PS2Controller;

// Constants for video output
static constexpr int borderSize           = 54;
static constexpr int borderXSize          = 72;
static constexpr int scanlinesPerCallback = 2;  // screen height should be divisible by this value

static TaskHandle_t  mainTaskHandle;
void * auxPoint;  // For use in setup

// **************************************************************************************************
// Hardware emulated in the ZX Spectrum computer
// "Tape recorder" is emulated in SPIFFS -> SNA file with NAME.SNA, started up with command LOAD "NAME"
File file;
bool isTape = false;      // Is SPIFFS ready

// We will use processor Z80 from the library FabGL
fabgl::Z80 m_Z80;
// Variables for emulating keyboard connection
int keyboardIn[8];      // Value read from keyboard port on individual lines
// Variables for VGA and colors
bool blinkFlag = false;     // Flag for blinking on display (bit 7 of attribute)
uint8_t ColorTable[128];    // Table for quick color select - 30 positions must be filled in setup()
uint8_t darkbgcolor;        // Background color - must be filled in setup()
int width, height;          // Display size - must be filled in setup()

// MCP23S17 can connect Kempston joystick (if present)
fabgl::MCP23S17  mcp2317;
bool readyMCP2317 = false;

// RAM memory will be just Byte array
volatile uint8_t ZXram[65536];   // selected addresses are overwritten by ROM in read mode
// ROM memory is contained in the array "rom_Sinclair_48K[]"
#include "romSinclair48K.h"
// Auxiliary global variables
unsigned int actualPC;  // Actual PC value is needed for IN operation
int BlinkCnt=0;   // Counter for video pixel blink

// **************************************************************************************************
// Functions for communication on the bus
static int readByte(void * context, int address)              { if(address < 0x4000) return(rom_sinclair_48k[address]); else return(ZXram[address]); };
static void writeByte(void * context, int address, int value) { ZXram[address] = (uint8_t)value; };
static int readWord(void * context, int addr)                 { return readByte(context, addr) | (readByte(context, addr + 1) << 8); };
static void writeWord(void * context, int addr, int value)    { writeByte(context, addr, value & 0xFF); writeByte(context, addr + 1, value >> 8); } ;
static int readIO(void * context, int address)
{
  // *** Keyboard inputs ULA has only 1 address
  if(address == 0xFE) {
    // This is work around of Lin Ke-Fong emulator imperfection (I/O in emulator has only byte address)
    // according to the instruction IN A,(n) ** 0xDB ** or IN r,(C) ** 0xED ** is content of register A or B simulated on higher address bus lines
    uint8_t adrKey, keyOut = 0x3F; // High byte of address and output value I/O read 
    if(((actualPC < 0x4000) && (rom_sinclair_48k[actualPC] == 0xDB)) || ((actualPC > 0x3FFF) && (ZXram[actualPC] == 0xDB))) adrKey = m_Z80.readRegByte(Z80_A);
      else adrKey = m_Z80.readRegByte(Z80_B);
    // Some games test more keys at one IN instruction, so put together keyboard status
    if(!(adrKey & 0x01)) keyOut &= keyboardIn[0];
    if(!(adrKey & 0x02)) keyOut &= keyboardIn[1];
    if(!(adrKey & 0x04)) keyOut &= keyboardIn[2];
    if(!(adrKey & 0x08)) keyOut &= keyboardIn[3];
    if(!(adrKey & 0x10)) keyOut &= keyboardIn[4];
    if(!(adrKey & 0x20)) keyOut &= keyboardIn[5];
    if(!(adrKey & 0x40)) keyOut &= keyboardIn[6];
    if(!(adrKey & 0x80)) keyOut &= keyboardIn[7];
    return keyOut;
  }
  // *** Kempston joystick on PORTA of MCP23S17
  if(address == 0x1F) {
    if(readyMCP2317) return mcp2317.readPort(MCP_PORTA);
  }
  return 0xFF; 
};
static void writeIO(void * context, int address, int value)
{
  if(address == 0xFE) {
    if(value & 0x10) digitalWrite(25, HIGH); else digitalWrite(25, LOW);      // Audio output is very simple :-) ... so it's not perfect
  }
};

// **************************************************************************************************
// Keyboard interface for selected keys
// Handles Key Up following keys:
void procesKeyUp(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardIn[4] |= 0x01; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardIn[3] |= 0x01; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardIn[3] |= 0x02; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardIn[3] |= 0x04; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardIn[3] |= 0x08; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardIn[3] |= 0x10; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardIn[4] |= 0x10; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardIn[4] |= 0x08; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardIn[4] |= 0x04; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardIn[4] |= 0x02; break;  // 9

      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[2] |= 0x01; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardIn[2] |= 0x02; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardIn[2] |= 0x04; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardIn[2] |= 0x08; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardIn[2] |= 0x10; break;  // t-T
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardIn[5] |= 0x10; break;  // y-Y
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardIn[5] |= 0x08; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardIn[5] |= 0x04; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardIn[5] |= 0x02; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardIn[5] |= 0x01; break;  // p-P

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardIn[1] |= 0x01; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[1] |= 0x02; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardIn[1] |= 0x04; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardIn[1] |= 0x08; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardIn[1] |= 0x10; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardIn[6] |= 0x10; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardIn[6] |= 0x08; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardIn[6] |= 0x04; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[6] |= 0x02; break;  // l-L

      case VirtualKey::VK_SPACE: keyboardIn[7] |= 0x01; break;  // space
      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardIn[0] |= 0x02; break;  // z-Z
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[0] |= 0x04; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardIn[0] |= 0x08; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardIn[0] |= 0x10; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[7] |= 0x10; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardIn[7] |= 0x08; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[7] |= 0x04; break;  // m-M
      case VirtualKey::VK_LCTRL:
      case VirtualKey::VK_RCTRL: keyboardIn[7] |= 0x02; break; break;  // Ctrl
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[6] |= 0x01; break;  // R Enter

      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: keyboardIn[0] |= 0x01;  // L and R shift
      default: break;
      }
};

// Handles Key Down following keys:
void procesKeyDown(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_ESCAPE: m_Z80.reset(); break;  // ESC will reset computer

      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardIn[4] &= 0xFE; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardIn[3] &= 0xFE; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardIn[3] &= 0xFD; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardIn[3] &= 0xFB; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardIn[3] &= 0xF7; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardIn[3] &= 0xEF; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardIn[4] &= 0xEF; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardIn[4] &= 0xF7; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardIn[4] &= 0xFB; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardIn[4] &= 0xFD; break;  // 9

      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[2] &= 0xFE; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardIn[2] &= 0xFD; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardIn[2] &= 0xFB; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardIn[2] &= 0xF7; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardIn[2] &= 0xEF; break;  // t-T
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardIn[5] &= 0xEF; break;  // y-Y
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardIn[5] &= 0xF7; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardIn[5] &= 0xFB; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardIn[5] &= 0xFD; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardIn[5] &= 0xFE; break;  // p-P

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardIn[1] &= 0xFE; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[1] &= 0xFD; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardIn[1] &= 0xFB; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardIn[1] &= 0xF7; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardIn[1] &= 0xEF; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardIn[6] &= 0xEF; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardIn[6] &= 0xF7; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardIn[6] &= 0xFB; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[6] &= 0xFD; break;  // l-L

      case VirtualKey::VK_SPACE: keyboardIn[7] &= 0xFE; break;  // space
      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardIn[0] &= 0xFD; break;  // z-Z
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[0] &= 0xFB; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardIn[0] &= 0xF7; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardIn[0] &= 0xEF; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[7] &= 0xEF; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardIn[7] &= 0xF7; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[7] &= 0xFB; break;  // m-M
      case VirtualKey::VK_LCTRL:
      case VirtualKey::VK_RCTRL: keyboardIn[7] &= 0xFD; break;  // Ctrl
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[6] &= 0xFE; break;  // R Enter

      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: keyboardIn[0] &= 0xFE; break;  // L and R shift
      default: break;
      }
};

// **************************************************************************************************
// VGA main function - prepare lines for displaying
void IRAM_ATTR drawScanline(void * arg, uint8_t * dest, int scanLine)
{
  // draws "scanlinesPerCallback" scanlines every time drawScanline() is called
  for (int i = 0; i < scanlinesPerCallback; ++i) {
    // fill border with background color
    memset(dest, darkbgcolor, width);
    if (!((scanLine < borderSize) || (scanLine >= (192+borderSize)))) {   // ZX Spectrum display is 192 rows height
      int auxPointer = (scanLine - borderSize) << 2;
      int dispPointer = (auxPointer & 0x00E0);      // bits Y5, Y4, Y3
      auxPointer = auxPointer << 3;
      dispPointer |= (auxPointer & 0x1800);      // bits Y7, Y6
      auxPointer = auxPointer << 3;
      dispPointer |= (0x4000 | (auxPointer & 0x0700));      // bits Y2, Y1, Y0
      int attrPointer = (((scanLine - borderSize) >> 3) *32) | 0x5800;
      for (int i = 0; i < 32; i++)  // 32 bytes must be transformed to 256 pixels on row
        {
          unsigned char videobyte = ZXram[dispPointer];  // Row Video RAM start address
          if ((ZXram[attrPointer] & 0x80) && blinkFlag) {   // Blink flag will exchange colors
            uint8_t shiftr = 0x80;
            for(int j=0; j<8; j++) {      // Set pixels on screen according this byte 
              if(videobyte & shiftr) VGA_PIXELINROW(dest, i*8+j+borderXSize) = ColorTable[ZXram[attrPointer] & 0x78];
              else VGA_PIXELINROW(dest, i*8+j+borderXSize) = ColorTable[ZXram[attrPointer] & 0x47];
              shiftr = shiftr >>1;
              }
          } else {
            uint8_t shiftr = 0x80;
            for(int j=0; j<8; j++) {
              if(videobyte & shiftr) VGA_PIXELINROW(dest, i*8+j+borderXSize) = ColorTable[ZXram[attrPointer] & 0x47];
              else VGA_PIXELINROW(dest, i*8+j+borderXSize) = ColorTable[ZXram[attrPointer] & 0x78];
              shiftr = shiftr >>1;
              }
           }
          dispPointer++; attrPointer++;       // Increment pointers to byte and attribute
        }
    }
    // go to next scanline
    ++scanLine;
    dest += width;
  }
  if (scanLine == height) {
    // signal end of screen
    vTaskNotifyGiveFromISR(mainTaskHandle, NULL);
  }
}

// **************************************************************************************************
// Load file from SPIFFS instead of cassete recorder
bool loadFile()
{
  uint8_t regHeader[27];    // Buffer for registers at file beginning
  
  // Create file name at first
  String fileOpened = "/";
  unsigned int namePoint = m_Z80.readRegWord(Z80_IX)-16;      // Name of loaded file starts here
  if((ZXram[namePoint] == 0xFF) || (ZXram[namePoint] == 0x20)) return false;  // Empty name is not allowed
  unsigned int k=0;  // counter for parsing file name in ZX memory
  do{
    fileOpened += (char)ZXram[namePoint+k]; k++;      // Parse file name
  } while((ZXram[namePoint+k] != 0x20) && (k<10));    // 10 characters max and ' ' is stop
  fileOpened += ".SNA";       // Add suffix
  if(isTape) {      // "Tape" must be ready
    file = SPIFFS.open(fileOpened, FILE_READ);    // Try to open file
    if(file) {      // File with such name exists
      if(file.size()==(unsigned long)49179) { // Only file with proper lenght will be processed
        for(int m=0; m<27; m++) regHeader[m] = file.read();   // Get register header at file beginning
        // Alternate registers, I,R and IM are not visible (private), so we prepare small program in ZX memory
        m_Z80.setPC(0x5800);       // Program will start just behind video
        ZXram[0x5800] = 0xED; ZXram[0x5801] = 0x47;   // LD I,A
        ZXram[0x5802] = 0xD9; ZXram[0x5803] = 0x08;   // EX BC-HL, EX AF
        ZXram[0x5804] = 0xED; ZXram[0x5805] = 0x4F;   // LD R,A
        ZXram[0x5806] = 0xED; ZXram[0x5807] = 0x46;   // Set INT mode 0 or
        if(regHeader[0x19]==1) ZXram[0x5807] = 0x56;   // Set INT mode 1 or
        if(regHeader[0x19]==2) ZXram[0x5807] = 0x5E;   // Set INT mode 2
        if(regHeader[0x13]&0x04) ZXram[0x5808] = 0xFB; else ZXram[0x5808] = 0xF3;  // DI or EI
        // Start fill registers
        m_Z80.writeRegByte(Z80_A, regHeader[0x0]); m_Z80.step();  // I filled
        m_Z80.writeRegByte(Z80_L, regHeader[0x1]); m_Z80.writeRegByte(Z80_H, regHeader[0x2]);
        m_Z80.writeRegByte(Z80_E, regHeader[0x3]); m_Z80.writeRegByte(Z80_D, regHeader[0x4]);
        m_Z80.writeRegByte(Z80_C, regHeader[0x5]); m_Z80.writeRegByte(Z80_B, regHeader[0x6]);
        m_Z80.writeRegByte(Z80_F, regHeader[0x7]); m_Z80.writeRegByte(Z80_A, regHeader[0x8]);  
        m_Z80.step();  m_Z80.step();  // Alternate regs filled with help of EX BC-HL, EX AF
        m_Z80.writeRegByte(Z80_L, regHeader[0x9]); m_Z80.writeRegByte(Z80_H, regHeader[0xA]);
        m_Z80.writeRegByte(Z80_E, regHeader[0xB]); m_Z80.writeRegByte(Z80_D, regHeader[0xC]);
        m_Z80.writeRegByte(Z80_C, regHeader[0xD]); m_Z80.writeRegByte(Z80_B, regHeader[0xE]);   // BC-HL filled
        m_Z80.writeRegWord(Z80_IY, (uint16_t)regHeader[0xF] + ((uint16_t)regHeader[0x10] << 8));   // IY filled
        m_Z80.writeRegWord(Z80_IX, (uint16_t)regHeader[0x11] + ((uint16_t)regHeader[0x12] << 8));   // IX filled
        m_Z80.writeRegByte(Z80_A, regHeader[0x14]); m_Z80.step(); m_Z80.step(); // R filled and set IM
        m_Z80.writeRegByte(Z80_F, regHeader[0x15]); m_Z80.writeRegByte(Z80_A, regHeader[0x16]);  // AF filled
        m_Z80.writeRegWord(Z80_SP, (uint16_t)regHeader[0x17] + ((uint16_t)regHeader[0x18] << 8));   // SP filled
        m_Z80.step(); // DI/EI
        // Registers almost ready, so fill memory
        for(unsigned int n=0x4000; n<0x10000; n++) ZXram[n] = file.read();   // Fill memory
        // And restore PC and SP from memory content
        m_Z80.setPC((uint16_t)ZXram[m_Z80.readRegWord(Z80_SP)] + ((uint16_t)ZXram[m_Z80.readRegWord(Z80_SP)+1] << 8)); // ... restore PC at first
        ZXram[m_Z80.readRegWord(Z80_SP)] = 0x0; ZXram[m_Z80.readRegWord(Z80_SP) + 1] = 0x0; // ... clear place of temporary PC store
        m_Z80.writeRegWord(Z80_SP, m_Z80.readRegWord(Z80_SP) + 2);       // ... and POP stack that contained the PC
        file.close();
        return true;
      } else { file.close(); return false; }
    } else return false;
  } else return false;
}

// **************************************************************************************************
void setup()
{
  mainTaskHandle = xTaskGetCurrentTaskHandle();

  // Audio output
  pinMode(25, OUTPUT);    // Audio output
  pinMode(34, OUTPUT);    // Auxiliary output

  // Start SPIFFS emulating tape recorder
  if(SPIFFS.begin(true)) isTape = true;
  // Expansion ports
  if(mcp2317.begin()) { 
    readyMCP2317 = true;
    mcp2317.enablePortPullUp(MCP_PORTA,0xFF); // Pull-up on inputs
    mcp2317.setPortInputPolarity(MCP_PORTA,0xFF); // Kempston joystick interface inverts
    mcp2317.enablePortPullUp(MCP_PORTB,0xFF);
  }

  // Set VGA for display monitor
  DisplayController.begin();
  DisplayController.setScanlinesPerCallBack(scanlinesPerCallback);
  DisplayController.setDrawScanlineCallback(drawScanline);
  DisplayController.setResolution(VGA_400x300_60Hz);
  width  = DisplayController.getScreenWidth();
  height = DisplayController.getScreenHeight();
  // Creating table of colors for VGA - table is 128 byte long, but only special positions are needed
  darkbgcolor = DisplayController.createRawPixel(RGB222(1, 1, 1)); // grey for background
  ColorTable[0] = ColorTable[64] = DisplayController.createRawPixel(RGB222(0, 0, 0)); // black
  ColorTable[1] = ColorTable[8] = DisplayController.createRawPixel(RGB222(0, 0, 2)); // blue
  ColorTable[2] = ColorTable[16] = DisplayController.createRawPixel(RGB222(2, 0, 0)); // red
  ColorTable[3] = ColorTable[24] = DisplayController.createRawPixel(RGB222(2, 0, 2)); // magenta
  ColorTable[4] = ColorTable[32] = DisplayController.createRawPixel(RGB222(0, 2, 0)); // green
  ColorTable[5] = ColorTable[40] = DisplayController.createRawPixel(RGB222(0, 2, 2)); // cyan
  ColorTable[6] = ColorTable[48] = DisplayController.createRawPixel(RGB222(2, 2, 0)); // yellow
  ColorTable[7] = ColorTable[56] = DisplayController.createRawPixel(RGB222(2, 2, 2)); // white
  ColorTable[65] = ColorTable[72] = DisplayController.createRawPixel(RGB222(0, 0, 3)); // bright blue
  ColorTable[66] = ColorTable[80] = DisplayController.createRawPixel(RGB222(3, 0, 0)); // bright red
  ColorTable[67] = ColorTable[88] = DisplayController.createRawPixel(RGB222(3, 0, 3)); // bright magenta
  ColorTable[68] = ColorTable[96] = DisplayController.createRawPixel(RGB222(0, 3, 0)); // bright green
  ColorTable[69] = ColorTable[104] = DisplayController.createRawPixel(RGB222(0, 3, 3)); // bright cyan
  ColorTable[70] = ColorTable[112] = DisplayController.createRawPixel(RGB222(3, 3, 0)); // bright yellow
  ColorTable[71] = ColorTable[120] = DisplayController.createRawPixel(RGB222(3, 3, 3)); // bright white

  // Set CPU bus functions and start it
  m_Z80.setCallbacks(auxPoint, readByte, writeByte, readWord, writeWord, readIO, writeIO); 
  m_Z80.reset();
  for (int i = 0; i < 8; i++) keyboardIn[i]=0xFF;

  // Set function pro Keyboard processing
  PS2Controller.begin(PS2Preset::KeyboardPort0, KbdMode::GenerateVirtualKeys);
  PS2Controller.keyboard()->onVirtualKey = [&](VirtualKey * vk, bool keyDown) {
      if (keyDown) {
        procesKeyDown(*vk);
    } else procesKeyUp(*vk);
  };
}


// **************************************************************************************************
// **************************************************************************************************

void loop()
{
  static int numCycles;
  numCycles = 0;
  while(numCycles < 55000) {  // approx. 55000 cycles per 16.6 milisec (60 Hz VGA)
    digitalWrite(34, HIGH); digitalWrite(34, LOW); // delay for slower audio
    numCycles += m_Z80.step();
    actualPC = m_Z80.getPC();     // Get PC value for input operation on the bus (see readIO() )
    if(actualPC == 0x0556) {      // instead of the LD-BYTES procedure in ROM, the filling of the memory with the SNA file is started
      if(!loadFile()) { m_Z80.writeRegWord(Z80_SP, (m_Z80.readRegWord(Z80_SP)+2));  m_Z80.setPC(0x0806); }    // Error on tape simulated (POP stack and call error)
      }
    }
  if(BlinkCnt % 6) m_Z80.IRQ(0x0FF);  // Trim interrupt from 60 Hz to 50 Hz (every 6th is omitted)
  if(BlinkCnt > 23) { BlinkCnt=0; blinkFlag = !blinkFlag; }     // Blink flag manipulation
  BlinkCnt++;

  // wait for vertical sync
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
