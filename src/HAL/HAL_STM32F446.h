// Platform setup ------------------------------------------------------------------------------------

// We define a more generic symbol, in case more STM32 boards based on different lines are supported
#define __ARM_STM32__

#define HAL_FAST_PROCESSOR

#define HAL_MAXRATE_LOWER_LIMIT 14   // Lower limit (fastest) step rate in uS (in SQW mode) assumes optimization set to Fastest (-O3)
#define HAL_PULSE_WIDTH         500  // Width of step pulse

#include <HardwareTimer.h>

// Interrupts
#define cli() noInterrupts()
#define sei() interrupts()

// New symbols for the Serial ports so they can be remapped if necessary -----------------------------

// SerialA is manidatory
#define SERIAL_A Serial
#if SERIAL_B_BAUD_DEFAULT != OFF
  // SerialB is optional
  #define SERIAL_B Serial1
#endif
// SerialC is optional
#if SERIAL_C_BAUD_DEFAULT != OFF
  #define SERIAL_C Serial3
#endif

// Handle special case of using software serial for a GPS
#if SerialGPS == SoftwareSerial2
  #include <SoftwareSerial.h>
  SoftwareSerial SWSerialGPS(PA3, PA2); // RX2, TX2
  #undef SerialGPS
  #define SerialGPS SWSerialGPS
#endif

// New symbol for the default I2C port ---------------------------------------------------------------
#include <Wire.h>
#define HAL_Wire Wire
#define HAL_WIRE_CLOCK 100000

// Non-volatile storage ------------------------------------------------------------------------------
#undef E2END
#if defined(NV_MB85RC256V)
  #include "../drivers/NV_I2C_FRAM_MB85RC256V.h"
#else
  // The FYSETC S6 v2 has a 4096 byte EEPROM built-in
  #if PINMAP == FYSETC_S6_2
    #define E2END 4095
    #define I2C_EEPROM_ADDRESS 0x50
  #endif
  // The FYSETC S6 has a 2048 byte EEPROM built-in
  #if PINMAP == FYSETC_S6
    #define E2END 2047
    #define I2C_EEPROM_ADDRESS 0x50
  #endif
  // Defaults to 0x57 and 4KB 
  #include "../drivers/NV_I2C_EEPROM_24XX_C.h"
#endif

//----------------------------------------------------------------------------------------------------
// Nanoseconds delay function
unsigned int _nanosPerPass=1;
void delayNanoseconds(unsigned int n) {
  unsigned int np=(n/_nanosPerPass);
  for (unsigned int i=0; i<np; i++) { __asm__ volatile ("nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t"); }
}

// Allow MCU reset -----------------------------------------------------------------------------------
#define HAL_RESET NVIC_SystemReset()

//--------------------------------------------------------------------------------------------------
// General purpose initialize for HAL
void HAL_Initialize(void) {
  // calibrate delayNanoseconds()
  uint32_t startTime,npp;
  startTime=micros(); delayNanoseconds(65535); npp=micros(); npp=((int32_t)(npp-startTime)*1000)/63335;
  if (npp<1) npp=1; if (npp>2000) npp=2000; _nanosPerPass=npp;
  analogWriteResolution(8);
}

//--------------------------------------------------------------------------------------------------
// Internal MCU temperature (in degrees C)
float HAL_MCU_Temperature(void) {
  return -999;
}