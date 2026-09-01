// timer.c - Timer and Alarms
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

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

#include <stddef.h>
#include <limits.h> // for ULLONG_MAX

// COMPILE-TIME OPTIONS
//

#ifndef BOLT_FREQ
#define BOLT_FREQ 50 // Hz [MP3cp3]
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


// INTERNAL GLOBAL VARIABLES
//

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
    sbi_set_timer(0); 
}


void sleep_until(unsigned long long twake) {
    struct timer_alarm alarm; 
    alarm.twake = twake; 
    condition_init(&alarm.woken, "alarm condition");
    alarm.next = NULL; 

    // check if the list is empty
    int pie = disable_interrupts();
    struct timer_alarm *curr = sleep_list; 
    if(!curr) {
        sleep_list = &alarm; 
    } 

    // if the list is not empty, find the spot to insert the alarm
    struct timer_alarm *prev = curr; 
    while(curr) {
        if(alarm.twake < curr->twake) {
            break;
        }
        prev = curr; 
        curr = curr->next; 
    }

    // case 1: there is one node, and is greater than alarm.twake
    // case 2: else 
    if(prev == curr) {
        sleep_list = &alarm; 
    } else {
        alarm.next = curr; 
        prev->next = &alarm; 
    }

    sbi_set_timer(sleep_list->twake); 
    trace("%s", __func__); 

    // must enable timer interrupts after condition otherwise may sleep forever
    condition_wait(&alarm.woken); 
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
    if(!sleep_list) {
        sbi_set_timer(ULLONG_MAX); 
        return; 
    }
    sbi_set_timer(sleep_list->twake); 
    trace("%s", __func__); 
}

// INTERNAL FUNCTION DEFINITIONS
//


void enable_timer_interrupts(void) {
    csrs_sie(RISCV_SIE_STIE);
}

void disable_timer_interrupts(void) {
    csrc_sie(RISCV_SIE_STIE);
}