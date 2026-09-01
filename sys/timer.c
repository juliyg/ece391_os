// timer.c - Timer and Alarms
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "process.h"
#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "sbi.h" // for sbi_set_timer
#include "intr.h"
#include "misc.h"
#include "string.h"
#include "console.h"
#include <stddef.h>
#include <limits.h> // for ULLONG_MAX

// COMPILE-TIME OPTIONS
//

#ifndef BOLT_FREQ
#define BOLT_FREQ 50 // Hz [MP3cp3]
#endif

#ifndef MLFQ_BOOST_INTERVAL_TICKS 
#define MLFQ_BOOST_INTERVAL_TICKS 100
#endif

// INTERNAL TYPE DEFINITIONS
//

struct timer_alarm {
    unsigned long long twake; 
    struct condition woken; 
    struct timer_alarm *next; 
};

// EXPORTED GLOBAL VARIABLES
//

char timer_initialized = 0;
unsigned int timer_frequency = 0;
unsigned int bolt_period = 0;

// INTERNAL GLOBAL VARIABLES
//

static unsigned int mlfq_boost_tick_count; 
static struct timer_alarm * sleep_list; // list of pending alarms


// INTERNAL FUNCTION DECLARATIONS
//


static void enable_timer_interrupts(void); // sets sie.STIE=1
static void disable_timer_interrupts(void); // clears sie.STIE


// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(unsigned int freq) {
    enable_timer_interrupts(); 
    timer_initialized = 1; 
    timer_frequency = freq; 
    bolt_period = timer_frequency / BOLT_FREQ;
    mlfq_boost_tick_count = rdtime() / bolt_period; 
    sbi_set_timer(0); 
}


void sleep_until(unsigned long long twake) {
    if (rdtime() >= twake){
        return;
    }
    
    int pie = disable_interrupts();
    struct timer_alarm * prev_iter;
    struct timer_alarm * cur_iter;

    struct timer_alarm cur_alarm;
    condition_init(&cur_alarm.woken, "alarm_cond");
    cur_alarm.twake = twake;
    cur_alarm.next = NULL;

    if (sleep_list == NULL || twake < sleep_list->twake) { //check case where head is null
        cur_alarm.next = sleep_list;
        sleep_list = &cur_alarm;
    } else {
        prev_iter = sleep_list;
        cur_iter = sleep_list->next;
    
        while (cur_iter != NULL && cur_iter->twake < twake) { // get prev and cur of the given element by order
            prev_iter = cur_iter;
            cur_iter = cur_iter->next;

        }

        cur_alarm.next = cur_iter;
        prev_iter->next = &cur_alarm;
    }

    sbi_set_timer(sleep_list->twake);
    trace("%s", __func__); 

    // must enable timer interrupts after condition otherwise may sleep forever
    condition_wait(&cur_alarm.woken); 
    restore_interrupts(pie);
}

void sleep_sec(unsigned int sec) {
    sleep_until(rdtime() + 1ULL * sec * timer_frequency);
}

void sleep_ms(unsigned int ms) {
    sleep_until(rdtime() + 1ULL * ms * timer_frequency / 1000);
}

void sleep_us(unsigned int us) {
    sleep_until(rdtime() + 1ULL * us * timer_frequency / 1000 / 1000);
}

void handle_timer_interrupt(void) {

    unsigned long long tnow = rdtime();

    trace("[%lu] %s()", tnow, __func__);
    
    // keep popping from the front until sleep_list is empty or twake > tnow
    while(sleep_list && sleep_list->twake <= tnow) {
        condition_broadcast(&sleep_list->woken);
        trace("%s", __func__); 
        sleep_list = sleep_list->next; 
        trace("%s", __func__); 
    }

    // NULL case for linked list if alarm is null
    unsigned long long t_next;
    if(sleep_list != NULL) {
        t_next = sleep_list->twake; 
    }
    else{
        t_next = ULLONG_MAX;
    }


    unsigned int current_tick = tnow / bolt_period;
    if(current_tick - mlfq_boost_tick_count > MLFQ_BOOST_INTERVAL_TICKS) {
        mlfq_boost_tick_count = current_tick;
        mlfq_boost_priority(); 
    }

    unsigned long long tbolt = (unsigned long long ) (current_tick + 1) * bolt_period;
    sbi_set_timer(MIN(t_next, tbolt)); 
    trace("%s", __func__); 
}

unsigned int current_timer_tick(void) {
    return rdtime() / bolt_period; 
}

// INTERNAL FUNCTION DEFINITIONS
//

void enable_timer_interrupts(void) {
    csrs_sie(RISCV_SIE_STIE);
}

void disable_timer_interrupts(void) {
    csrc_sie(RISCV_SIE_STIE);
}