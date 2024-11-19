#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

#define VCOOL_VERSION "V1.10"

#define CONSUMER "vcool"

#define PWM_PERIOD_US 50000     // PWM period in microseconds (20Hz)

#define GPIO_CHIP "gpiochip4"   // use GPIO_4D1
#define GPIO_LINE 25

#define MAX_ROW_LENGTH 100

#define PROCESS_ID_FILE "/var/run/vcool.pid"

#define STRATEGY_FILE "/etc/vcool/vcool.stg"

#define CPU_TEMP_FILE "/sys/devices/virtual/thermal/thermal_zone0/temp"

#define GPU_TEMP_FILE "/sys/devices/virtual/thermal/thermal_zone1/temp"


typedef struct
{
    int duty_cycle;
    struct gpiod_line *line;
} pwm_args_t;

pwm_args_t pwm_args;

pthread_t pwm_thread_id;

struct gpiod_chip *chip;
struct gpiod_line *line;


// output log
void log_msg(const char *format, ...) 
{
    time_t rawtime;
    struct tm *timeinfo;
    char time_buffer[20];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    printf("[%s] ", time_buffer);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(NULL);
}


// release GPIO and restore to input mode with pull-up
void cleanup_handler(void *arg)
{
    struct gpiod_line *line = (struct gpiod_line *)arg;
    gpiod_line_release(line);
    struct gpiod_chip *chip = gpiod_line_get_chip(line);
    struct gpiod_line *input_line = gpiod_chip_get_line(chip, gpiod_line_offset(line));
    struct gpiod_line_request_config config =
    {
        .consumer = CONSUMER,
        .request_type = GPIOD_LINE_REQUEST_DIRECTION_INPUT,
        .flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP
    };
    if (gpiod_line_request(input_line, &config, 0) < 0)
    {
        log_msg("Call gpiod_line_request_input_flags() failed.");
    }
}


// cleanup handler for the thread
void thread_cleanup_handler(void *arg)
{
    //log_msg("Thread is cancelled.");
}


// thread to output PWM on GPIO
void *pwm_thread(void *args)
{
    pwm_args_t *pwm_args = (pwm_args_t *)args;
    int duty_cycle = pwm_args->duty_cycle;
    struct gpiod_line *line = pwm_args->line;
    const int period_us = PWM_PERIOD_US;
    int high_time_us = (period_us * duty_cycle) / 100;
    int low_time_us = period_us - high_time_us;

    pthread_cleanup_push(thread_cleanup_handler, NULL);

    while (1)
    {
        if (high_time_us > 0)
        {
          gpiod_line_set_value(line, 0); // set GPIO low (active)
          usleep(high_time_us);
        }
        if (low_time_us > 0)
        {
          gpiod_line_set_value(line, 1); // set GPIO high (inactive)
          usleep(low_time_us);
        }
    }

    pthread_cleanup_pop(1); // cleanup for cancelling
    return NULL;
}


// handler for received signal
void signal_handler(int signum)
{
    pthread_cancel(pwm_thread_id);
    pthread_join(pwm_thread_id, NULL);

    if (line != NULL)
    {
        cleanup_handler((void *)line);
    }

    exit(EXIT_SUCCESS);
}


// function to read the temperature from a file
int read_temperature(const char *file_path)
{
    FILE *file = fopen(file_path, "r");
    if (file == NULL)
    {
        log_msg("Could not open temperature file %s", file_path);
        return -1;
    }

    int temp_milli;
    fscanf(file, "%d", &temp_milli);
    fclose(file);

    return temp_milli / 1000;
}

// control the fan
bool control_fan(int duty_cycle, int cpu_temp, int gpu_temp)
{
    if (duty_cycle != pwm_args.duty_cycle)
    {
        pthread_cancel(pwm_thread_id);
        pthread_join(pwm_thread_id, NULL);
        pwm_args.duty_cycle = duty_cycle;
        if (cpu_temp > -999 && gpu_temp > -999)
        {
          char ch=248;
          log_msg("CPU=%d°C, GPU=%d°C, Fan=%d%%", cpu_temp, gpu_temp, duty_cycle);
        }
        if (duty_cycle > 0)
        {
            return (pthread_create(&pwm_thread_id, NULL, pwm_thread, &pwm_args) == 0);
        }
        else
        {
            gpiod_line_set_value(pwm_args.line, 1); // set GPIO high (inactive)
        }
    }
    return true;
}


// return true if the row is processed successfully
bool process_row(char *row, int cpu_temp, int gpu_temp)
{
    char *condition, *action;
    char *token;
    token = strtok(row, " ");
    if (token == NULL)
    {
        return false;
    }
    condition = token;
    token = strtok(NULL, " ");
    if (token == NULL || token[0] != 'F')
    {
        return false;
    }
    action = token;
    int duty_cycle = atoi(&action[1]);

    // check for logical operators in condition
    if (strstr(condition, "|"))
    {
        char *sub_cond1 = strtok(condition, "|");
        char *sub_cond2 = strtok(NULL, "|");
        if (sub_cond1 == NULL || sub_cond2 == NULL)
        {
            return false;
        }
        if ((sub_cond1[0] == 'C' && cpu_temp >= atoi(&sub_cond1[1])) ||
            (sub_cond1[0] == 'G' && gpu_temp >= atoi(&sub_cond1[1])) ||
            (sub_cond2[0] == 'C' && cpu_temp >= atoi(&sub_cond2[1])) ||
            (sub_cond2[0] == 'G' && gpu_temp >= atoi(&sub_cond2[1])))
        {
            return control_fan(duty_cycle, cpu_temp, gpu_temp);
        }
    }
    else if (strstr(condition, "&"))
    {
        char *sub_cond1 = strtok(condition, "&");
        char *sub_cond2 = strtok(NULL, "&");
        if (sub_cond1 == NULL || sub_cond2 == NULL)
        {
            return false;
        }
        if (((sub_cond1[0] == 'C' && cpu_temp >= atoi(&sub_cond1[1])) ||
             (sub_cond1[0] == 'G' && gpu_temp >= atoi(&sub_cond1[1]))) &&
            ((sub_cond2[0] == 'C' && cpu_temp >= atoi(&sub_cond2[1])) ||
             (sub_cond2[0] == 'G' && gpu_temp >= atoi(&sub_cond2[1]))))
        {
            return control_fan(duty_cycle, cpu_temp, gpu_temp);
        }
    }
    else
    {
        if ((condition[0] == 'C' && cpu_temp >= atoi(&condition[1])) ||
            (condition[0] == 'G' && gpu_temp >= atoi(&condition[1])))
        {
            return control_fan(duty_cycle, cpu_temp, gpu_temp);
        }
    }
    return false;
}

// check if given string represents a number
bool is_number(const char *str) 
{
    if (*str == '-') 
    {
        str ++;
    }
    while (*str) 
    {
        if (!isdigit(*str)) 
        {
            return false;
        }
        str ++;
    }
    return true;
}


/*
Usage:
    sudo vcool
    sudo vcool kill
    sudo vcool force
    sudo vcool 50
    sudo vcool 100 force
*/
int main(int argc, char **argv) {
    
    if (geteuid() != 0)
    {
        printf("Please run %s with sudo.\n", argv[0]);
        return EXIT_FAILURE;
    }
  
  
    log_msg("%s (%s) is launched.", argv[0], VCOOL_VERSION);
    
    bool kill_mode = false;
    bool force_mode = false;
    bool test_mode = false;
    int duty_cycle = 0;
    
    for (int i = 1; i < argc; i ++) 
    {
        if (strcmp(argv[i], "kill") == 0) 
        {
            kill_mode = true;
        }
        if (strcmp(argv[i], "force") == 0) 
        {
            force_mode = true;
        }
        if (is_number(argv[i]))
        {
            test_mode = true;
            duty_cycle = atoi(argv[i]);
            if (duty_cycle < 0)
            {
                duty_cycle = 0;
            }
            else if (duty_cycle > 100)
            {
                duty_cycle = 100;
            }
        }
    }
    
    // check for existed instance
    bool instance_found = false;
    FILE *file = fopen(PROCESS_ID_FILE, "r");
    if (file != NULL)
    {
        pid_t saved_pid;
        if (fscanf(file, "%d", &saved_pid) == 1)
        {
            if (kill(saved_pid, 0) == 0)
            {
                instance_found = true;
                if (kill_mode || force_mode)
                {
                    log_msg("Kill another instance with PID %d.", saved_pid);
                    kill(saved_pid, SIGINT);
                    sleep(1);
                }
                else
                {
                    printf("Another instance is running, run \"%s kill\" to kill it first, or run \"%s force\" to force running.\n", argv[0], argv[0]);
                }
            }
        }
        fclose(file);
    }
    if (instance_found && !force_mode)
    {
        return EXIT_SUCCESS;
    }
    
    
    // save current pid into file
    file = fopen(PROCESS_ID_FILE, "w");
    if (file == NULL) {
        log_msg("Can not open PID file for writting.");
        return EXIT_FAILURE;
    }
    fprintf(file, "%d\n", getpid());
    fclose(file);

    
    // request GPIO access
    chip = gpiod_chip_open_by_name(GPIO_CHIP);
    if (!chip) {
        log_msg("Call gpiod_chip_open_by_name() failed.");
        return EXIT_FAILURE;
    }

    line = gpiod_chip_get_line(chip, GPIO_LINE);
    if (!line) {
        log_msg("Call gpiod_chip_get_line() failed.");
        gpiod_chip_close(chip);
        return EXIT_FAILURE;
    }
    pwm_args.line = line;

    // set initial state to high (inactive)
    if (gpiod_line_request_output(line, CONSUMER, 1) < 0)
    {
        log_msg("Call gpiod_line_request_output() failed.");
        gpiod_chip_close(chip);
        return EXIT_FAILURE;
    }

    // register handler for SIGINT signal
    signal(SIGINT, signal_handler);
    
    if (test_mode)
    {
        log_msg("Test mode: set fan duty cycle to %d%%.", duty_cycle);
        control_fan(duty_cycle, -999, -999);
    }

    // check temperature and control fan in a loop
    while (1)
    {
        if (!test_mode)
        {
            int cpu_temp = read_temperature(CPU_TEMP_FILE);
            int gpu_temp = read_temperature(GPU_TEMP_FILE);
    
            //log_msg("CPU=%d, GPU=%d", cpu_temp, gpu_temp);
    
            char row[MAX_ROW_LENGTH];
            FILE *file = fopen(STRATEGY_FILE, "r");
            if (file == NULL)
            {
                log_msg("Could not open strategy file for reading.");
                return 1;
            }
            bool processed = false;
            while (fgets(row, sizeof(row), file))
            {
                row[strcspn(row, "\n")] = '\0';
                if (process_row(row, cpu_temp, gpu_temp))
                {
                    processed = true;
                    break;
                }
            }
            if (!processed)
            {
                control_fan(0, cpu_temp, gpu_temp);
            }
            fclose(file);
        }
        sleep(3);
    }

    // clean up
    pthread_cancel(pwm_thread_id);
    pthread_join(pwm_thread_id, NULL);

    cleanup_handler((void *)line);
    gpiod_chip_close(chip);

    return EXIT_SUCCESS;
}
