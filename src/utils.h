#include <Arduino.h>
#include <avr/interrupt.h>
#include "assembly.h"

#include "ticks_per_seconds.h"

#define PERIOD(freq_in_Hz_ints) freq_in_Hz_ints/TICK_FREQUENCY
#define DELAY_TO_TICKS(d) (uint16_t)(d*((double)Hz_1k)/((double)TICK_FREQUENCY))


/****************** TYPE DEFENITIONS *************/
typedef struct {
    volatile uint8_t*   stack_ptr;                  // Pointer to where pxCurrentTCB should be when switching context
    uint16_t            stack_size;                 // Size of the allocated stack in bytes
    uint8_t*            stack_array_ptr;            // Pointer to the task specific stack
    void                (*func)(void);              // Pointer to task function to execute.
    uint16_t            delay;                      
    const uint8_t       priority;                   // Priority for fixed-priority scheduling
    uint8_t             state;                     // Status for scheduling.
    const uint16_t      period;                     // Number of ticks between activations
} Task;

enum state {
    TASK_READY,     // Ready to be executed 
    TASK_RUNNING,   // Currently executing on the processorTASK_DONE
    TASK_WAITING,   // Task is waiting for a resource to be unlocked, like a mutex
    TASK_DONE,      // Task has completed is job. Shifts to TASK_READY in the next activation period
    TASK_DEAD       // One-shot tasks that shall not run again
};
/****************** FUNCTION DECLARATIONS *************/
void vPortYieldFromTick( void ) __attribute__ ( ( naked ) );
void Sched_Init( void );
void Sched_Dispatch( void );
uint8_t *pxPortInitialiseStack( uint8_t* pxTopOfStack, void (*pxCode)(), void *pvParameters );


/****************** GLOBAL VARIABLES *************/
Task* tasks[MAX_TASKS+1] = {0};     // list of tasks
uint8_t task_count = 0;             // number of tasks registed
int current_task = 0;                       //currently executing task
bool from_suspension = false;
volatile void* volatile pxCurrentTCB = 0;
#define finish_task()               from_suspension = true; tasks[current_task]->state = TASK_DONE; vPortYieldFromTick(); 



uint8_t *pxPortInitialiseStack( uint8_t* pxTopOfStack, void (*pxCode)(), void *pvParameters ) {
    uint16_t usAddress;
    /* Simulate how the stack would look after a call to vPortYield() generated by
    the compiler. */

    /* The start of the task code will be popped off the stack last, so place
    it on first. */
    usAddress = ( uint16_t ) pxCode;
    *pxTopOfStack = ( uint8_t ) ( usAddress & ( uint16_t ) 0x00ff );
    pxTopOfStack--;

    usAddress >>= 8;
    *pxTopOfStack = ( uint8_t ) ( usAddress & ( uint16_t ) 0x00ff );
    pxTopOfStack--;

    /* Next simulate the stack as if after a call to portSAVE_CONTEXT().
    portSAVE_CONTEXT places the flags on the stack immediately after r0
    to ensure the interrupts get disabled as soon as possible, and so ensuring
    the stack use is minimal should a context switch interrupt occur. */
    *pxTopOfStack = ( uint8_t ) 0x00;    /* R0 */
    pxTopOfStack--;
    *pxTopOfStack = ( (uint8_t) 0x80 );
    pxTopOfStack--;

    /* Now the remaining registers. The compiler expects R1 to be 0. */
    *pxTopOfStack = ( uint8_t ) 0x00;    /* R1 */

    /* Leave R2 - R23 untouched */
    pxTopOfStack -= 23;

    /* Place the parameter on the stack in the expected location. */
    usAddress = ( uint16_t ) pvParameters;
    *pxTopOfStack = ( uint8_t ) ( usAddress & ( uint16_t ) 0x00ff );
    pxTopOfStack--;

    usAddress >>= 8;
    *pxTopOfStack = ( uint8_t ) ( usAddress & ( uint16_t ) 0x00ff );

    /* Leave register R26 - R31 untouched */
    pxTopOfStack -= 7;

    return pxTopOfStack;
}



#define TASK(name, pr, fr, initial_delay, stack_sz, task) \
 uint8_t name##_stack[stack_sz]; \
 Task name = { \
    .func = task , \
    .stack_ptr = 0, \
    .stack_size = stack_sz, \
    .delay = DELAY_TO_TICKS(initial_delay), \
    .priority = pr, \
    .state = TASK_DONE, \
    .period = PERIOD(fr), \
 }; 


uint8_t addTask(Task* task, uint8_t* stack_pointer) {
    task->stack_array_ptr = stack_pointer;
    task->stack_ptr = pxPortInitialiseStack(stack_pointer+task->stack_size, task->func, 0);
    tasks[task_count] = task;
    task_count++;
    return task_count - 1;
}


void hardwareInit(){
    noInterrupts();  // disable all interrupts

    TCCR1A = 0;
    TCCR1B = 0;
    TCNT1 = 0;
    
    OCR1A = TICK_FREQUENCY;
    TCCR1B |= (1 << WGM12);     // CTC mode
    TCCR1B |= (1 << CS12);      // 256 prescaler
    TIMSK1 |= (1 << OCIE1A);    // enable timer compare interrupt

    interrupts();  // enable all interrupts
}



ISR(TIMER1_COMPA_vect, ISR_NAKED) {
    /* Call the tick function. */
    vPortYieldFromTick();
    /* Return from the interrupt. If a context
    switch has occurred this will return to a
    different task. */
    asm volatile ( "reti" );
}

void vPortYieldFromTick( void ) {
    // This is a naked function so the context
    // must be saved manually. 
    portSAVE_CONTEXT();

    // Increment the tick count and check to see
    // if the new tick value has caused a delay
    // period to expire. This function call can
    // cause a task to become ready to run.
    if (!from_suspension)
        Sched_Init();
    from_suspension = false;
    
    // See if a context switch is required.
    // Switch to the context of a task made ready
    // to run by Sched_Init() if it has a
    // priority higher than the interrupted task.
    Sched_Dispatch();
    
    // Restore the context. If a context switch
    // has occurred this will restore the context of
    // the task being resumed.
    portRESTORE_CONTEXT();
    
    // Return from this naked function.
    asm volatile ( "ret" );
}

void Sched_Init() {
    
    for (int i = 0; i < task_count; i++) {
        if (tasks[i] && tasks[i]->state != TASK_DEAD) {
            if (!tasks[i]->delay) {
                tasks[i]->state = TASK_READY;
                tasks[i]->delay = tasks[i]->period;
            }
            else {
                (tasks[i]->delay)--;
            }
        }
    }
    
    return;
}

void Sched_Dispatch() { 

    if(tasks[current_task]->state == TASK_RUNNING)
        tasks[current_task]->state = TASK_WAITING;


    // find the highest priority task which is ready (i.e., task->priority is lowest)

    uint8_t exec_task = 0;
    uint8_t task_prio = 255;
    for(uint8_t i = 0; i < task_count; i++){
        if(tasks[i] && tasks[i]->priority <= task_prio && (tasks[i]->state == TASK_READY || tasks[i]->state == TASK_WAITING) ) {
            exec_task = i;
            task_prio = tasks[i]->priority;
        }
    }

    current_task = exec_task;
    tasks[current_task]->state = TASK_RUNNING;
    pxCurrentTCB = &tasks[current_task]->stack_ptr;
    
    return;
}

