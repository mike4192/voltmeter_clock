/*
  Volt Meter Clock
  ----------------------

  Substantially based off of lcamtuf's original code and design, see:
  https://lcamtuf.substack.com/p/a-nicer-voltmeter-clock

  MCU: AVR32DB28 / AVR64DB28 / AVR128DB28

  Pinout: PC0, PC1, PC2 - PWM outputs to meters (other side to gnd)
          PA0, PA1      - 8 MHz crystal + 18 pF caps to gnd
          PD6, PD7      - time adjustment buttons (other side to gnd)

  The only other MCU connections are power supply pins and the UPDI
  programming header.

  Comapred to lcamtuf's original source code, the following general changes were made
  - PWM loop speed increased to 100 kHz to avoid audible hum
  - sigma-delta dithering approach to emulate higher PWM duty cycle step resolution as
    a consequence of faster loop speed
  - Smooth second hand fall back on minute roll-over, excepting hour changes
  - Some basic button press debouncing since the PWM control was moved into a 
    dedicated timer

  Michael Romanko

 */

#define F_CPU 8000000
#define PWM_TOP 79 /* 100 kHz at 8 MHz with prescaler /1 (PER+1 = 80) */
#define DUTY_SCALE 600
#define BUTTON_POLL_MS 20
#define FLYBACK_TICKS 650 /* 1 kHz ISR: ramp second hand to 0 in ~0.25 s */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/atomic.h>
/* User-friendly typedefs */
typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
/* Configure clock. External 8 MHz crystal on PA0, PA1 */
static void setup_clock() {
  CCP = 0xd8;                       /* Unlock register access   */
  CLKCTRL.XOSCHFCTRLA = 0b10000001; /* Enable external clock    */
  while (!(CLKCTRL.MCLKSTATUS & 0b10000))
    ;
  CCP = 0xd8;            /* Unlock register access   */
  CLKCTRL.MCLKCTRLA = 3; /* Switch to external clock */
  while (!(CLKCTRL.MCLKSTATUS & 1))
    ;
}
/* Configure ports. */
static void setup_ports() {
  PORTA.DIR = 0b11111111;
  PORTC.DIR = 0b11111111;
  PORTD.DIR = 0b00111111; /* PA6, PA7: buttons */
  PORTF.DIR = 0b11111111;
  /* Pull-up for buttons */
  PORTD.PIN6CTRL = 0b00001000;
  PORTD.PIN7CTRL = 0b00001000;
  /* Slew rate limit for PWM output */
  PORTC.PORTCTRL = 1;
}


/* Configure TCA0 for 3-channel hardware PWM at 100 kHz (no interrupts). */
static void setup_pwm() {
  /* Route WO0/WO1/WO2 to PC0/PC1/PC2 on AVR128DB28. */
  PORTMUX.TCAROUTEA = PORTMUX_TCA0_PORTC_gc;
  TCA0.SINGLE.CTRLA = 0; /* Disable while reconfiguring */
  TCA0.SINGLE.CTRLB = TCA_SINGLE_WGMODE_SINGLESLOPE_gc | TCA_SINGLE_CMP0EN_bm | TCA_SINGLE_CMP1EN_bm | TCA_SINGLE_CMP2EN_bm;
  TCA0.SINGLE.PER = PWM_TOP;
  TCA0.SINGLE.CMP0BUF = 0; /* Hour meter (PC0 / WO0)   */
  TCA0.SINGLE.CMP1BUF = 0; /* Minute meter (PC1 / WO1) */
  TCA0.SINGLE.CMP2BUF = 0; /* Second meter (PC2 / WO2) */
  TCA0.SINGLE.INTCTRL = 0;
  TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV1_gc | TCA_SINGLE_ENABLE_bm;
}


/* Configure TCB0 at 1 kHz; ISR divides by 100 to get 10 Hz.
   (TCB only offers CLK_PER, CLK_PER/2, or CLK_TCA — no /256.) */
static void setup_timebase() {
  TCB0.CTRLA = 0;
  TCB0.CTRLB = TCB_CNTMODE_INT_gc;
  TCB0.CCMP = 8000 - 1; /* 8 MHz / 8000 = 1 kHz */
  TCB0.INTCTRL = TCB_CAPT_bm;
  TCB0.CTRLA = TCB_CLKSEL_DIV1_gc | TCB_ENABLE_bm;
}

/* Update time, handling wrap-around. */
static volatile u8 cur_hr, cur_min; /* Hour (0-11) and minute (0-59)     */
static volatile u16 cur_secx10;     /* Tenth of a second counter (0-599) */
static volatile u16 second_display; /* Second meter position (flyback or cur_secx10) */
static volatile u16 flyback_rem;    /* Flyback ticks left; 0 = not active */
static volatile u8 tick_div;        /* 1 kHz -> 10 Hz divider            */
static volatile u16 ms_tick;        /* 1 kHz millisecond counter         */
ISR(TCB0_INT_vect) {

  ms_tick++;

  if (++tick_div >= 100) {

    tick_div = 0;
    cur_secx10++;

    if (cur_secx10 >= 600) {
      u8 hour_wrap = (cur_min == 59);
      cur_secx10 = 0;
      cur_min++;

      if (cur_min == 60) {
        cur_min = 0;
        cur_hr++;
        if (cur_hr == 12) cur_hr = 0;
      }

      /* Analog-style rewind on minute rollover; skip at hour rollover. */
      if (hour_wrap)
        flyback_rem = 0;
      else
        flyback_rem = FLYBACK_TICKS;
    }
  }

  if (flyback_rem) {
    second_display = (u32)((599UL * flyback_rem) / FLYBACK_TICKS);
    if (--flyback_rem == 0)
      second_display = cur_secx10;
  } else

    second_display = cur_secx10;

  TCB0.INTFLAGS = TCB_CAPT_bm; /* Acknowledge interrupt */
}


/* Map 0..599 logical units to 0..PWM_TOP with sigma-delta dithering.
   With only 80 PWM steps, plain integer division makes the second hand
   jump; the accumulator alternates between cmp and cmp+1 to hit the
   correct long-term average duty. */
static u16 sd_hr, sd_min, sd_sec;

static u16 duty_to_cmp(u16 adj, u16 *sd) {

  u32 num = (u32)adj * (PWM_TOP + 1);
  u16 cmp = num / DUTY_SCALE;
  u16 rem = num % DUTY_SCALE;

  *sd += rem;
  if (*sd >= DUTY_SCALE) {
    *sd -= DUTY_SCALE;
    cmp++;
  }

  if (cmp > PWM_TOP) cmp = PWM_TOP;
  return cmp;
}


/* Main entry point */
int main(void) {
  setup_clock();
  setup_ports();
  setup_pwm();
  setup_timebase();
  sei();
  /* Main loop computes meter targets and updates hardware PWM compare values. */
  u8 key_last = 0; /* Previous key state */
  u16 last_button_poll = 0;
  while (1) {
    /* Compute duty cycles. The minute gauge has divisions from 0 to 60. One minute corresponds
       to a duty cycle step of 10, so we multiply the minute counter accordingly, and then add
       another value between 0 and 9 based on the state of the second counter.
       For the hour gauge, we have divisions from 0 to 12, and one hour corresponds to an
       increment of 50. We multiply the hour counter by 50 and add another 0-49 depending on
       the minute counter.
       This is also where you can incorporate fudge factors if the meters aren't precise. */
    u8 hr;
    u8 min;
    u16 secx10;
    u16 sec_display;
    u32 adj_minx10, adj_hrx10, adj_secx10;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      hr = cur_hr;
      min = cur_min;
      secx10 = cur_secx10;
      sec_display = second_display;
    }

    /* Minute/hour use true time; second meter uses second_display (flyback). */
    adj_minx10 = (u32)(min * 10 + secx10 / 60) * 256 / 256;
    adj_hrx10 = (u32)(hr * 50 + adj_minx10 / 12) * 256 / 256;
    adj_secx10 = (u32)sec_display * 256 / 256;

    /* Scale from 0..599 logical units to 0..PWM_TOP compare values. */
    TCA0.SINGLE.CMP2BUF = duty_to_cmp(adj_secx10, &sd_sec);
    TCA0.SINGLE.CMP1BUF = duty_to_cmp(adj_minx10, &sd_min);
    TCA0.SINGLE.CMP0BUF = duty_to_cmp(adj_hrx10, &sd_hr);

    // Button detection once every BUTTON_POLL_MS
    u16 now;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      now = ms_tick;
    }
    if ((u16)(now - last_button_poll) < BUTTON_POLL_MS)
      continue;

    last_button_poll = now;

    u8 pd = (PORTD.IN >> 6) & 0b11;

    /* Both inputs are high: reset state and continue */
    if (pd == 0b11) {
      key_last = 0;
      continue;
    }

    /* At least one button pressed. If this is a continuation of a previous keypress, bail out. */
    if (key_last) { continue; }
    key_last = 1;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      /* Key 1 advances the minute dial while zeroing the seconds. */
      if (!(pd & 0b10)) {
        cur_secx10 = 0;
        second_display = 0;
        flyback_rem = 0;
        if (++cur_min == 60) cur_min = 0;
        sd_sec = 0;
        sd_min = 0;
      }

      /* Key 0 advances the hour dial without messing up any of the other dials. */
      if (!(pd & 0b01)) {
        if (++cur_hr == 12) cur_hr = 0;
        sd_hr = 0;
      }
    }
  }
}
