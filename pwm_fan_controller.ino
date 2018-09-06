/*
    Fan Controller LCD
    Author: Dustin Westaby
    Date: September 3rd, 2018

  ################################
  # onoff_state = SYSTEM_OFF     #
  ################################
  Pressing on/off will:
   * Switch the relay line high to send power to the fan (12V mosfet)
   * Set the throttle to [last selected]

  ################################
  # onoff_state = SYSTEM_ON      #
  ################################
  Pressing on/off will:
   * Switch the relay line low to remove power from the fan (12V mosfet)
   * Set the throttle to OFF
  Use the +/- buttons to go up and down in 10% chunks
  HOLDING (3 second count) on/off will set throttle to MAX_FAN_SPEED

  When max speed is reached:
  * Do not increase the pwm farther.
  * Turn off PWM and ground the pin.
  * This will set the fan to it's true max speed.

  ################################
  TO DO:
  * When turned off, then on.  Reset the fan to 20%
  * Holding on/off sets MAX_FAN_SPEED, but the observed fan speed is slowed then speeds up

  ################################
  CHANGES:
  * FIXED: Tach reading output is clipping.
  * FIXED: Up and Down buttons are intermittent
  * FIXED: Debounced button presses
  * FIXED: Turbo instructions are displayed
  * FIXED: Fan speed correction when PWM is set to OFF

*/

#include <LiquidCrystal.h> //https://www.arduino.cc/en/Reference/LiquidCrystal
#include "PWM.h" //https://github.com/terryjmyers/PWM

/***********************/
/* FINE TUNING NUMBERS */
/***********************/
#define VERSION_NUMBER ".11"

#define MIN_FAN_DUTYCYCLE_ANALOG 155 //this is the pwm range for the bargraph 1-9 (MAX_PWM_SPEED)
#define MAX_FAN_DUTYCYCLE_ANALOG 255

#define BUTTON_HOLD_FOR_TURBO_DELAY_IN_MILLIS 2000

#define BOOTSCREEN_DISPLAY_TIME_IN_MILLIS 2000 //press onoff to punch past the boot screen early

#define TURBO_TEXT_DISPLAY_TIME_IN_MILLIS 500

                                // decrease if buttons become unresponsive to fast clicks
#define DEBOUNCE_DELAY_MILLS 40 // increase if the display flickers when buttons are pressed

#define TACHOMETER_SAMPLING_TIMOUT_MICROSECONDS 40

#define TACHOMETER_SCALOR 5.7        //            <--------------- If RPM is inaccurate, adjust the scalor

//Set display and fan updates to be less frequent, giving cpu priority to button interactions
#define DISPLAY_REFRESH_RATE_MILLIS 100 //100 millis = 10Hz refresh.
#define FAN_REFRESH_RATE_MILLIS     10 //10 millis = 100Hz refresh.
#define BUTTON_REFRESH_RATE_MILLIS  1  // 1 millis = 1kHz refresh.

/*******************************/
/* CIRCUIT WIRING TO CHIP PINS */
/*******************************/
//                RS EN D4  D5  D6  D7
LiquidCrystal lcd(7,  8, 9, 10, 11, 12);

#define FAN_PULSE_READ_PIN  4
#define FAN_CONTROL_PIN     3

#define BUTTON_SPEED_UP_PIN     1
#define BUTTON_SPEED_DOWN_PIN   2
#define BUTTON_ONOFF_TOGGLE_PIN 0 //Holding will set throttle to 100%

#define RELAY_CONTROL_PIN   7
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

enum
{
  IS_NOT_PRESSED,
  IS_PRESSED,
  WAS_RELEASED,
  IS_HELD,
  PROCESSED,
  NOT_PROCESSED
};

/***********************/
/*   BUTTON HANDLING   */
/***********************/
byte button_pins[3]  = { BUTTON_SPEED_UP_PIN, BUTTON_SPEED_DOWN_PIN, BUTTON_ONOFF_TOGGLE_PIN };
byte button_state[3] = { IS_NOT_PRESSED,      IS_NOT_PRESSED,        IS_NOT_PRESSED };

//order of above arrays
#define SPEED_UP_BUTTON     0
#define SPEED_DOWN_BUTTON   1
#define ONOFF_TOGGLE_BUTTON 2
#define NUMBER_OF_BUTTONS   3

enum
{
  SYSTEM_ON = 0,
  SYSTEM_OFF,
  TURBO_REQUESTED,
  TURBO_ON,  //tbd, not used
};

byte onoff_state = SYSTEM_ON;
byte turbo_requested_by_user = false;

/***********************/
/*    FAN HANDLING     */
/***********************/
#define OFF_FAN_SPEED 0
#define MIN_FAN_SPEED 1
#define MAX_PWM_SPEED 9
#define MAX_FAN_SPEED 10
#define INCREMENT_FAN_SPEED 1

#define DEFAULT_FAN_SPEED (MIN_FAN_SPEED + INCREMENT_FAN_SPEED) //This is 20% or ## on the bargraph

byte user_fan_speed = DEFAULT_FAN_SPEED;

#define FAN_PWM_FREQUENCY 25000

/* --------------- */
/* Setup Functions */
/* --------------- */

void setup()
{
    for(byte i; i < NUMBER_OF_BUTTONS; i++)
    {
        //enable internal pullups
        pinMode(button_pins[i], INPUT_PULLUP);
    }

    pinMode(RELAY_CONTROL_PIN, OUTPUT);
    digitalWrite(RELAY_CONTROL_PIN,RELAY_OFF);

    lcd.begin(16, 2);

    //enable internal pullups
    pinMode(FAN_PULSE_READ_PIN, INPUT);
    digitalWrite(FAN_PULSE_READ_PIN,HIGH);

    SetPinFrequencySafe(FAN_CONTROL_PIN, FAN_PWM_FREQUENCY);

    onoff_state = SYSTEM_OFF;

    show_boot_screen();

}

/* -------------------------- */
/* Man Scheduling Functions   */
/* -------------------------- */

void loop()
{
    process_button_schedule();

    process_fan_schedule();

    process_display_schedule();
}

void process_button_schedule()
{
    static unsigned long last_buttons_update = millis();

   // if(BUTTON_REFRESH_RATE_MILLIS + last_buttons_update < millis())
   // {
        last_buttons_update = millis();

        poll_input_signals();

        process_buttons();

        process_turbo_request();
   // }
}

void process_fan_schedule()
{
    static unsigned long last_fan_update = millis();

    if(FAN_REFRESH_RATE_MILLIS + last_fan_update < millis())
    {
        last_fan_update = millis();

        process_fan_relay();

        process_fan_speed();
    }

}


void process_display_schedule()
{
    static unsigned long last_display_update = millis();

    if(DISPLAY_REFRESH_RATE_MILLIS + last_display_update < millis())
    {
        last_display_update = millis();

        process_display_updates();
    }

}

/* ------------------- */
/* Button Functions    */
/* ------------------- */

void poll_input_signals()
{
    static byte last_button_read[] = {false, false, false};
    static byte current_button_read[] = {false, false, false};

    for(byte i; i < NUMBER_OF_BUTTONS; i++)
    {
        /* read digital active low button signal */
        current_button_read[i] = !digitalRead(button_pins[i]);

        if (!last_button_read[i] && current_button_read[i])
        {
            //button was just pressed
            button_state[i] = IS_PRESSED;
        }
        else if  (last_button_read[i] && current_button_read[i])
        {
            button_state[i] = IS_HELD;
        }
        else if (last_button_read[i] && !current_button_read[i])
        {
            //button was just released
            button_state[i] = WAS_RELEASED;
        }
        else
        {
            button_state[i] = IS_NOT_PRESSED;
        }

        last_button_read[i] = current_button_read[i];
    }
}

void process_buttons()
{
    static unsigned long last_interaction_timestamp[NUMBER_OF_BUTTONS] = {millis(), millis(), millis()};
    static byte last_button_state[NUMBER_OF_BUTTONS] = {NOT_PROCESSED, NOT_PROCESSED, NOT_PROCESSED};

    for(byte i; i < NUMBER_OF_BUTTONS; i++)
    {
        if ( ( button_state[i] == IS_HELD ) &&
             ( last_interaction_timestamp[i] + BUTTON_HOLD_FOR_TURBO_DELAY_IN_MILLIS < millis() ) )
        {
            //HELD ACTION
            //Button was held for a duration greater than BUTTON_HOLD_FOR_TURBO_DELAY_IN_MILLIS

            last_interaction_timestamp[i] = millis();

            if(last_button_state[i] == NOT_PROCESSED)
            {
                last_button_state[i] = PROCESSED;

                process_button_hold(i);
            }
        }
        else if ( ( button_state[i] == IS_NOT_PRESSED ) &&
                  ( last_interaction_timestamp[i] + DEBOUNCE_DELAY_MILLS < millis() ) )
        {
            //CLICKED ACTION
            //Button was held for a duration greater than DEBOUNCE_DELAY_MILLS

            last_interaction_timestamp[i] = millis();

            if(last_button_state[i] == NOT_PROCESSED)
            {
                last_button_state[i] = PROCESSED;

                process_button_click(i);
            }
        }
        else if (button_state[i] == IS_NOT_PRESSED)
        {
            //reset timers, one action per press / hold
            last_interaction_timestamp[i] = millis();
            last_button_state[i] = NOT_PROCESSED;
        }
    }
}

void process_button_click(byte button_clicked)
{
    switch (button_clicked)
    {
        case ONOFF_TOGGLE_BUTTON:
            process_onoff_click();
            break;
        case SPEED_UP_BUTTON:
            process_speed_up_click();
            break;
        case SPEED_DOWN_BUTTON:
            process_speed_down_click();
            break;
        default:
            //do nothing
            break;
    }
}

void process_button_hold(byte button_held)
{
    switch (button_held)
    {
        case ONOFF_TOGGLE_BUTTON:
            process_onoff_hold();
            break;
        case SPEED_UP_BUTTON:
        case SPEED_DOWN_BUTTON:
        default:
            //do nothing
            break;
    }
}

void process_onoff_click()
{
    static byte save_fan_speed = user_fan_speed;

    //toggle on off state
    if( onoff_state != SYSTEM_OFF )
    {
        onoff_state = SYSTEM_OFF;

        //save fan speed and turn off
        save_fan_speed = user_fan_speed;
        user_fan_speed = OFF_FAN_SPEED;
    }
    else //(onoff_state == SYSTEM_OFF)
    {
        onoff_state = SYSTEM_ON;

        //restore fan to previous speed
        user_fan_speed = save_fan_speed;
    }
}

void process_onoff_hold()
{
    if(onoff_state != SYSTEM_OFF)
    {
        set_max_fan_speed();
    }
    //else, do nothing
}

void process_speed_up_click()
{
    if( onoff_state != SYSTEM_OFF )
    {
        speed_up_fan_speed();
    }
    //else, do nothing
}

void process_speed_down_click()
{
    if( onoff_state != SYSTEM_OFF )
    {
        slow_down_fan_speed();
    }
    //else, do nothing
}

/* --------------------- */
/* Fan Control Functions */
/* --------------------- */

void process_fan_relay()
{
    if( onoff_state != SYSTEM_OFF )
    {
        digitalWrite(RELAY_CONTROL_PIN,RELAY_ON);
    }
    else //(onoff_state == SYSTEM_OFF)
    {
        digitalWrite(RELAY_CONTROL_PIN,RELAY_OFF);
    }
}

void set_max_fan_speed()
{
    user_fan_speed = MAX_FAN_SPEED;
}

void speed_up_fan_speed()
{
    if(user_fan_speed == OFF_FAN_SPEED) //trap in case default was broken, we should always be on when speed buttons are processed
    {
        user_fan_speed = MIN_FAN_SPEED;
    }
    else if(user_fan_speed < (MAX_FAN_SPEED - INCREMENT_FAN_SPEED))
    {
        user_fan_speed += INCREMENT_FAN_SPEED;
    }
    else
    {
      turbo_requested_by_user = true;
    }
}

void slow_down_fan_speed()
{
    if(user_fan_speed >= (MIN_FAN_SPEED + INCREMENT_FAN_SPEED))
    {
        user_fan_speed -= INCREMENT_FAN_SPEED;
    }
}

void process_fan_speed()
{
    if( onoff_state != SYSTEM_OFF )
    {
        if (user_fan_speed == MAX_FAN_SPEED)
        {
            //turbo mode
            set_fan_speed(OFF_FAN_SPEED);
            digitalWrite(FAN_CONTROL_PIN,LOW);
        }
        else
        {
            set_fan_speed(user_fan_speed);
        }
    }
    else
    {
        set_fan_speed(OFF_FAN_SPEED);
    }
}

void set_fan_speed(byte requested_fan_speed)
{
  byte target;

  if(requested_fan_speed == OFF_FAN_SPEED)
  {
    //passthrough the speed selection
    analogWrite(FAN_CONTROL_PIN, OFF_FAN_SPEED);
  }
  else
  {
    //translate the speed selection to a pwm value
    target = map(requested_fan_speed, MIN_FAN_SPEED, MAX_PWM_SPEED, MIN_FAN_DUTYCYCLE_ANALOG, MAX_FAN_DUTYCYCLE_ANALOG);
    analogWrite(FAN_CONTROL_PIN, target);
  }
}

unsigned int read_fan_rpm_from_tachometer_average()
{
    unsigned long pulseDuration = 0;
    double frequency = 0;
    unsigned long rpm = 0;

    if( onoff_state != SYSTEM_OFF )
    {
      //ALTERNATIVE 1: convert microseconds to RPM
      pulseDuration = pulseIn(FAN_PULSE_READ_PIN, LOW, TACHOMETER_SAMPLING_TIMOUT_MICROSECONDS);
      frequency = 1000000/pulseDuration;
      rpm = frequency/TACHOMETER_SCALOR*60; //RPM
    }
    // else, rpm = 0

    return rpm;
}

void process_turbo_request()
{
  static unsigned long turbo_request_displayed_timer = millis();
  byte turbo_request_exit = false;

  if (user_fan_speed != MAX_FAN_SPEED)
  {
    if(turbo_requested_by_user == true)
    {
      onoff_state = TURBO_REQUESTED;
      turbo_request_displayed_timer = millis();
    }
  }

  if(onoff_state == TURBO_REQUESTED)
  {
    if(TURBO_TEXT_DISPLAY_TIME_IN_MILLIS + turbo_request_displayed_timer < millis())
    {
      turbo_request_exit = true;
    }

    if (user_fan_speed == MAX_FAN_SPEED)
    {
      turbo_request_exit = true;
    }
  }

  if (turbo_request_exit == true)
  {
    onoff_state = SYSTEM_ON;
    lcd.clear();
  }

   turbo_requested_by_user = false;
}

/* ------------------- */
/* Animation Functions */
/* ------------------- */

void show_boot_screen()
{
    long bootscreen_timer = millis();

    //  BOOT SCREEN
    //  +----------------+
    //  |PWM FAN         |
    //  |CONTROLLER VX.X |
    //  +----------------+

    lcd.setCursor(0,0);
    lcd.print("PWM FAN");
    lcd.setCursor(0,1);
    lcd.print("CONTROLLER V");
    lcd.print(VERSION_NUMBER);

    //allow pressing onoff button to punch through the boot screen
    while(bootscreen_timer + BOOTSCREEN_DISPLAY_TIME_IN_MILLIS > millis())
    {
       process_button_schedule();

       if(onoff_state != SYSTEM_OFF)
       {
        break;
       }
    }

    lcd.clear();
}

void process_display_updates()
{
    byte i,j;
    unsigned int read_back_rpms = read_fan_rpm_from_tachometer_average();

    switch (onoff_state)
    {
        case SYSTEM_ON:
            //   0123456789012345
            //  60% PWM              80% PWM              100% PWM             MAX FAN
            //  +----------------+   +----------------+   +----------------+   +----------------+
            //  |FAN SPEED: 1000 |   |FAN SPEED: 1900 |   |FAN SPEED: 2700 |   |FAN SPEED: 3000 |
            //  |LO #          HI|   |LO #####     HI |   |LO #########  HI|   |LO ########## HI|
            //  +----------------+   +----------------+   +----------------+   +----------------+

            lcd.setCursor(0,0);
            lcd.print("FAN SPEED: ");
            lcd.print(read_back_rpms);

            //Display a variable number of "#" characters based on the selected speed
            // Then fill the remaining display area with spaces
            lcd.setCursor(0,1);
            lcd.print("LO ");
            for(i=0; i<user_fan_speed; i++)
            {
              lcd.print("#");
            }
            for(j=i; j<MAX_FAN_SPEED; j++)
            {
              lcd.print(" ");
            }

            //0123456789012345
            //LO ########## HI
            lcd.setCursor(13,1);
            lcd.print(" HI");

            break;
        case SYSTEM_OFF:
            //  FAN OFF (STARTING STATE)
            //  +----------------+
            //  |FAN SPEED: 2800 |
            //  |OFF             |
            //  +----------------+

            lcd.setCursor(0,0);
            lcd.print("FAN SPEED: ");
            lcd.print(read_back_rpms);

            // 0123456789012345
            //"      OFF       "
            lcd.setCursor(0,1);
            lcd.print("      OFF       ");
            break;
        case TURBO_REQUESTED:
            //   0123456789012345
            //  +----------------+
            //  |   HOLD POWER   |
            //  |   FOR TURBO    |
            //  +----------------+

            lcd.setCursor(0,0);
            lcd.print("   HOLD POWER   ");
            lcd.setCursor(0,1);
            lcd.print("   FOR TURBO    ");
           break;
        case TURBO_ON:
        default:
            //do nothing
            break;
    }
}
