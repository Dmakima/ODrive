
#include <stdlib.h>
#include <functional>
#include "gpio.h"

#include "utils.h"
#include "odrive_main.h"

Axis::Axis(const AxisHardwareConfig_t& hw_config,
           AxisConfig_t& config,
           Encoder& encoder,
           SensorlessEstimator& sensorless_estimator,
           Controller& controller,
           Motor& motor)
    : hw_config_(hw_config),
      config_(config),
      encoder_(encoder),
      sensorless_estimator_(sensorless_estimator),
      controller_(controller),
      motor_(motor)
{
    encoder_.axis_ = this;
    sensorless_estimator_.axis_ = this;
    controller_.axis_ = this;
    motor_.axis_ = this;
}

static void step_cb_wrapper(void* ctx) {
    reinterpret_cast<Axis*>(ctx)->step_cb();
}

static void min_endstop_cb_wrapper(void* ctx){
    reinterpret_cast<Axis*>(ctx)->min_endstop_cb();
}

static void max_endstop_cb_wrapper(void* ctx){
    reinterpret_cast<Axis*>(ctx)->max_endstop_cb();
}

// @brief Sets up all components of the axis,
// such as gate driver and encoder hardware.
void Axis::setup() {
    encoder_.setup();
    motor_.setup();
}

static void run_state_machine_loop_wrapper(void* ctx) {
    reinterpret_cast<Axis*>(ctx)->run_state_machine_loop();
    reinterpret_cast<Axis*>(ctx)->thread_id_valid_ = false;
}

// @brief Starts run_state_machine_loop in a new thread
void Axis::start_thread() {
    osThreadDef(thread_def, run_state_machine_loop_wrapper, hw_config_.thread_priority, 0, 4*512);
    thread_id_ = osThreadCreate(osThread(thread_def), this);
    thread_id_valid_ = true;
}

// @brief Unblocks the control loop thread.
// This is called from the current sense interrupt handler.
void Axis::signal_current_meas() {
    if (thread_id_valid_)
        osSignalSet(thread_id_, M_SIGNAL_PH_CURRENT_MEAS);
}

// @brief Blocks until a current measurement is completed
// @returns True on success, false otherwise
bool Axis::wait_for_current_meas() {
    return osSignalWait(M_SIGNAL_PH_CURRENT_MEAS, PH_CURRENT_MEAS_TIMEOUT).status == osEventSignal;
}

// step/direction interface
void Axis::step_cb() {
    if (enable_step_dir_) {
        GPIO_PinState dir_pin = HAL_GPIO_ReadPin(hw_config_.dir_port, hw_config_.dir_pin);
        float dir = (dir_pin == GPIO_PIN_SET) ? 1.0f : -1.0f;
        controller_.pos_setpoint_ += dir * config_.counts_per_step;
    }
};

// @brief Enables or disables step/dir input
void Axis::set_step_dir_enabled(bool enable) {
    if (enable) {
        // Set up the direction GPIO as input
        GPIO_InitTypeDef GPIO_InitStruct;
        GPIO_InitStruct.Pin = hw_config_.dir_pin;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(hw_config_.dir_port, &GPIO_InitStruct);

        // Subscribe to rising edges of the step GPIO
        GPIO_subscribe(hw_config_.step_port, hw_config_.step_pin, GPIO_PULLDOWN, 
                       GPIO_MODE_IT_FALLING, step_cb_wrapper, this);

        enable_step_dir_ = true;
    } else {
        enable_step_dir_ = false;

        // Unsubscribe from step GPIO
        GPIO_unsubscribe(hw_config_.step_port, hw_config_.step_pin);
    }
}

void Axis::min_endstop_cb(){
    uint16_t gpio_pin = get_gpio_pin_by_pin(config_.min_endstop.gpio_num);
    GPIO_TypeDef* gpio_port = get_gpio_port_by_pin(config_.min_endstop.gpio_num);

    if(config_.min_endstop.enabled){
        min_endstop_state_ = HAL_GPIO_ReadPin(gpio_port, gpio_pin);
        if(config_.min_endstop.is_active_high == false)
            min_endstop_state_ = !min_endstop_state_;
    } else {
        min_endstop_state_ = false;
    }
}

void Axis::set_min_endstop_enabled(bool enable){
    uint16_t gpio_pin = get_gpio_pin_by_pin(config_.min_endstop.gpio_num);
    GPIO_TypeDef* gpio_port = get_gpio_port_by_pin(config_.min_endstop.gpio_num);
    if(enable){
        GPIO_InitTypeDef GPIO_InitStruct;
        GPIO_InitStruct.Pin = gpio_pin;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(gpio_port, &GPIO_InitStruct);

        uint32_t pull_up_down = config_.min_endstop.is_active_high ? GPIO_PULLDOWN : GPIO_PULLUP;
        uint32_t interrupt_mode = config_.min_endstop.is_active_high ? GPIO_MODE_IT_RISING : GPIO_MODE_IT_FALLING;
        GPIO_subscribe(gpio_port, gpio_pin, pull_up_down, interrupt_mode,
                        min_endstop_cb_wrapper, this);
    }
    else {
        GPIO_unsubscribe(gpio_port, gpio_pin);
    }
}

void Axis::max_endstop_cb(){
    uint16_t gpio_pin = get_gpio_pin_by_pin(config_.max_endstop.gpio_num);
    GPIO_TypeDef* gpio_port = get_gpio_port_by_pin(config_.max_endstop.gpio_num);

    if(config_.max_endstop.enabled){
        max_endstop_state_ = HAL_GPIO_ReadPin(gpio_port, gpio_pin);
        if(config_.max_endstop.is_active_high == false)
            max_endstop_state_ = !max_endstop_state_;
    } else {
        max_endstop_state_ = false;
    }
}

void Axis::set_max_endstop_enabled(bool enable){
    uint16_t gpio_pin = get_gpio_pin_by_pin(config_.max_endstop.gpio_num);
    GPIO_TypeDef* gpio_port = get_gpio_port_by_pin(config_.max_endstop.gpio_num);
    if(enable){
        GPIO_InitTypeDef GPIO_InitStruct;
        GPIO_InitStruct.Pin = gpio_pin;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(gpio_port, &GPIO_InitStruct);

        uint32_t pull_up_down = config_.max_endstop.is_active_high ? GPIO_PULLDOWN : GPIO_PULLUP;
        uint32_t interrupt_mode = config_.max_endstop.is_active_high ? GPIO_MODE_IT_RISING : GPIO_MODE_IT_FALLING;
        GPIO_subscribe(gpio_port, gpio_pin, pull_up_down, interrupt_mode,
                        max_endstop_cb_wrapper, this);
    }
    else {
        GPIO_unsubscribe(gpio_port, gpio_pin);
    }
}

bool Axis::check_for_errors() {
    // Maybe we should update this to only trigger on new errors?
    // The danger with that is we could fail to bail on uncleared errors that still prevent
    // correct opreation.

    // For now: we treat ERROR_INVALID_STATE in idle loop special, or we could never stay
    // in idle after this kind of error.
    if (current_state_ == AXIS_STATE_IDLE)
        return (error_ & ~ERROR_INVALID_STATE) == ERROR_NONE;
    else
        return error_ == ERROR_NONE;
}

// @brief Do axis level checks and call subcomponent do_checks
// Returns true if everything is ok.
bool Axis::do_checks() {
    if (!brake_resistor_armed)
        error_ |= ERROR_BRAKE_RESISTOR_DISARMED;
    if ((current_state_ != AXIS_STATE_IDLE) && (motor_.armed_state_ == Motor::ARMED_STATE_DISARMED))
        // motor got disarmed in something other than the idle loop
        error_ |= ERROR_MOTOR_DISARMED;
    if (!(vbus_voltage >= board_config.dc_bus_undervoltage_trip_level))
        error_ |= ERROR_DC_BUS_UNDER_VOLTAGE;
    if (!(vbus_voltage <= board_config.dc_bus_overvoltage_trip_level))
        error_ |= ERROR_DC_BUS_OVER_VOLTAGE;

    // Sub-components should use set_error which will propegate to this error_
    motor_.do_checks();
    encoder_.do_checks();
    // sensorless_estimator_.do_checks();
    // controller_.do_checks();

    return check_for_errors();
}

// @brief Update all esitmators
bool Axis::do_updates() {
    // Sub-components should use set_error which will propegate to this error_
    encoder_.update();
    sensorless_estimator_.update();
    return check_for_errors();
}

float Axis::get_temp() {
    float adc = adc_measurements_[hw_config_.thermistor_adc_ch];
    float normalized_voltage = adc / adc_full_scale;
    return horner_fma(normalized_voltage, thermistor_poly_coeffs, thermistor_num_coeffs);
}

bool Axis::run_sensorless_spin_up() {
    // Early Spin-up: spiral up current
    float x = 0.0f;
    run_control_loop([&](){
        float phase = wrap_pm_pi(config_.ramp_up_distance * x);
        float I_mag = config_.spin_up_current * x;
        x += current_meas_period / config_.ramp_up_time;
        if (!motor_.update(I_mag, phase))
            return error_ |= ERROR_MOTOR_FAILED, false;
        return x < 1.0f;
    });
    if (error_ != ERROR_NONE)
        return false;
    
    // Late Spin-up: accelerate
    float vel = config_.ramp_up_distance / config_.ramp_up_time;
    float phase = wrap_pm_pi(config_.ramp_up_distance);
    run_control_loop([&](){
        vel += config_.spin_up_acceleration * current_meas_period;
        phase = wrap_pm_pi(phase + vel * current_meas_period);
        float I_mag = config_.spin_up_current;
        if (!motor_.update(I_mag, phase))
            return error_ |= ERROR_MOTOR_FAILED, false;
        return vel < config_.spin_up_target_vel;
    });

    // call to controller.reset() that happend when arming means that vel_setpoint
    // is zeroed. So we make the setpoint the spinup target for smooth transition.
    controller_.vel_setpoint_ = config_.spin_up_target_vel;

    return check_for_errors();
}

// Note run_sensorless_control_loop and run_closed_loop_control_loop are very similar and differ only in where we get the estimate from.
bool Axis::run_sensorless_control_loop() {
    set_min_endstop_enabled(config_.min_endstop.enabled);
    set_max_endstop_enabled(config_.max_endstop.enabled);
    set_step_dir_enabled(config_.enable_step_dir);

    run_control_loop([this](){
        if (controller_.config_.control_mode >= CTRL_MODE_POSITION_CONTROL)
            return error_ |= ERROR_POS_CTRL_DURING_SENSORLESS, false;

        // Note that all estimators are updated in the loop prefix in run_control_loop
        float current_setpoint;
        if (!controller_.update(sensorless_estimator_.pll_pos_, sensorless_estimator_.vel_estimate_, &current_setpoint))
            return error_ |= ERROR_CONTROLLER_FAILED, false;
        if (!motor_.update(current_setpoint, sensorless_estimator_.phase_))
            return false; // set_error should update axis.error_
        return true;
    });
    set_step_dir_enabled(false);
    return check_for_errors();
}

bool Axis::run_closed_loop_control_loop() {
    set_step_dir_enabled(config_.enable_step_dir);
    run_control_loop([this](){
        // Note that all estimators are updated in the loop prefix in run_control_loop
        float current_setpoint;
        if (!controller_.update(encoder_.pos_estimate_, encoder_.vel_estimate_, &current_setpoint))
            return error_ |= ERROR_CONTROLLER_FAILED, false; //TODO: Make controller.set_error
        if (!motor_.update(current_setpoint, encoder_.phase_))
            return false; // set_error should update axis.error_
        

        // Check for endstop presses
        if(config_.min_endstop.enabled && min_endstop_state_) {
            return error_ |= ERROR_MIN_ENDSTOP_PRESSED, false;
        } else if(config_.max_endstop.enabled && max_endstop_state_) {
            return error_ |= ERROR_MAX_ENDSTOP_PRESSED, false;
        }
        return true;
    });
    set_step_dir_enabled(false);
    return check_for_errors();
}

bool Axis::run_idle_loop() {
    // run_control_loop ignores missed modulation timing updates
    // if and only if we're in AXIS_STATE_IDLE
    safety_critical_disarm_motor_pwm(motor_);
    run_control_loop([this](){
        return true;
    });
    return check_for_errors();
}

// Infinite loop that does calibration and enters main control loop as appropriate
void Axis::run_state_machine_loop() {

    // Allocate the map for anti-cogging algorithm and initialize all values to 0.0f
    // TODO: Move this somewhere else
    // TODO: respect changes of CPR
    int encoder_cpr = encoder_.config_.cpr;
    controller_.anticogging_.cogging_map = (float*)malloc(encoder_cpr * sizeof(float));
    if (controller_.anticogging_.cogging_map != NULL) {
        for (int i = 0; i < encoder_cpr; i++) {
            controller_.anticogging_.cogging_map[i] = 0.0f;
        }
    }

    // arm!
    motor_.arm();
    
    for (;;) {
        // Load the task chain if a specific request is pending
        if (requested_state_ != AXIS_STATE_UNDEFINED) {
            size_t pos = 0;
            if (requested_state_ == AXIS_STATE_STARTUP_SEQUENCE) {
                if (config_.startup_motor_calibration)
                    task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                if (config_.startup_encoder_index_search && encoder_.config_.use_index)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_INDEX_SEARCH;
                if (config_.startup_encoder_offset_calibration)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_OFFSET_CALIBRATION;
                if (config_.startup_closed_loop_control){
                    task_chain_[pos++] = AXIS_STATE_CLOSED_LOOP_CONTROL;
                    if(config_.startup_homing)
                        task_chain_[pos++] = AXIS_STATE_HOMING;
                }
                else if (config_.startup_sensorless_control)
                    task_chain_[pos++] = AXIS_STATE_SENSORLESS_CONTROL;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ == AXIS_STATE_HOMING){
                task_chain_[pos++] = AXIS_STATE_HOMING;
                task_chain_[pos++] = AXIS_STATE_CLOSED_LOOP_CONTROL;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ == AXIS_STATE_FULL_CALIBRATION_SEQUENCE) {
                task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                if (encoder_.config_.use_index)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_INDEX_SEARCH;
                task_chain_[pos++] = AXIS_STATE_ENCODER_OFFSET_CALIBRATION;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ != AXIS_STATE_UNDEFINED) {
                task_chain_[pos++] = requested_state_;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            }
            task_chain_[pos++] = AXIS_STATE_UNDEFINED; // TODO: bounds checking
            requested_state_ = AXIS_STATE_UNDEFINED;
            // Auto-clear any invalid state error
            error_ &= ~ERROR_INVALID_STATE;
        }

        // Note that current_state is a reference to task_chain_[0]

        // Validate the state before running it
        if (current_state_ > AXIS_STATE_MOTOR_CALIBRATION && !motor_.is_calibrated_)
            current_state_ = AXIS_STATE_UNDEFINED;
        if (current_state_ > AXIS_STATE_ENCODER_OFFSET_CALIBRATION && !encoder_.is_ready_)
            current_state_ = AXIS_STATE_UNDEFINED;

        // Run the specified state
        // Handlers should exit if requested_state != AXIS_STATE_UNDEFINED
        bool status;
        switch (current_state_) {
            case AXIS_STATE_MOTOR_CALIBRATION:
                status = motor_.run_calibration();
                break;

            case AXIS_STATE_ENCODER_INDEX_SEARCH:
                status = encoder_.run_index_search();
                break;

            case AXIS_STATE_HOMING:
                status = controller_.home_axis();
                break;

            case AXIS_STATE_ENCODER_OFFSET_CALIBRATION:
                status = encoder_.run_offset_calibration();
                break;

            case AXIS_STATE_SENSORLESS_CONTROL:
                status = run_sensorless_spin_up(); // TODO: restart if desired
                if (status)
                    status = run_sensorless_control_loop();
                break;

            case AXIS_STATE_CLOSED_LOOP_CONTROL:
                status = run_closed_loop_control_loop();
                break;

            case AXIS_STATE_IDLE:
                run_idle_loop();
                status = motor_.arm(); // done with idling - try to arm the motor
                break;

            default:
                error_ |= ERROR_INVALID_STATE;
                status = false; // this will set the state to idle
                break;
        }

        // If the state failed, go to idle, else advance task chain
        if (!status)
            current_state_ = AXIS_STATE_IDLE;
        else
            memcpy(task_chain_, task_chain_ + 1, sizeof(task_chain_) - sizeof(task_chain_[0]));
    }
}
