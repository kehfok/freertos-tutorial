/*
Author: Kevin Fok
Date: 03/10/24

1. Use hardware timer to sample from ADC at 10Hz and add value to circular buffer.
2. After 10 samples have been added to the circular buffer, have a task (task A) wake up and compute the average.
Store the value in a global variable (float).
3. Create another task (task B) to manage the Serial interface. When the user types any character, echo that character
back to Serial. If the user types the command "avg", print out the average value.
*/

#include <Arduino.h>

// Use only core 1 for demo purposes
#if CONFIG_FREERTOS_UNICORE
    static const BaseType_t app_cpu = 0;
#else
    static const BaseType_t app_cpu = 1;
#endif

typedef struct
{
    uint32_t* const arr; // pointer is constant, but buffer is not
    int head; // pointer to head of circular buff
    int tail; // pointer to tail of circular buff
    const int max_len;
} circ_bbuf_t;

// macro for declaring and initializing circ_bbuf_t objects
#define CIRC_BBUF_DEF(x,y)                \
    uint32_t x##_data_space[y];            \
    static volatile circ_bbuf_t x = {                     \
        .arr = x##_data_space,         \
        .head = 0,                        \
        .tail = 0,                        \
        .max_len = y                       \
    }

// Settings
static const uint16_t timer_div = 80; // prescaler
static const uint64_t timer_max_count = 100000; // approximately .1s to achieve 10Hz
static const uint8_t CMD_BUF_LEN = 255; // message queue length
static const uint8_t BUF_LEN = 10; // # length of ISR fifo sample buffer
static const char avg_cmd[] = "avg";

// Pins
static const int adc_pin = A0; // adc0

// Globals
CIRC_BBUF_DEF(my_circ_buf, BUF_LEN); // circular buffer for storing samples from ADC
static uint8_t buf_idx = 0; // keeps track of the circular buffers idx
static float avg = 0.; // stores the avg value of the last 10 samples
static hw_timer_t* timer = NULL; // hw timer to sample from ADC at 10hz
static TaskHandle_t taskHandleCalculateAverage = NULL; // process task handle for calculating average

// func for pushing byte into buffer
int circ_bbuf_push(volatile circ_bbuf_t* buf, uint32_t data)
{
    // queue is full    
    if (buf->tail == buf->head)
    {
        return -1;
    }

    buf->arr[buf->head] = data; // save data to head
    // increment head and check if buffer is full
    buf->head = (buf->head + 1) % buf->max_len;
    return 0;
}

// Retrieve value from shared buffer via data. Success if return value is 0.
int circ_bbuf_pop(volatile circ_bbuf_t* buf, uint32_t* data)
{
    // buffer is empty
    if (buf->tail == buf->head)
    {
        return -1;
    }

    // copy value from buffer to data and return 0 for success
    *data = buf->arr[buf->tail];
    buf->tail = (buf->tail + 1) % buf->max_len;
    return 0;
}

// Interrupt Service Routines
// IRAM_ATTR = specify that the function is loaded into internal ram instead of flash
// Sample from adc and add to buffer
void IRAM_ATTR onTimer()
{
    // buf_idx = 0; // idx to keep track of current buffer fill
    BaseType_t task_woken = pdFALSE; // Keeps track of the task status

    // Only read adc and push to buffer if under the buffer length
    if (buf_idx < BUF_LEN)
    {
        uint16_t val = analogRead(adc_pin);
        if (circ_bbuf_push(&my_circ_buf, val) == 0)
        {
            buf_idx++;   
        }
    }

    // After 10 items have been added to the buffer, notify task to calculate average
    if (buf_idx >= BUF_LEN)
    {
        // idx = 0; // Reset counter
        vTaskNotifyGiveFromISR(taskHandleCalculateAverage, &task_woken);
    }

    // Use task_woken to check if the taskCalculateAverage is awoken from the semaphore / notification
    // and if so, immediately context switch to the task via portYIELD_FROM_ISR()
    if (task_woken)
    {
        portYIELD_FROM_ISR();
    }

    // Allow scheduler to choose next task
}

// Tasks
// Wait for semaphore (from ISR) and calculate average of values from buffer
// Task reads 10 values from the circular buffer before updating index
void taskCalculateAverage(void* parameters)
{
    while (1)
    {
        // Similar to xSempahoreTake() but uses a notification (faster) in place of a semaphore
        // xClearCountOnExit = when set to true will reset the notification counter to 0
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Read values from buffer and calculate average
        float tmp_avg = 0.;
        for (int i = 0; i < BUF_LEN; i++)
        {
            uint32_t val;
            assert(circ_bbuf_pop(&my_circ_buf, &val) != -1);
            tmp_avg += val;
        }
        tmp_avg /= BUF_LEN;
        // TODO: see if we need to add CS when storing tmp to global
        avg = tmp_avg;
        // TODO: see if we need to protect the buff idx
        buf_idx = 0;
    }
}

void taskCLI(void* parameters)
{
    uint8_t cmd_len = strlen(avg_cmd);
    char c;
    char cmd_buf[CMD_BUF_LEN];
    uint8_t idx = 0;
    memset(cmd_buf, 0, CMD_BUF_LEN); // Initially zero out the command buffer

    while (1)
    {
        // Echo user input and check if they've entered the avg command
        if (Serial.available() > 0)
        {
            c = Serial.read();
            Serial.print(c);

            // Add character input to buffer if space is available
            if (idx < CMD_BUF_LEN - 1)
            {
                cmd_buf[idx] = c;
                idx++;
            }

            // Check for avg command from user on newline (return)
            if (c == '\n' || c == '\r')
            {
                // User has entered avg command
                if (memcmp(cmd_buf, avg_cmd, cmd_len) == 0)
                {
                    char out[50];
                    sprintf(out, "Average: %.2f", avg);
                    Serial.println(avg);
                }

                // Clear buffer after user sends newline
                memset(cmd_buf, 0, CMD_BUF_LEN);
                idx = 0;
            }
        }
    }    
}

void setup()
{
    // Configure serial
    Serial.begin(115200);

   // Wait a moment to start (so we don't miss Serial output)
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    Serial.println();
    Serial.println("---FreeRTOS Hardware Interrupt Solution---");

    // Configure hw timer
    // Create and start timer - timerBegin is using the arduino esp32 api
    timer = timerBegin(0 /*timer id*/, timer_div, true /*count up*/);

    // Provide ISR to timer (timer, function, edge)
    timerAttachInterrupt(timer, &onTimer, true);

    // Configure timer count that should trigger ISR
    // 1000000 = 1 second delay -- 10Hz = 1/10 = .1s (100ms)
    timerAlarmWrite(timer, timer_max_count, true/*auto-reload*/);

    timerAlarmEnable(timer);

    // Create tasks
    // Create CLI task with higher priority
    xTaskCreatePinnedToCore(taskCLI, "taskClI", 2048, NULL, 2, NULL, app_cpu);
    // Create average task with lower priority
    xTaskCreatePinnedToCore(taskCalculateAverage, "taskClI", 2048, NULL, 1, &taskHandleCalculateAverage, app_cpu);

    // Delete the setup and loop task
    vTaskDelete(NULL);
}

void loop()
{
  // put your main code here, to run repeatedly:
}
