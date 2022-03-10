#include <Arduino_LSM6DS3.h>
#include <SoftwareSerial.h>
#include <PololuQik.h>
#include <Average.h>

PololuQik2s9v1 qik(10, 11, 4);                  // Motor encoder arduino connection pins
float xlx, xly, xlz;                            // Accelerometer values
float gx, gy, gz;                               // Gyroscope values

// Rotary Encoder Inputs
#define CLK 2
#define DT 3
#define ENCVCC 12
#define ENCGND 9
#define ENCPPR 150 // Counts/revln

float RAD2DEG = 180/PI;
float TH_OFFSET = 0.0;                         // Offset for pole angle calibration 78.0

float WHEEL_DIA = 0.08;                         // Wheel diameter in metres
int counter = -1;                               // Wheel encoder counter
int state_clk;                                  // Flag to check whether encoder value should be updated
int state_clk_prev;                                 // - Related flag
float state_x = 0;                              // Most recent position measurement
float state_x_prev = 0;                         // Previous position measurement

long state_tpos = 0;                            // Absolute most recent time for position/velocity calculations
long state_tpos_prev = 0;                       // Absolute previous time for position/velocity calculations
float state_dx = 0;                             // Velocity state measurement
float dt_v = 5;                                 // Velocity state update interval (ms)

float state[4] = {0, 0, 0, 0};                  // Structure for storing states {x, dx, theta, dtheta}
float MAX_THETA = 80;                           // Maximum theta value

long first_time, now_time, prev_time=0, loop_time;
float state2_acc = 0;
float state2_gyro = 0;
float d_gyro;

float state_th;                                 // Variable for pole angle estimation
float theta_xl;                                 // Pole angle measurement from accelerometer

int varyindx = 0;                               // Index for velocity smoothing filter
float varray[5] = {0,0,0,0,0};                  // Structure for velocity smoothing filter
int varysize = sizeof(varray)/4;                // Size of array in bytes - 4 bytes in float

bool init_flag = true;                          // Second initialization semaphore

Average<float> th_ave(10);                       // Smoothing filter for pole angle measurement (from external library)
Average<float> dth_ave(10);                      // Smoothing filter for d(pole angle) measurement (from external library)

float endNode;

// MOTOR CONTROLS AND LIMITS
int speedVal = 0;
int speedInc = 1;
int maxSpeed = 127;

//==============SETUP======================

void setup() {

    Serial.begin(19200);
    qik.init();
    encoderInit();

    while (!Serial);

    if (!IMU.begin()) {
        Serial.println("Failed to initialize IMU!");

        while (1);
    }

    Serial.println("<Arduino is ready>");    
}

//===============LOOP=======================

void loop() {

    float command;

    if (init_flag){
        // Initial encoder time measurement
        state_tpos_prev = millis();

        // Initial pole angle measurement
        IMU.readAcceleration(xlx, xly, xlz);
        state_th = atan2(-xlz,xly)*RAD2DEG+TH_OFFSET;
        init_flag = false;
        Serial.println("<Second initialization complete>");
    }

    first_time = millis();
    now_time = millis();

    // SENSOR TEST
    while ((now_time - first_time)/1000 < 5) {
       qik.setSpeeds(100, 100);
    }

    qik.setSpeeds(0, 0);
    Serial.println("<EXPERIMENT ENDED>");
    while (1) {
    }
}

//==========================================
// Takes raw IMU and encoder data and calculates position, velocity, pole angle, angular velocity
void getStates2() {

    // Set distance ===================================================================================
    state[0] = state_x;
    
    // Set velocity ===================================================================================
    state[1] = state_dx;

    // Set pole angle =================================================================================
    IMU.readGyroscope(gx, gy, gz);                                          // Take gyroscope measurement
    IMU.readAcceleration(xlx, xly, xlz);                                    // Take accelerometer measurement
    // Accelerator measurement
    state2_acc = atan2(xlz,xly)*RAD2DEG+TH_OFFSET;
    // Gyroscope measurement
    d_gyro = gx*loop_time/1000-0.2;
    state2_gyro = state2_gyro + d_gyro;
    // Filtered angle
    th_ave.push(-state2_acc);
    state[2] = th_ave.mean();                                               // Theta with averaging filter
//    state[2] = -state2_acc;
    
    // Set angular velocity ===================================================================================
    dth_ave.push(-d_gyro);
    state[3] = dth_ave.mean();                                              // dTheta with averaging filter
//    state[3] = -d_gyro;
}

//==========================================
// Takes raw IMU and encoder data and calculates position, velocity, pole angle, angular velocity
void getStates() {

    // Set distance ===================================================================================
    state[0] = state_x;

    // Set velocity ===================================================================================
    state[1] = state_dx;
    
    IMU.readGyroscope(gx, gy, gz);                                          // Take gyroscope measurement
    IMU.readAcceleration(xlx, xly, xlz);                                    // Take accelerometer measurement

    gx = gx - 0.8;                                                         // Stationary offset adjustment

    if (!isnan(gx) && !isnan(xly) && !isnan(xlz)){
        // Set pole angle =================================================================================
        theta_xl = atan2(-xlz,xly)*RAD2DEG+TH_OFFSET;
        state_th = 0.9*(state_th+gx*(loop_time/1000.0)) + 0.1*theta_xl;       // Kalman filter operation to combine gyro and xl measurements (gyro outputs in deg/s)
//        Serial.println((loop_time/1000.0));
        
        state[2] = state_th;                                                // Theta without averaging filter
//        th_ave.push(state_th);
//        state[2] = th_ave.mean();                                           // dTheta with averaging filter

        // Set angular velocity ===========================================================================
        dth_ave.push(gx);
        state[3] = dth_ave.mean();                                          // dTheta with averaging filter
    }
}

//==========================================
// Updates encoder count via interrupt for position and velocity measurements
void updateEncoder() {
    // Read the current state of CLK
    float varysum = 0;
    float vmean = 0;

    state_clk = digitalRead(CLK);

    // If last and current state of CLK are different, then pulse occurred
    // React to only 1 state change to avoid double count
    if (state_clk != state_clk_prev  && state_clk == 1) {

        // If the DT state is different than the CLK state, the encoder is rotating CCW so decrement
        if (digitalRead(DT) != state_clk) {
            counter --;
        } else {
        // Encoder is rotating CW so increment
            counter ++;
        }

        // Calculate cart position based on wheel encoder counter
        state_x = WHEEL_DIA * PI * counter / ENCPPR;

    }

    // Update velocity value on every 10 ms (if time interval dt is too small, errors may occur)
    state_tpos = millis();
    if (state_tpos - state_tpos_prev >= dt_v) {
        state_dx = 1000*(state_x - state_x_prev) / (state_tpos - state_tpos_prev);

        // Smoothing filter for velocity measurements
        varray[varyindx] = state_dx;
        varyindx ++;
        if (varyindx == varysize){
            varyindx = 0;
        }
        for (int i = 0; i < varysize; i++){
            varysum += varray[i];
        }
        vmean = varysum/varysize;
        state_dx = vmean;
        
        state_tpos_prev = state_tpos;
        state_x_prev = state_x;
    }

  // Remember last CLK state
  state_clk_prev = state_clk;
}

//==========================================
// Initializes encoder interrupt routine
void encoderInit() {
    pinMode(ENCVCC, OUTPUT);
    pinMode(ENCGND, OUTPUT);
    digitalWrite(ENCVCC, HIGH);
    digitalWrite(ENCGND, LOW);

    pinMode(CLK, INPUT);
    pinMode(DT, INPUT);

    // Read the initial state of CLK
    state_clk_prev = digitalRead(CLK);

    // Call updateEncoder() when any high/low changed seen on interrupt 0 (pin 2), or interrupt 1 (pin 3)
    attachInterrupt(0, updateEncoder, CHANGE);
    attachInterrupt(1, updateEncoder, CHANGE);

    state_tpos_prev = millis();
}
