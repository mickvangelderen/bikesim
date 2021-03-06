/*
    title:    BsgBikeSim2014
    date:    18-12-2014
    author: Thom van Beek, thom@fietsenmakers.net +31648254856
    copyright Fietsenmakers Product developers 2014.

    description:
        This arduino software executes a bicycle simulator application on a hometrainer setup with a haptic steer.
        The arduino gets the sensory data. Does a first form of processing and sends it to the serial line on a regular
        basis/frequency. It senses:
        steer angle (delta) in [rad]
        steer rate (deltaDot) in [rad/s]
        pedal frequency (cadence) in [RPM]

        It also sends the steer output torque to the Maxon motor controller:
        Steer torque (Td) in [Nm]

        The way the arduino software implements this, in pseudocode, is:
        Interrupt Service Routine I:
        - set a timed interrupt on a regular interval.
        - in the Interrupt Service Routine (ISR) raise a flag
        - exit the ISR

        In the Loop() routine:
        - if flag was raised:
            - set the noInterrupt flag (? should we?)
            - update the analog input signals from the steer angle and steer
              rate sensors
            - update the digital input of the brake signal
            - Send the sensory data (steer angle, steering rate, cadence,
              brake) over serial communication as a CSV in fixed order
        - if serial.available
            - add it to the serial input buffer until a full line is received
            - If full line received:
                - calculate the output voltage from the desired torque
                - Set the output voltage on the connected DAC module

*/

/*    Dependencies / Libraries */
#include <Wire.h>
#include <Adafruit_MCP4725.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include "sample.h"
#include "butterlowpass.h"

#define SERIAL_PREFIX_CHAR 's'
#define SERIAL_SUFFIX_CHAR 'e'

namespace {
    /*    Constants definitions */
    // Pin assignments:
    const int DELTAPIN = A0;
    const int DELTADOTPIN = A1;
    const int MCENABLEPIN = 6;  // Digital output pin enabling the motorcontroller
    const int CADENCEPIN = 4; // Input trigger for the cadence counter timer interrupt on Timer1
    const int BRAKEHANDLE_INT = 4; // Input trigger for the brake signal. INT 4 is on pin 7 of Leonardo
    const int BRAKEINPUTPIN = 7; // Input trigger pin for the brake signal.
                                 // attach it to external interrupt. Pin 7 for INT 4.

    // Define conversion factor from measured analog signal to delta
    // 12 bit ADC -> 4096 values
    // with 5V range, 1.22 mV / bit
    // Using values below: (62 degrees)/(680 - 289 bits) = 0.159 degrees/bit
    // 0.130 degrees / mV
    const float DELTAMAXLEFT = -32.0f;
    const float DELTAMAXRIGHT = 30.0f;
    const float VALMAXLEFT = 289.0f;
    const float VALMAXRIGHT = 680.0f;
    const float SLOPE_DELTA = (DELTAMAXRIGHT - DELTAMAXLEFT)/
        (VALMAXRIGHT - VALMAXLEFT);
    const float C_DELTA = DELTAMAXLEFT - SLOPE_DELTA*VALMAXLEFT;

    // Delta dot:
    // From datasheet: 20 mV/(deg/s)
    // (1 (deg/s)) / (20 mV) * 1.22mV/bit = 0.061 (deg/s)/bit
    const float SLOPE_DELTADOT = -0.24438f;
    const float C_DELTADOT = 125.0f;

    // conversion factor from degrees to radians
    const float DEGTORAD = 3.14/180;

    // Define the sampling frequency serial tranmission frequency with TIMER3
    //   Use no more than 100 Hz for serial transmission rate where
    //   SERIAL_TX_FREQ = SAMPLING_FREQ/SERIAL_TX_PRE
    const int SAMPLING_FREQ = 200;
    const int SERIAL_TX_PRE = 4; // prescaler for serial transmission
    int txCount = 0;

    // Define constants for converstion from torque to motor PWM
    const float maxon_346970_max_current_peak = 3.0f; // A
    const float maxon_346970_max_current_cont = 1.780f; // A
    const float maxon_346970_torque_constant = 0.217f; // Nm/A
    const float maxon_346970_max_torque_peak =
        maxon_346970_max_current_peak * maxon_346970_torque_constant; // Nm
    const float maxon_346970_max_torque_cont =
        maxon_346970_max_current_cont * maxon_346970_torque_constant; // Nm

    // measured with calipers
    const float gearwheel_mechanical_advantage = 11.0f/2.0f;

    /*    Variable initialization for Serial communication*/
    const int RX_BUFFER_SIZE = 16;
    char rxBuffer[RX_BUFFER_SIZE] = {}; // buffer to receive actuation torque
    int rxBufferIndex = 0;

    // watchdog counter and limit to disable torque
    int torqueWatchDog = 0;
    const int TORQUE_WATCHDOG_LIMIT = 10;

    //bicycle state struct
    Sample sample;
    float prevDelta = 0.0f;
    int sampleCount = 0;
    ButterLowpass deltaFilter = ButterLowpass();
    ButterLowpass deltaDotFilter = ButterLowpass();

    /*    Declare objects */
    Adafruit_MCP4725 dac;    // The Digital to Analog converter attached via i2c
} // namespace

float valToDelta(int val){ // bits to rad
    float myDelta = DEGTORAD*(SLOPE_DELTA * val + C_DELTA);
    return myDelta;
}

float valToDeltaDot(int val){ // bits to rad/s
    float myDelta = DEGTORAD*(SLOPE_DELTADOT * val + C_DELTADOT);
    return myDelta;
}

int torqueToDigitalOut (float torque) {
    const int pwm_zero_offset = 2048;
    const int max_pwm_int = 2048;
    float act_torque = torque / gearwheel_mechanical_advantage;
    // pwm needs to be negated, likely due to wiring
    int pwm = static_cast<int>(
            -max_pwm_int*act_torque/maxon_346970_max_torque_peak);
    return pwm + pwm_zero_offset;
}

void readSensors() {
    if (++txCount > SERIAL_TX_PRE - 1) {
        sample.delta = deltaFilter.filter(valToDelta(analogRead(DELTAPIN)));
        sample.deltaDot = (sample.delta - prevDelta)*SAMPLING_FREQ/SERIAL_TX_PRE;
        prevDelta = sample.delta;
        // directly call USB send function, blocking
        USB_Send(CDC_TX, &sample, sizeof(sample));
        txCount = 0;
    } else {
        sample.delta = deltaFilter.filter(valToDelta(analogRead(DELTAPIN)));
    }

}

void readTorque() { // read and apply torque from serial input
    // read all bytes available into receive buffer
    // USB_Recv takes the minimum of input argument 'len' and size of receive
    // FIFO so we simply provide the largest value we can fit in the buffer.
    int bytes_read;
    // As we use a circular buffer, if rxBufferIndex is at the end of the
    // buffer, provide the beginning of the buffer and RX_BUFFER_SIZE as the
    // length.
    if (rxBufferIndex < (RX_BUFFER_SIZE - 1)) {
        bytes_read = USB_Recv(CDC_RX, &rxBuffer[rxBufferIndex],
                RX_BUFFER_SIZE - rxBufferIndex);
    } else {
        bytes_read = USB_Recv(CDC_RX, &rxBuffer[0], RX_BUFFER_SIZE);
    }

    for (int i = 0; i < bytes_read; ++i) {
        if (rxBuffer[rxBufferIndex++] == SERIAL_SUFFIX_CHAR) {
            rxBufferIndex %= RX_BUFFER_SIZE;
            int prefixIndex = (rxBufferIndex - 2 - sizeof(float) +
                    RX_BUFFER_SIZE) % RX_BUFFER_SIZE;
            if (rxBuffer[prefixIndex++] == SERIAL_PREFIX_CHAR) {
                float torque;
                memcpy(&torque, &rxBuffer[prefixIndex % RX_BUFFER_SIZE],
                        sizeof(float));
                writeHandleBarTorque(torque);
            }
        }
        rxBufferIndex %= RX_BUFFER_SIZE;
    }
}

void configSampleTimer () {
    // NOTE: interrupts must be disabled when configuring timers
    TCNT3 = 0; // set counter to zero

    // Timer Control Register A/B
    //   Waveform Generation Mode - 0100: Clear Time on Compare match (CTC)
    //   Input Capture Edge Select - 1: rising edge trigger
    //   Clock Select - 010: clk/8 (from prescaler)
    TCCR3A = 0;
    TCCR3B = ((1 << CS31)|(1 << WGM32));

    // Interupt Mask Register - Interrupt Enable:
    //   Output Compare A Match
    TIMSK3 = (1 << OCIE3A); // enable timer compare interrupts channel A on timer 1.

    // Output Compare Register
    //   16 MHz/PRESCALER/SAMPLING_FREQ
    OCR3A = 16000000/(8*SAMPLING_FREQ) - 1;
}

void configCadenceTimer () {
    // NOTE: interrupts must be disabled when configuring timers
    TCNT1 = 0; // set counter to zero

    // Timer Control Register A/B
    //   Waveform Generation Mode - 0000: Normal
    //   Input Capture Noise Canceler
    //   Input Capture Edge Select - 1: rising edge trigger
    //   Clock Select - 101: clk/1024 (from prescaler)
    TCCR1A = 0;
    TCCR1B = ((1 << ICNC1) | (1 << ICES1) | (1 << CS12) | (1 << CS10));

    // Interupt Mask Register - Interrupt Enable:
    //   Input Capture, Overflow
    //TIMSK1 = (1 << ICIE1) | (1 << TOIE1);
}

void writeHandleBarTorque (float t) {
    int val = constrain(torqueToDigitalOut(t), 0, 4095);
    dac.setVoltage(val, false); // set the torque. Flag when DAC not connected.
    if (t != 0.0f) {
        torqueWatchDog = TORQUE_WATCHDOG_LIMIT;
    }
}

//void brakeSignalchangeISR () {
//    // Brake signal ISR handler which is called on pin change.
//    //Get the brake level by reading the pin:
//    ./brakeState = !digitalRead(BRAKEINPUTPIN);
//}


//FIXME cadence calculations
//ISR(TIMER1_CAPT_vect) {
//    // using timer 1 configured prescaler
//    const uint16_t t1_freq = 16000000/1024; // clk/sec
//    float c = 60.0f * t1_freq / ICR1; // rev/min
//
//    // if calculated cadence is reasonable, set value and reset counter
//    if (c < 200.0f) {
//        cadence = c;
//        TCNT1 = 0;
//    }
//}

//ISR(TIMER1_OVF_vect) {
//    cadence = 0.0f;
//}

ISR(TIMER3_COMPA_vect) {
    readSensors();
}

void setup() {
    // configure input/output pins
    pinMode(CADENCEPIN, INPUT_PULLUP); // should be input capture pin timer 1
    pinMode(BRAKEINPUTPIN, INPUT_PULLUP);
    pinMode(MCENABLEPIN, OUTPUT);

    // enable motor controller
    digitalWrite(MCENABLEPIN, HIGH);

    // Attach the interrupts and configure timers
    noInterrupts();
    //attachInterrupt(BRAKEHANDLE_INT, brakeSignalchangeISR, CHANGE);
    configSampleTimer(); // sample and serial transmission timer
    configCadenceTimer();
    interrupts();

    // For Adafruit MCP4725A1 the address is 0x62 (default) or 0x63 (ADDR pin tied to VCC)
    dac.begin(0x62);
    // Set the initial value of 2.5 volt. Flag when done. (0 Nm)
    dac.setVoltage(2048, true);

    sample.prefix = SERIAL_PREFIX_CHAR;
    sample.suffix = SERIAL_SUFFIX_CHAR;
    Serial.begin(2000000);
    while (!Serial); // wait for Serial to connect. Needed for Leonardo only.
}

void loop() {
    delay(10);
    // Check if incoming serial commands are available and process them
    readTorque();
    if ((torqueWatchDog > 0) && (--torqueWatchDog == 0)) {
        writeHandleBarTorque(0);
    }
}
