#include <FirmataPlus86.h>

#define CONNECT_86DUINO_METHOD 1 // 0: USB Serial 1: BlueTooth 2: Arduino WiFi Shield 3: Ethernet 4: ESP8266 WiFi 5: ESP8266 AP

char* projectName = "Hello, 86Duino";
char* _ssid = "your_ssid";
char* _password = "your_password"; // If it is ESP8266 AP mode, the password can not be shorter that 8 chars.
#define BT_ESP8266_Serial         Serial1
#define BT_ESP8266_Serial_Baud    9600

#if CONNECT_86DUINO_METHOD == 2 // Arduino WiFi Shield
    bool _wep = false;
    IPAddress _ip(0, 0, 0, 0);
#elif CONNECT_86DUINO_METHOD == 3 // Ethernet
    #define ETHERNET_DHCP
    #ifndef ETHERNET_DHCP
        IPAddress localIP(192, 168, 4, 100);
        IPAddress subnet(255, 255, 240, 0);
        IPAddress dnsserver(192, 168, 1, 1);
        IPAddress gateway(192, 168, 1, 1);
    #endif
#elif CONNECT_86DUINO_METHOD == 4 || CONNECT_86DUINO_METHOD == 5 // ESP8266
    int ch_pd_pin = 10;
    uint8_t _chl = 11; // channel ID (only for ESP8266 AP mode)
    uint8_t _ecn = 4; // encryption method (only for ESP8266 AP mode)
#endif
//////////////////////////////////////////////////////////////////////////////////


#include <Servo86.h>
#if defined __86DUINO_ONE || defined __86DUINO_AI
    #include <FreeIMU1.h>
#endif
#include <Wire.h>
#include <NewPing.h>
#include <Stepper.h>
#include <Encoder.h>
#include <avr/wdt.h>


#define I2C_WRITE                   B00000000
#define I2C_READ                    B00001000
#define I2C_READ_CONTINUOUSLY       B00010000
#define I2C_STOP_READING            B00011000
#define I2C_READ_WRITE_MODE_MASK    B00011000
#define I2C_10BIT_ADDRESS_MODE_MASK B00100000
#define I2C_END_TX_MASK             B01000000
#define I2C_STOP_TX                 1
#define I2C_RESTART_TX              0
#define I2C_MAX_QUERIES             8
#define I2C_REGISTER_NOT_SPECIFIED  -1

// the minimum interval for sampling analog input
#define MINIMUM_SAMPLING_INTERVAL 1

#define REGISTER_NOT_SPECIFIED -1

#define ENCODER_NOT_PRESENT 0
#define ENCODER_IS_PRESENT  1

#define INTER_PING_INTERVAL 40 // 40 ms.

// SYSEX command sub specifiers
#define TONE_TONE 0
#define TONE_NO_TONE 1

#define STEPPER_CONFIGURE 0
#define STEPPER_STEP 1
#define STEPPER_LIBRARY_VERSION 2


/*==============================================================================
 * GLOBAL VARIABLES
 *============================================================================*/
bool imu_data_start_sampling = false;
bool enableIMUWaiting = false;
int imu_data_type = 0;

bool one_servo_is_moving = false;
int one_servo_used_pin;

bool checkActiveStart = false;

/* analog inputs */
int analogInputsToReport = 0; // bitwise array to store pin reporting

/* digital input ports */
byte reportPINs[TOTAL_PORTS];       // 1 = report this port, 0 = silence
byte previousPINs[TOTAL_PORTS];     // previous 8 bits sent

/* pins configuration */
byte pinConfig[TOTAL_PINS];         // configuration of every pin
byte portConfigInputs[TOTAL_PORTS]; // each bit: 1 = pin in INPUT, 0 = anything else
int pinState[TOTAL_PINS];           // any value that has been written

/* timer variables */
unsigned long currentMillis;        // store the current value from millis()
unsigned long previousMillis;       // for comparison with currentMillis
unsigned int samplingInterval = 19; // how often to run the main loop (in ms)
unsigned long previousKeepAliveMillis = 0;
unsigned int keepAliveInterval = 0;

/* i2c data */
struct i2c_device_info {
  byte addr;
  int reg;
  byte bytes;
  byte stopTX;
};

/* for i2c read continuous more */
i2c_device_info query[I2C_MAX_QUERIES];

byte i2cRxData[64];
boolean isI2CEnabled = false;
signed char queryIndex = -1;
// default delay time between i2c read request and Wire.requestFrom()
unsigned int i2cReadDelayTime = 0;

boolean isResetting = false;

// test for "w" command to waiting
boolean test_waiting_start = false;
int imu_waiting_id;
int waiting_time;
unsigned long start_waiting_time;
  
/* utility functions */
void wireWrite(byte data)
{
#if ARDUINO >= 100
  Wire.write((byte)data);
#else
  Wire.send(data);
#endif
}

byte wireRead(void)
{
#if ARDUINO >= 100
  return Wire.read();
#else
  return Wire.receive();
#endif
}


// Servos
Servo servos[MAX_SERVOS];
byte servoPinMap[TOTAL_PINS];
byte detachedServos[MAX_SERVOS];
byte detachedServoCount = 0;
byte servoCount = 0;

/* Rotary Encoder Support */
unsigned char encoderMSB = 0, encoderLSB = 0, encoderHLSB = 0, encoderHMSB = 0, encoderLastB = 0;     // sysex data registers
uint8_t encoderModule, encoderMode ; // user specified encoder pins
unsigned long encoderPostion = 0L;            // current position of encoder
int8_t clicks = 0 ;                // encoder click counter
boolean encoderPresent = false, pre_encoderPresent = false;   // encoder installed flag

// Ping variables

int numLoops = 0 ;
int pingLoopCounter = 0 ;

int numActiveSonars = 0 ; // number of sonars attached
uint8_t sonarPinNumbers[MAX_SONARS] ;
int nextSonar = 0 ; // index into sonars[] for next device

// array to hold up to 6 instances of sonar devices
NewPing *sonars[MAX_SONARS] ;

uint8_t sonarTriggerPin;
uint8_t sonarEchoPin ;
uint8_t currentSonar = 0;            // Keeps track of which sensor is active.

uint8_t pingInterval = 33 ;  // Milliseconds between sensor pings (29ms is about the min to avoid
// cross- sensor echo).
byte sonarMSB, sonarLSB ;


// Stepper Motor

Stepper *stepper = NULL;

#if defined __86DUINO_ONE || defined __86DUINO_AI
FreeIMU1 my3IMU = FreeIMU1();
#endif

#ifdef FIRMATA_SERIAL_FEATURE
SerialFirmata serialFeature;
#endif

/*==============================================================================
 * FUNCTIONS
 *============================================================================*/

void attachServo(byte pin, int minPulse, int maxPulse)
{
  if (servoCount < MAX_SERVOS) {
    // reuse indexes of detached servos until all have been reallocated
    if (detachedServoCount > 0) {
      servoPinMap[pin] = detachedServos[detachedServoCount - 1];
      if (detachedServoCount > 0) detachedServoCount--;
    } else {
      servoPinMap[pin] = servoCount;
      servoCount++;
    }
    if (minPulse > 0 && maxPulse > 0) {
      servos[servoPinMap[pin]].attach(PIN_TO_DIGITAL(pin), minPulse, maxPulse);
    } else {
      servos[servoPinMap[pin]].attach(PIN_TO_DIGITAL(pin));
    }
  } else {
    Firmata.sendString("Max servos attached");
  }
}

void detachServo(byte pin)
{
  servos[servoPinMap[pin]].detach();
  // if we're detaching the last servo, decrement the count
  // otherwise store the index of the detached servo
  if (servoPinMap[pin] == servoCount && servoCount > 0) {
    servoCount--;
  } else if (servoPinMap[pin] != 255 && servoCount > 0) {
    // keep track of detached servos because we want to reuse their indexes
    // before incrementing the count of attached servos
    detachedServoCount++;
    detachedServos[detachedServoCount - 1] = servoPinMap[pin];
  }

  servoPinMap[pin] = 255;
}

void readAndReportData(byte address, int theRegister, byte numBytes, byte stopTX) {
  // allow I2C requests that don't require a register read
  // for example, some devices using an interrupt pin to signify new data available
  // do not always require the register read so upon interrupt you call Wire.requestFrom()
  if (theRegister != I2C_REGISTER_NOT_SPECIFIED) {
    Wire.beginTransmission(address);
    wireWrite((byte)theRegister);
    Wire.endTransmission(stopTX); // default = true
    // do not set a value of 0
    if (i2cReadDelayTime > 0) {
      // delay is necessary for some devices such as WiiNunchuck
      delayMicroseconds(i2cReadDelayTime);
    }
  } else {
    theRegister = 0;  // fill the register with a dummy value
  }

  Wire.requestFrom(address, numBytes);  // all bytes are returned in requestFrom

  // check to be sure correct number of bytes were returned by slave
  if (numBytes < Wire.available()) {
    Firmata.sendString("I2C: Too many bytes received");
  } else if (numBytes > Wire.available()) {
    Firmata.sendString("I2C: Too few bytes received");
  }

  i2cRxData[0] = address;
  i2cRxData[1] = theRegister;

  for (int i = 0; i < numBytes && Wire.available(); i++) {
    i2cRxData[2 + i] = wireRead();
  }

  // send slave address, register and received bytes
  Firmata.sendSysex(SYSEX_I2C_REPLY, numBytes + 2, i2cRxData);
}

void outputPort(byte portNumber, byte portValue, byte forceSend)
{
  // pins not configured as INPUT are cleared to zeros
  portValue = portValue & portConfigInputs[portNumber];
  // only send if the value is different than previously sent
  if (forceSend || previousPINs[portNumber] != portValue) {
    Firmata.sendDigitalPort(portNumber, portValue);
    previousPINs[portNumber] = portValue;
  }
}

/* -----------------------------------------------------------------------------
 * check all the active digital inputs for change of state, then add any events
 * to the Serial output queue using Serial.print() */
void checkDigitalInputs(void)
{
  /* Using non-looping code allows constants to be given to readPort().
   * The compiler will apply substantial optimizations if the inputs
   * to readPort() are compile-time constants. */
  if (TOTAL_PORTS > 0 && reportPINs[0]) outputPort(0, readPort(0, portConfigInputs[0]), false);
  if (TOTAL_PORTS > 1 && reportPINs[1]) outputPort(1, readPort(1, portConfigInputs[1]), false);
  if (TOTAL_PORTS > 2 && reportPINs[2]) outputPort(2, readPort(2, portConfigInputs[2]), false);
  if (TOTAL_PORTS > 3 && reportPINs[3]) outputPort(3, readPort(3, portConfigInputs[3]), false);
  if (TOTAL_PORTS > 4 && reportPINs[4]) outputPort(4, readPort(4, portConfigInputs[4]), false);
  if (TOTAL_PORTS > 5 && reportPINs[5]) outputPort(5, readPort(5, portConfigInputs[5]), false);
  if (TOTAL_PORTS > 6 && reportPINs[6]) outputPort(6, readPort(6, portConfigInputs[6]), false);
  if (TOTAL_PORTS > 7 && reportPINs[7]) outputPort(7, readPort(7, portConfigInputs[7]), false);
  if (TOTAL_PORTS > 8 && reportPINs[8]) outputPort(8, readPort(8, portConfigInputs[8]), false);
  if (TOTAL_PORTS > 9 && reportPINs[9]) outputPort(9, readPort(9, portConfigInputs[9]), false);
  if (TOTAL_PORTS > 10 && reportPINs[10]) outputPort(10, readPort(10, portConfigInputs[10]), false);
  if (TOTAL_PORTS > 11 && reportPINs[11]) outputPort(11, readPort(11, portConfigInputs[11]), false);
  if (TOTAL_PORTS > 12 && reportPINs[12]) outputPort(12, readPort(12, portConfigInputs[12]), false);
  if (TOTAL_PORTS > 13 && reportPINs[13]) outputPort(13, readPort(13, portConfigInputs[13]), false);
  if (TOTAL_PORTS > 14 && reportPINs[14]) outputPort(14, readPort(14, portConfigInputs[14]), false);
  if (TOTAL_PORTS > 15 && reportPINs[15]) outputPort(15, readPort(15, portConfigInputs[15]), false);
}

// -----------------------------------------------------------------------------
/* sets the pin mode to the correct state and sets the relevant bits in the
 * two bit-arrays that track Digital I/O and PWM status
 */
void setPinModeCallback(byte pin, int mode)
{
  if (Firmata.getPinMode(pin) == PIN_MODE_IGNORE)
    return;

  if (Firmata.getPinMode(pin) == PIN_MODE_I2C && isI2CEnabled && mode != PIN_MODE_I2C) {
    // disable i2c so pins can be used for other functions
    // the following if statements should reconfigure the pins properly
    disableI2CPins();
  }
  if (IS_PIN_DIGITAL(pin) && mode != PIN_MODE_SERVO) {
    if (servoPinMap[pin] < MAX_SERVOS && servos[servoPinMap[pin]].attached()) {
      detachServo(pin);
    }
  }
  if (IS_PIN_ANALOG(pin)) {
    reportAnalogCallback(PIN_TO_ANALOG(pin), mode == PIN_MODE_ANALOG ? 1 : 0); // turn on/off reporting
  }
  if (IS_PIN_DIGITAL(pin)) {
    if (mode == INPUT || mode == PIN_MODE_PULLUP) {
      portConfigInputs[pin / 8] |= (1 << (pin & 7));
    } else {
      portConfigInputs[pin / 8] &= ~(1 << (pin & 7));
    }
  }
  Firmata.setPinState(pin, 0);
  switch (mode) {
    case PIN_MODE_ANALOG:
      if (IS_PIN_ANALOG(pin)) {
        if (IS_PIN_DIGITAL(pin)) {
          pinMode(PIN_TO_DIGITAL(pin), INPUT);    // disable output driver
#if ARDUINO <= 100
          // deprecated since Arduino 1.0.1 - TODO: drop support in Firmata 2.6
          digitalWrite(PIN_TO_DIGITAL(pin), LOW); // disable internal pull-ups
#endif
        }
        Firmata.setPinMode(pin, PIN_MODE_ANALOG);
      }
      break;
    case INPUT:
      if (IS_PIN_DIGITAL(pin)) {
        pinMode(PIN_TO_DIGITAL(pin), INPUT);    // disable output driver
#if ARDUINO <= 100
        // deprecated since Arduino 1.0.1 - TODO: drop support in Firmata 2.6
        digitalWrite(PIN_TO_DIGITAL(pin), LOW); // disable internal pull-ups
#endif
        Firmata.setPinMode(pin, INPUT);
      }
      break;
    case PIN_MODE_PULLUP:
      if (IS_PIN_DIGITAL(pin)) {
        pinMode(PIN_TO_DIGITAL(pin), INPUT_PULLUP);
        Firmata.setPinMode(pin, PIN_MODE_PULLUP);
        Firmata.setPinState(pin, 1);
      }
      break;
    case OUTPUT:
      if (IS_PIN_DIGITAL(pin)) {
        digitalWrite(PIN_TO_DIGITAL(pin), LOW); // disable PWM
        pinMode(PIN_TO_DIGITAL(pin), OUTPUT);
        Firmata.setPinMode(pin, OUTPUT);
      }
      break;
    case PIN_MODE_PWM:
      if (IS_PIN_PWM(pin)) {
        pinMode(PIN_TO_PWM(pin), OUTPUT);
        analogWrite(PIN_TO_PWM(pin), 0);
        Firmata.setPinMode(pin, PIN_MODE_PWM);
      }
      break;
    case PIN_MODE_SERVO:
      if (IS_PIN_DIGITAL(pin)) {
        Firmata.setPinMode(pin, PIN_MODE_SERVO);
        if (servoPinMap[pin] == 255 || !servos[servoPinMap[pin]].attached()) {
          // pass -1 for min and max pulse values to use default values set
          // by Servo library
          attachServo(pin, -1, -1);
        }
      }
      break;
    case PIN_MODE_I2C:
      if (IS_PIN_I2C(pin)) {
        // mark the pin as i2c
        // the user must call I2C_CONFIG to enable I2C for a device
        Firmata.setPinMode(pin, PIN_MODE_I2C);
      }
      break;
    case PIN_MODE_SERIAL:
#ifdef FIRMATA_SERIAL_FEATURE
      serialFeature.handlePinMode(pin, PIN_MODE_SERIAL);
#endif
      break;
    case PIN_MODE_TONE:
      Firmata.setPinMode(pin, PIN_MODE_TONE);
      //pinConfig[pin] = TONE ;
      break ;
    case PIN_MODE_SONAR:
      Firmata.setPinMode(pin, PIN_MODE_SONAR);
      //pinConfig[pin] = SONAR ;
      break ;
    case PIN_MODE_STEPPER:
      Firmata.setPinMode(pin, PIN_MODE_STEPPER);
      //pinConfig[pin] = STEPPER ;
      break ;
    default:
      Firmata.sendString("Unknown pin mode"); // TODO: put error msgs in EEPROM
      break ;
  }
  // TODO: save status to EEPROM here, if changed
}

/*
 * Sets the value of an individual pin. Useful if you want to set a pin value but
 * are not tracking the digital port state.
 * Can only be used on pins configured as OUTPUT.
 * Cannot be used to enable pull-ups on Digital INPUT pins.
 */
void setPinValueCallback(byte pin, int value)
{
  if (pin < TOTAL_PINS && IS_PIN_DIGITAL(pin)) {
    if (Firmata.getPinMode(pin) == OUTPUT) {
      Firmata.setPinState(pin, value);
      digitalWrite(PIN_TO_DIGITAL(pin), value);
    }
  }
}

void analogWriteCallback(byte pin, int value)
{
  if (pin < TOTAL_PINS) {
    switch (Firmata.getPinMode(pin)) {
      case PIN_MODE_SERVO:
        if (IS_PIN_DIGITAL(pin))
          servos[servoPinMap[pin]].write(value);
        Firmata.setPinState(pin, value);
        break;
      case PIN_MODE_PWM:
        if (IS_PIN_PWM(pin))
          analogWrite(PIN_TO_PWM(pin), value);
        Firmata.setPinState(pin, value);
        break;
    }
  }
}

void digitalWriteCallback(byte port, int value)
{
  byte pin, lastPin, pinValue, mask = 1, pinWriteMask = 0;

  if (port < TOTAL_PORTS) {
    // create a mask of the pins on this port that are writable.
    lastPin = port * 8 + 8;
    if (lastPin > TOTAL_PINS) lastPin = TOTAL_PINS;
    for (pin = port * 8; pin < lastPin; pin++) {
      // do not disturb non-digital pins (eg, Rx & Tx)
      if (IS_PIN_DIGITAL(pin)) {
        // only write to OUTPUT and INPUT (enables pullup)
        // do not touch pins in PWM, ANALOG, SERVO or other modes
        if (Firmata.getPinMode(pin) == OUTPUT || Firmata.getPinMode(pin) == INPUT) {
          pinValue = ((byte)value & mask) ? 1 : 0;
          if (Firmata.getPinMode(pin) == OUTPUT) {
            pinWriteMask |= mask;
          } else if (Firmata.getPinMode(pin) == INPUT && pinValue == 1 && Firmata.getPinState(pin) != 1) {
            // only handle INPUT here for backwards compatibility
#if ARDUINO > 100
            pinMode(pin, INPUT_PULLUP);
#else
            // only write to the INPUT pin to enable pullups if Arduino v1.0.0 or earlier
            pinWriteMask |= mask;
#endif
          }
          Firmata.setPinState(pin, pinValue);
        }
      }
      mask = mask << 1;
    }
    writePort(port, (byte)value, pinWriteMask);
  }
}


// -----------------------------------------------------------------------------
/* sets bits in a bit array (int) to toggle the reporting of the analogIns
 */
//void FirmataClass::setAnalogPinReporting(byte pin, byte state) {
//}
void reportAnalogCallback(byte analogPin, int value)
{
  if (analogPin < TOTAL_ANALOG_PINS) {
    if (value == 0) {
      analogInputsToReport = analogInputsToReport & ~ (1 << analogPin);
    } else {
      analogInputsToReport = analogInputsToReport | (1 << analogPin);
      // prevent during system reset or all analog pin values will be reported
      // which may report noise for unconnected analog pins
      if (!isResetting) {
        // Send pin value immediately. This is helpful when connected via
        // ethernet, wi-fi or bluetooth so pin states can be known upon
        // reconnecting.
        Firmata.sendAnalog(analogPin, analogRead(analogPin));
      }
    }
  }
  // TODO: save status to EEPROM here, if changed
}

void reportDigitalCallback(byte port, int value)
{
  if (port < TOTAL_PORTS) {
    reportPINs[port] = (byte)value;
    // Send port value immediately. This is helpful when connected via
    // ethernet, wi-fi or bluetooth so pin states can be known upon
    // reconnecting.
    if (value) outputPort(port, readPort(port, portConfigInputs[port]), true);
  }
  // do not disable analog reporting on these 8 pins, to allow some
  // pins used for digital, others analog.  Instead, allow both types
  // of reporting to be enabled, but check if the pin is configured
  // as analog when sampling the analog inputs.  Likewise, while
  // scanning digital pins, portConfigInputs will mask off values from any
  // pins configured as analog
}

/*==============================================================================
 * SYSEX-BASED commands
 *============================================================================*/

void sysexCallback(byte command, byte argc, byte *argv)
{
  byte mode;
  byte stopTX;
  byte slaveAddress;

  byte data;
  int slaveRegister;
  unsigned int delayTime;
  byte pin ;// used for tone
  int frequency ;
  int duration ;
  
  unsigned long angle;
  unsigned long msec;
  unsigned long velocity;

  byte sendBuff[512];
  int i;
  
  switch (command) {
    case I2C_REQUEST:
      mode = argv[1] & I2C_READ_WRITE_MODE_MASK;
      if (argv[1] & I2C_10BIT_ADDRESS_MODE_MASK) {
        Firmata.sendString("10-bit addressing not supported");
        return;
      }
      else {
        slaveAddress = argv[0];
      }


      // need to invert the logic here since 0 will be default for client
      // libraries that have not updated to add support for restart tx
      if (argv[1] & I2C_END_TX_MASK) {
        stopTX = I2C_RESTART_TX;
      }
      else {
        stopTX = I2C_STOP_TX; // default
      }

      switch (mode) {
        case I2C_WRITE:
          Wire.beginTransmission(slaveAddress);
          for (byte i = 2; i < argc; i += 2) {
            data = argv[i] + (argv[i + 1] << 7);
            wireWrite(data);
          }
          Wire.endTransmission();
          delayMicroseconds(70);
          break;
        case I2C_READ:
          if (argc == 6) {
            // a slave register is specified
            slaveRegister = argv[2] + (argv[3] << 7);
            data = argv[4] + (argv[5] << 7);  // bytes to read
            //readAndReportData(slaveAddress, (int)slaveRegister, data);
          }
          else {
            // a slave register is NOT specified
            slaveRegister = I2C_REGISTER_NOT_SPECIFIED;
            data = argv[2] + (argv[3] << 7);  // bytes to read
            //readAndReportData(slaveAddress, (int)REGISTER_NOT_SPECIFIED, data);
          }
          readAndReportData(slaveAddress, (int)slaveRegister, data, stopTX);
          break;
        case I2C_READ_CONTINUOUSLY:
          if ((queryIndex + 1) >= I2C_MAX_QUERIES) {
            // too many queries, just ignore
            Firmata.sendString("too many queries");
            break;
          }
          if (argc == 6) {
            // a slave register is specified
            slaveRegister = argv[2] + (argv[3] << 7);
            data = argv[4] + (argv[5] << 7);  // bytes to read
          }
          else {
            // a slave register is NOT specified
            slaveRegister = (int)I2C_REGISTER_NOT_SPECIFIED;
            data = argv[2] + (argv[3] << 7);  // bytes to read
          }
          queryIndex++;
          query[queryIndex].addr = slaveAddress;
          query[queryIndex].reg = slaveRegister;
          query[queryIndex].bytes = data;
          query[queryIndex].stopTX = stopTX;
          break;
        case I2C_STOP_READING:
          byte queryIndexToSkip;
          // if read continuous mode is enabled for only 1 i2c device, disable
          // read continuous reporting for that device
          if (queryIndex <= 0) {
            queryIndex = -1;
          } else {
            queryIndexToSkip = 0;
            // if read continuous mode is enabled for multiple devices,
            // determine which device to stop reading and remove it's data from
            // the array, shifiting other array data to fill the space
            for (byte i = 0; i < queryIndex + 1; i++) {
              if (query[i].addr == slaveAddress) {
                queryIndexToSkip = i;
                break;
              }
            }

            for (byte i = queryIndexToSkip; i < queryIndex + 1; i++) {
              if (i < I2C_MAX_QUERIES) {
                query[i].addr = query[i + 1].addr;
                query[i].reg = query[i + 1].reg;
                query[i].bytes = query[i + 1].bytes;
                query[i].stopTX = query[i + 1].stopTX;
              }
            }
            queryIndex--;
          }
          break;
        default:
          break;
      }
      break;
    case I2C_CONFIG:
      delayTime = (argv[0] + (argv[1] << 7));

      if (delayTime > 0) {
        i2cReadDelayTime = delayTime;
      }

      if (!isI2CEnabled) {
        enableI2CPins();
      }

      break;
    case SERVO_CONFIG:
      if (argc > 4) {
        // these vars are here for clarity, they'll optimized away by the compiler
        byte pin = argv[0];
        int minPulse = argv[1] + (argv[2] << 7);
        int maxPulse = argv[3] + (argv[4] << 7);

        if (IS_PIN_DIGITAL(pin)) {
          if (servoPinMap[pin] < MAX_SERVOS && servos[servoPinMap[pin]].attached()) {
            detachServo(pin);
          }
          attachServo(pin, minPulse, maxPulse);
          setPinModeCallback(pin, PIN_MODE_SERVO);
        }
      }
      break;
    case KEEP_ALIVE:
      keepAliveInterval = argv[0] + (argv[1] << 7);
      previousKeepAliveMillis = millis();
      break;      
    case SAMPLING_INTERVAL:
      if (argc > 1) {
        samplingInterval = argv[0] + (argv[1] << 7);
        if (samplingInterval < MINIMUM_SAMPLING_INTERVAL) {
          samplingInterval = MINIMUM_SAMPLING_INTERVAL;
        }
        /* calculate number of loops per ping */
        numLoops = INTER_PING_INTERVAL / samplingInterval ;
        //numLoops = 1 ;
      }
      else {
        //Firmata.sendString("Not enough data");
      }
      break;
    case EXTENDED_ANALOG:
      if (argc > 1) {
        int val = argv[1];
        if (argc > 2) val |= (argv[2] << 7);
        if (argc > 3) val |= (argv[3] << 14);
        analogWriteCallback(argv[0], val);
      }
      break;
    case CAPABILITY_QUERY:
      for (i=0; i<sizeof(sendBuff); i++) sendBuff[i] = 0;
      
      sendBuff[0] = START_SYSEX;
      sendBuff[1] = CAPABILITY_RESPONSE;
      i = 2;
      for (byte pin = 0; pin < TOTAL_PINS; pin++) {
        if (IS_PIN_DIGITAL(pin)) {
          sendBuff[i++] = ((byte)INPUT);
          sendBuff[i++] = 1;
          sendBuff[i++] = ((byte)PIN_MODE_PULLUP);
          sendBuff[i++] = 1;
          sendBuff[i++] = ((byte)OUTPUT);
          sendBuff[i++] = 1;
        }
        if (IS_PIN_ANALOG(pin)) {
          sendBuff[i++] = PIN_MODE_ANALOG;
          sendBuff[i++] = 10; // 10 = 10-bit resolution
        }
        if (IS_PIN_PWM(pin)) {
          sendBuff[i++] = PIN_MODE_PWM;
          sendBuff[i++] = DEFAULT_PWM_RESOLUTION;
        }
        if (IS_PIN_DIGITAL(pin)) {
          sendBuff[i++] = PIN_MODE_SERVO;
          sendBuff[i++] = 14;
        }
        if (IS_PIN_I2C(pin)) {
          sendBuff[i++] = PIN_MODE_I2C;
          sendBuff[i++] = 1;  // TODO: could assign a number to map to SCL or SDA
        }
#ifdef FIRMATA_SERIAL_FEATURE
        serialFeature.handleCapability(pin);
#endif
        sendBuff[i++] = 127;
      }
      sendBuff[i++] = END_SYSEX;
      Firmata.write((const uint8_t*)sendBuff, i);
      break;
    case PIN_STATE_QUERY:
      if (argc > 0) {
        for (i=0; i<sizeof(sendBuff); i++) sendBuff[i] = 0;
        
        byte pin = argv[0];
        sendBuff[0] = START_SYSEX;
        sendBuff[1] = PIN_STATE_RESPONSE;
        sendBuff[2] = pin;
        i = 3;
        if (pin < TOTAL_PINS) {
          sendBuff[i++] = Firmata.getPinMode(pin);
          sendBuff[i++] = (byte)Firmata.getPinState(pin) & 0x7F;
          if (Firmata.getPinState(pin) & 0xFF80) sendBuff[i++] = (byte)(Firmata.getPinState(pin) >> 7) & 0x7F;
          if (Firmata.getPinState(pin) & 0xC000) sendBuff[i++] = (byte)(Firmata.getPinState(pin) >> 14) & 0x7F;
        }
        sendBuff[i++] = END_SYSEX;
        Firmata.write((const uint8_t*)sendBuff, i);
      }
      break;
    case ANALOG_MAPPING_QUERY:
      for (i=0; i<sizeof(sendBuff); i++) sendBuff[i] = 0;
      sendBuff[0] = START_SYSEX;
      sendBuff[1] = ANALOG_MAPPING_RESPONSE;
      i = 2;
      for (byte pin = 0; pin < TOTAL_PINS; pin++) {
        sendBuff[i++] = (IS_PIN_ANALOG(pin) ? PIN_TO_ANALOG(pin) : 127);
      }
      sendBuff[i++] = END_SYSEX;
      Firmata.write((const uint8_t*)sendBuff, i);
      break;
  case ENCODER_CONFIG:
    // instantiate an encoder object with user's
    // requested pin designators
    encoderPresent = ((argv[0] == 0) ? false : true);
    encoderModule = argv[1] ; // for 86Duino, it is the "number of module"
    
    if (pre_encoderPresent == true && encoderPresent == false)
    {
        if      (encoderModule == 0) Enc0.end();
        else if (encoderModule == 1) Enc1.end();
        else if (encoderModule == 2) Enc2.end();
        else if (encoderModule == 3) Enc3.end();
        break;    
    }
    else if (pre_encoderPresent == true && encoderPresent == true)
    {
        encoderMode = argv[2] ; // for 86Duino, it is "encoder mode"
        if      (encoderModule == 0) {Enc0.end(); Enc0.begin(encoderMode);}
        else if (encoderModule == 1) {Enc1.end(); Enc1.begin(encoderMode);}
        else if (encoderModule == 2) {Enc2.end(); Enc2.begin(encoderMode);}
        else if (encoderModule == 3) {Enc3.end(); Enc3.begin(encoderMode);}
        break;   
    }
    else if (pre_encoderPresent == false && encoderPresent == true)
    {
        encoderMode = argv[2] ; // for 86Duino, it is "encoder mode"
        if      (encoderModule == 0) Enc0.begin(encoderMode);
        else if (encoderModule == 1) Enc1.begin(encoderMode);
        else if (encoderModule == 2) Enc2.begin(encoderMode);
        else if (encoderModule == 3) Enc3.begin(encoderMode);
        break;
    }
    pre_encoderPresent = encoderPresent;
    // static AdaEncoder encoder = 
      // AdaEncoder('a', encoderModule, encoderMode) ;
    //thisEncoder = &encoder ;
    // encoderPresent = true ;
    break ; 

  case SERIAL_MESSAGE:
#ifdef FIRMATA_SERIAL_FEATURE
      serialFeature.handleSysex(command, argc, argv);
#endif
      break;

    case TONE_PLAY:
      byte toneCommand, pin;
      int frequency, duration;

      toneCommand = argv[0];
      pin = argv[1];

      if (toneCommand == TONE_TONE) {
        frequency = argv[2] + (argv[3] << 7);
        // duration is currently limited to 16,383 ms
        duration = argv[4] + (argv[5] << 7);
        tone(pin, frequency, duration);
      }
      else if (toneCommand == TONE_NO_TONE) {
        noTone(pin);
      }
      break ;

      // arg0 = trigger pin
      // arg1 = echo pin
      // arg2 = ping interval in milliseconds if 0, then set to 33
      // arg3 = maxdistance lsb
      // arg4 = maxdistance msb
    case SONAR_CONFIG :
      int max_distance ;
      if ( numActiveSonars < MAX_SONARS)
      {
        sonarTriggerPin = argv[0] ;
        sonarEchoPin = argv[1] ;
        // set interval to a minium of 33 ms.
        if ( argv[2] >= 33 ) {
          pingInterval = argv[2] ;
        }
        else {
          pingInterval = 33 ;
        }
        max_distance = argv[3] + (argv[4] << 7 ) ;
        sonarPinNumbers[numActiveSonars] = sonarTriggerPin ;

        setPinModeCallback(sonarTriggerPin, PIN_MODE_SONAR);
        setPinModeCallback(sonarEchoPin, PIN_MODE_SONAR);
        sonars[numActiveSonars] = new NewPing(sonarTriggerPin, sonarEchoPin, max_distance) ;
        numActiveSonars++ ;
      }
      else {
        Firmata.sendString("PING_CONFIG Error: Exceeded number of supported ping devices");
      }
      break ;

    case STEPPER_DATA:
      // determine if this a STEPPER_CONFIGURE command or STEPPER_OPERATE command
      if (argv[0] == STEPPER_CONFIGURE)
      {
        int numSteps = argv[1] + (argv[2] << 7);
        int pin1 = argv[3] ;
        int pin2 = argv[4] ;
        if ( argc == 5 )
        {
          // two pin motor
          stepper = new Stepper(numSteps, pin1, pin2) ;
        }
        else if (argc == 7 ) // 4 wire motor
        {
          int pin3 = argv[5] ;
          int pin4 = argv[6] ;
          stepper =  new Stepper(numSteps, pin1, pin2, pin3, pin4) ;
        }
        else
        {
          Firmata.sendString("STEPPER CONFIG Error: Wrong Number of arguments");
          printData("argc = ", argc) ;
        }
      }
      else if ( argv[0] == STEPPER_STEP )
      {
        long speed = (long)argv[1] | ((long)argv[2] << 7) | ((long)argv[3] << 14);
        int numSteps = argv[4] + (argv[5] << 7);
        int direction = argv[6] ;
        if (stepper != NULL )
        {
          stepper->setSpeed(speed) ;
          if (direction == 0 )
          {
            numSteps *= -1 ;
          }
          stepper->step(numSteps) ;
        }
        else
        {
          Firmata.sendString("STEPPER OPERATE Error: MOTOR NOT CONFIGURED");
        }
      }
      else if ( argv[0] == STEPPER_LIBRARY_VERSION )
      {
        if ( stepper != NULL )
        {
          int version = stepper->version() ;
          Firmata.write(START_SYSEX);
          Firmata.write(STEPPER_DATA);
          Firmata.write(version & 0x7F);
          Firmata.write(version >> 7);
          Firmata.write(END_SYSEX);
        }
        else
        {
          // did not find a configured stepper
          Firmata.sendString("STEPPER FIRMWARE VERSION Error: NO MOTORS CONFIGURED");
        }
        break ;
      }
      else
      {
        Firmata.sendString("STEPPER CONFIG Error: UNKNOWN STEPPER COMMAND");
      }
      break ;
      
    case MOVE_ONE_SERVO:
      pin = one_servo_used_pin = (int)argv[0];
	  angle = (long)argv[1] | ((long)argv[2] << 7);
      
      if (angle < MIN_PULSE_WIDTH)
	  { // treat values less than 450 as angles in degrees (valid values in microseconds are handled as microseconds)
		if (angle < 0) angle = 0;
		if (angle > 180) angle = 180;
		angle = map(angle, 0, 180, MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);      
	  }
    
      if (argv[3] == 0)
      {
        msec = (long)argv[4] | ((long)argv[5] << 7);
        servos[servoPinMap[pin]].setPosition(angle, msec);
      }
      else if (argv[3] == 1)
      {
        velocity = (long)argv[4] | ((long)argv[5] << 7);
        servos[servoPinMap[pin]].setVelocity(velocity);
        servos[servoPinMap[pin]].setPosition(angle);
      }
      servos[servoPinMap[pin]].run();
      
      one_servo_is_moving = true;
      
      break;
      
    case SERVO_MOVING_ALL:
      servoMultiRun();
      break ;
      
	case SERVO_CONFIG_EX:
	  pin = (int)argv[0];
	  angle = (long)argv[1] | ((long)argv[2] << 7);
      
      if (angle < MIN_PULSE_WIDTH)
	  { // treat values less than 450 as angles in degrees (valid values in microseconds are handled as microseconds)
		if (angle < 0) angle = 0;
		if (angle > 180) angle = 180;
		angle = map(angle, 0, 180, MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);      
	  }
      
      if (argv[3] == 0) // waiting time
      {
        msec = (long)argv[4] | ((long)argv[5] << 7);
        servos[servoPinMap[pin]].setPosition(angle, msec);
      }
      else if (argv[3] == 1) // set speed
      {
        velocity = (long)argv[4] | ((long)argv[5] << 7);
        servos[servoPinMap[pin]].setVelocity(velocity);
        servos[servoPinMap[pin]].setPosition(angle);
      }
	  break ;
    
    case ENABLE_IMU:
      enableIMUWaiting = true;
      imu_waiting_id = (int)argv[0] | ((int)argv[1] << 7);
      break;
    case CHECK_86DUINO_ACTIVE:
      checkActiveStart = true;
      break;
  }
}

void enableI2CPins()
{
  byte i;
  // is there a faster way to do this? would probaby require importing
  // Arduino.h to get SCL and SDA pins
  for (i = 0; i < TOTAL_PINS; i++) {
    if (IS_PIN_I2C(i)) {
      // mark pins as i2c so they are ignore in non i2c data requests
      setPinModeCallback(i, PIN_MODE_I2C);
    }
  }

  isI2CEnabled = true;

  // is there enough time before the first I2C request to call this here?
  Wire.begin();
}

/* disable the i2c pins so they can be used for other functions */
void disableI2CPins() {
  isI2CEnabled = false;
  // disable read continuous mode for all devices
  queryIndex = -1;
  // uncomment the following if or when the end() method is added to Wire library
  // Wire.end();
}

/*==============================================================================
 * SETUP()
 *============================================================================*/

void systemResetCallback()
{
  if (isResetting) return;
  isResetting = true;

  // initialize a defalt state
  // TODO: option to load config from EEPROM instead of default
#ifdef FIRMATA_SERIAL_FEATURE
  serialFeature.reset();
#endif

  if (isI2CEnabled) {
    disableI2CPins();
  }
  for (byte i = 0; i < TOTAL_PORTS; i++) {
    reportPINs[i] = false;    // by default, reporting off
    portConfigInputs[i] = 0;  // until activated
    previousPINs[i] = 0;
  }
  // pins with analog capability default to analog input
  // otherwise, pins default to digital output
  for (byte i = 0; i < TOTAL_PINS; i++) {
    if (IS_PIN_ANALOG(i)) {
      // turns off pullup, configures everything
      setPinModeCallback(i, PIN_MODE_ANALOG);
    }
    else if ( IS_PIN_TONE(i)) {
      if (servoPinMap[i] < MAX_SERVOS && servos[servoPinMap[i]].attached()) {
        detachServo(i);
      }
      noTone(i) ;
    }
    else {
      // sets the output to 0, configures portConfigInputs
      setPinModeCallback(i, OUTPUT);
    }
    servoPinMap[i] = 255;
  }
  // stop pinging
  numActiveSonars = 0 ;
  for (int i = 0; i < MAX_SONARS; i++) {
    sonarPinNumbers[i] = PIN_MODE_IGNORE ;
    if ( sonars[i] ) {
      sonars[i] = NULL ;
    }
  }
  numActiveSonars = 0 ;
  
  // Encoder
  if (encoderPresent == true)
  {
      if      (encoderModule == 0) Enc0.end();
      else if (encoderModule == 1) Enc1.end();
      else if (encoderModule == 2) Enc2.end();
      else if (encoderModule == 3) Enc3.end();
      pre_encoderPresent = encoderPresent = false;
  }

  // by default, do not report any analog inputs
  analogInputsToReport = 0;

  detachedServoCount = 0;
  servoCount = 0;

  // clear stepper pointer
  stepper = NULL ;

  /* send digital inputs to set the initial state on the host computer,
   * since once in the loop(), this firmware will only send on change */
  /*
  TODO: this can never execute, since no pins default to digital input
        but it will be needed when/if we support EEPROM stored config
  for (byte i=0; i < TOTAL_PORTS; i++) {
    outputPort(i, readPort(i, portConfigInputs[i]), true);
  }
  */
  isResetting = false;
  
  imu_data_start_sampling = false;

}

void setup()
{
  #if CONNECT_86DUINO_METHOD == 0
    Firmata.begin(57600);
  #elif CONNECT_86DUINO_METHOD == 1
    Firmata.beginBlueTooth(BT_ESP8266_Serial, BT_ESP8266_Serial_Baud);
  #elif CONNECT_86DUINO_METHOD == 2
    Firmata.beginWiFiShield(projectName, 2000, _ssid, _password, _wep, _ip);
  #elif CONNECT_86DUINO_METHOD == 3
    #ifdef ETHERNET_DHCP
      Firmata.beginEthernet(projectName, 2000);
    #else
      Firmata.beginEthernet(projectName, 2000, localIP, subnet, dnsserver, gateway);
    #endif
  #elif CONNECT_86DUINO_METHOD == 4
      Firmata.beginESP8266(projectName, 2000, BT_ESP8266_Serial, BT_ESP8266_Serial_Baud, ch_pd_pin, _ssid, _password);
  #elif CONNECT_86DUINO_METHOD == 5
      Firmata.beginESP8266_AP(projectName, 2000, BT_ESP8266_Serial, BT_ESP8266_Serial_Baud, ch_pd_pin, _ssid, _password, _chl, _ecn);
  #endif
  
  Firmata.setFirmwareVersion(FIRMATA_FIRMWARE_MAJOR_VERSION, FIRMATA_FIRMWARE_MINOR_VERSION);

  Firmata.attach(ANALOG_MESSAGE, analogWriteCallback);
  Firmata.attach(DIGITAL_MESSAGE, digitalWriteCallback);
  Firmata.attach(REPORT_ANALOG, reportAnalogCallback);
  Firmata.attach(REPORT_DIGITAL, reportDigitalCallback);
  Firmata.attach(SET_PIN_MODE, setPinModeCallback);
  Firmata.attach(SET_DIGITAL_PIN_VALUE, setPinValueCallback);
  Firmata.attach(START_SYSEX, sysexCallback);
  Firmata.attach(SYSTEM_RESET, systemResetCallback);
  
  #if CONNECT_86DUINO_METHOD == 2
    pinMode(PIN_TO_DIGITAL(4), OUTPUT);    // switch off SD card bypassing Firmata
    digitalWrite(PIN_TO_DIGITAL(4), HIGH); // SS is active low;
  #endif
  
  systemResetCallback();  // reset to default config
}

void check_localIP()
{
    static bool serial_monitor_open = false;
    if (Serial && serial_monitor_open == false)
    {
        serial_monitor_open = true;
        Serial.print("IP : ");
        Serial.println(Firmata.getLocalIP());
        Serial.print("Gateway : ");
        Serial.println(Firmata.getGatewayIP());
        Serial.print("SubnetMask : ");
        Serial.println(Firmata.getSubnetMask());
    }
    
    if (!Serial && serial_monitor_open == true)
    {
        serial_monitor_open = false;
    }
}

/*==============================================================================
 * LOOP()
 *============================================================================*/
void loop()
{
  byte pin, analogPin;
  int i, pingResult = 0;
  float ypr[3]; // yaw pitch roll
  byte sendBuff[512];

  #if CONNECT_86DUINO_METHOD == 2 || CONNECT_86DUINO_METHOD == 3 || CONNECT_86DUINO_METHOD == 4 || CONNECT_86DUINO_METHOD == 5
    check_localIP();
  #endif
  
  /* DIGITALREAD - as fast as possible, check for changes and output them to the
   * FTDI buffer using Serial.print()  */
  checkDigitalInputs();

  /* STREAMREAD - processing incoming messagse as soon as possible, while still
   * checking digital inputs.  */
  while (Firmata.available())
    Firmata.processInput();

  // TODO - ensure that Stream buffer doesn't go over 60 bytes

  currentMillis = millis();
  if (currentMillis - previousMillis > samplingInterval) {
    previousMillis += samplingInterval;

    if ( pingLoopCounter++ > numLoops)
    {
      pingLoopCounter = 0 ;
      if (numActiveSonars)
      {
        unsigned int uS = sonars[nextSonar]->ping();
        // Convert ping time to distance in cm and print
        pingResult = uS / US_ROUNDTRIP_CM ;
        currentSonar = nextSonar ;
        if ( nextSonar++ >= numActiveSonars - 1)
        {
          nextSonar = 0 ;
        }
        sonarLSB = pingResult & 0x7f ;
        sonarMSB = pingResult >> 7 & 0x7f ;

        for (i=0; i<sizeof(sendBuff); i++) sendBuff[i] = 0;
        // printf("sonarPinNumbers[currentSonar]=%d\n", sonarPinNumbers[currentSonar]);
        sendBuff[0] = START_SYSEX;
        sendBuff[1] = SONAR_DATA;
        sendBuff[2] = sonarPinNumbers[currentSonar];
        sendBuff[3] = sonarLSB;
        sendBuff[4] = sonarMSB;
        sendBuff[5] = END_SYSEX;
        
        Firmata.write((const uint8_t*)sendBuff, 6);
      }
    }


    // ANALOGREAD - do all analogReads() at the configured sampling interval 
    for (pin = 0; pin < TOTAL_PINS; pin++) {
      if (IS_PIN_ANALOG(pin) && Firmata.getPinMode(pin) == PIN_MODE_ANALOG) {
        analogPin = PIN_TO_ANALOG(pin);
        if (analogInputsToReport & (1 << analogPin)) {
          Firmata.sendAnalog(analogPin, analogRead(analogPin));
        }
      }
    }
    // report i2c data for all device with read continuous mode enabled
    if (queryIndex > -1) {
      for (byte i = 0; i < queryIndex + 1; i++) {
        readAndReportData(query[i].addr, query[i].reg, query[i].bytes, query[i].stopTX);
      }
    }
    // if encoder was installed, return its data
    if ( encoderPresent == true)
    {
      for (i=0; i<sizeof(sendBuff); i++) sendBuff[i] = 0;
      
      if (encoderModule == 0)
          encoderPostion = Enc0.read();
      else if (encoderModule == 1)
          encoderPostion = Enc1.read();
      else if (encoderModule == 2)
          encoderPostion = Enc2.read();
      else if (encoderModule == 3)
          encoderPostion = Enc3.read();
      
      encoderLSB = (unsigned char) (encoderPostion & 0xFFL);
      encoderMSB = (unsigned char) ((encoderPostion >> 8) & 0xFFL);
      encoderHLSB = (unsigned char) ((encoderPostion >> 16) & 0xFFL);
      encoderHMSB = (unsigned char) ((encoderPostion >> 24) & 0xFFL);
      encoderLastB = ((encoderLSB & 0x80) >> 7) | ((encoderMSB & 0x80) >> 6) | ((encoderHLSB & 0x80) >> 5) | ((encoderHMSB & 0x80) >> 4);

      sendBuff[0] = START_SYSEX;
      sendBuff[1] = ENCODER_DATA;
      sendBuff[2] = encoderModule;
      sendBuff[3] = encoderLSB & 0x7F;
      sendBuff[4] = encoderMSB & 0x7F;
      sendBuff[5] = encoderHLSB & 0x7F;
      sendBuff[6] = encoderHMSB & 0x7F;
      sendBuff[7] = encoderLastB;
      sendBuff[8] = END_SYSEX;
      
      Firmata.write((const uint8_t*)sendBuff, 9);
    }
    if( keepAliveInterval ) {
      currentMillis = millis();
      if (currentMillis - previousKeepAliveMillis > keepAliveInterval*1000) {
        systemResetCallback();
        
        wdt_enable(WDTO_15MS);
        // systemResetCallback();
        while(1)
           ;
      }
    }
    if(imu_data_start_sampling) {
    #if defined __86DUINO_ONE || defined __86DUINO_AI
      for (i=0; i<sizeof(sendBuff); i++) sendBuff[i] = 0;
      
      my3IMU.getYawPitchRoll(ypr);
      char* pitch_data_p = (char*) &ypr[1];
      char* roll_data_p = (char*) &ypr[2];
      char data_5byte[2][5];
      
      data_5byte[0][0] = pitch_data_p[0] & 0x7F;
      data_5byte[0][1] = ((pitch_data_p[1] & 0x3F) << 1) | ((pitch_data_p[0] & 0x80) >> 7);
      data_5byte[0][2] = ((pitch_data_p[2] & 0x1F) << 2) | ((pitch_data_p[1] & 0xC0) >> 6);
      data_5byte[0][3] = ((pitch_data_p[3] & 0x0F) << 3) | ((pitch_data_p[2] & 0xE0) >> 5);
      data_5byte[0][4] = (pitch_data_p[3] & 0xF0) >> 4;
      
      data_5byte[1][0] = roll_data_p[0] & 0x7F;
      data_5byte[1][1] = ((roll_data_p[1] & 0x3F) << 1) | ((roll_data_p[0] & 0x80) >> 7);
      data_5byte[1][2] = ((roll_data_p[2] & 0x1F) << 2) | ((roll_data_p[1] & 0xC0) >> 6);
      data_5byte[1][3] = ((roll_data_p[3] & 0x0F) << 3) | ((roll_data_p[2] & 0xE0) >> 5);
      data_5byte[1][4] = (roll_data_p[3] & 0xF0) >> 4;

      sendBuff[0] = START_SYSEX;
      sendBuff[1] = IMU_RESPONSE;
      sendBuff[2] = data_5byte[0][0];
      sendBuff[3] = data_5byte[0][1];
      sendBuff[4] = data_5byte[0][2];
      sendBuff[5] = data_5byte[0][3];
      sendBuff[6] = data_5byte[0][4];
      sendBuff[7] = data_5byte[1][0];
      sendBuff[8] = data_5byte[1][1];
      sendBuff[9] = data_5byte[1][2];
      sendBuff[10] = data_5byte[1][3];
      sendBuff[11] = data_5byte[1][4];
      sendBuff[12] = END_SYSEX;
      
      Firmata.write((const uint8_t*)sendBuff, 13);
    #endif
    }
    
    if (one_servo_is_moving && servos[servoPinMap[one_servo_used_pin]].isMoving() == false) {
        for (i=0; i<sizeof(sendBuff); i++) sendBuff[i] = 0;
        
        sendBuff[0] = START_SYSEX;
        sendBuff[1] = MOVE_ONE_SERVO;
        sendBuff[2] = 99;
        sendBuff[3] = END_SYSEX;
        Firmata.write((const uint8_t*)sendBuff, 4);
        
        one_servo_is_moving = false;
    }
    
    if (enableIMUWaiting) {
        for (i=0; i<sizeof(sendBuff); i++) sendBuff[i] = 0;
        #if defined __86DUINO_ONE || defined __86DUINO_AI
            Wire.begin();
            delay(5);
            my3IMU.init(); // the parameter enable or disable fast mode
            delay(5);
        #endif
        
        sendBuff[0] = START_SYSEX;
        sendBuff[1] = ENABLE_IMU;
        sendBuff[2] = imu_waiting_id & 0x7F;
        sendBuff[3] = (imu_waiting_id >> 7) & 0x7F;
        sendBuff[4] = 99;
        sendBuff[5] = END_SYSEX;
        
        Firmata.write((const uint8_t*)sendBuff, 6);
        
        enableIMUWaiting = false;
        imu_data_start_sampling = true;
    }
    
    if (checkActiveStart == true)
    {
        Firmata.write(START_SYSEX);
        Firmata.write(_86DUINO_RESPONSE) ;
        Firmata.write(0x5A)  ;
        Firmata.write(END_SYSEX);
        checkActiveStart = false;
    }
    
  }
}



void printData(char * id,  long data)
{
  char myArray[64] ;

  String myString = String(data);
  myString.toCharArray(myArray, 64) ;
  Firmata.sendString(id) ;
  Firmata.sendString(myArray);
}



