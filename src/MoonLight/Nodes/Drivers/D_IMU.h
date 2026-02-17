/**
    @title     MoonLight
    @file      D_MPU6050.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  // #include <BMI160Gen.h>
  #include <MPU6050_6Axis_MotionApps20.h>

class IMUDriver : public Node {
 public:
  static const char* name() { return "IMU driver"; }  // Inertial Measurement Unit
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  bool motionTrackingReady = false;  // set true if DMP init was successful

  Coord3D gyro;  // in degrees (not radians)
  Coord3D accell;
  uint8_t board = 0;  // default MPU6050

  bool i2cActuallyReady = false;

  void setup() override {
    addControl(gyro, "gyro", "coord3D");
    addControl(accell, "accell", "coord3D");
    addControl(board, "board", "select");
    addControlValue("MPU6050");
    addControlValue("BMI160");  // not supported yet

    // Subscribe to IO updates to detect when I2C becomes ready
    moduleIO->addUpdateHandler([this](const String& originId) { moduleIO->read([&](ModuleState& state) { i2cActuallyReady = state.data["I2CReady"]; }, name()); }, false);
  }

  void initBoard() {
    if (motionTrackingReady) return;

    EXT_LOGI(ML_TAG, "Starting board %d", board);
    if (board == 0) {  // MPU6050
      mpu.initialize();

      // delay(100);

      // verify connection
      if (mpu.testConnection()) {
        EXT_LOGI(ML_TAG, "MPU6050 connection successful Initializing DMP...");
        uint8_t devStatus = mpu.dmpInitialize();

        if (devStatus == 0) {
          // // Calibration Time: generate offsets and calibrate our MPU6050
          mpu.CalibrateAccel(6);
          mpu.CalibrateGyro(6);
          // mpu.PrintActiveOffsets();

          mpu.setDMPEnabled(true);  // mandatory

          // mpuIntStatus = mpu.getIntStatus();

          motionTrackingReady = true;
        } else {
          // ERROR!
          // 1 = initial memory load failed
          // 2 = DMP configuration updates failed
          // (if it's going to break, usually the code will be 1)
          EXT_LOGW(ML_TAG, "DMP Initialization failed (code %d)", devStatus);
        }
      } else
        EXT_LOGW(ML_TAG, "Testing device connections MPU6050 connection failed");
    } else if (board == 1) {  // BMI160 - NEW

      // BMI160.begin(BMI160GenClass::I2C_MODE, 0x68);

      // if (BMI160.getDeviceID() == 0xD1) {  // BMI160 device ID
      //   EXT_LOGI(ML_TAG, "BMI160 connection successful");
      //   motionTrackingReady = true;
      // } else {
      //   EXT_LOGW(ML_TAG, "BMI160 connection failed");
      // }
    }
  }

  void stopBoard() {
    if (!motionTrackingReady) return;  // not active so no need to stop

    EXT_LOGI(ML_TAG, "Stopping board %d", board);
    motionTrackingReady = false;
    if (board == 0) {  // MPU6050
      mpu.setDMPEnabled(false);
    }
  }

  bool requestInitBoard = false;
  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    // add your custom onUpdate code here
    if (!control["on"].isNull()) {  // control is the node n case of on!
      if (on) {
        requestInitBoard = true;
      } else {
        stopBoard();
      }
    } else if (control["name"] == "board") {
      stopBoard();
      if (on) requestInitBoard = true;
    }
  }

  void loop20ms() override {
    // mpu.getMotion6(&accell.x, &accell.y, &accell.z, &gyro.x, &gyro.y, &gyro.z);
    // // display tab-separated accel/gyro x/y/z values
    // EXT_LOGI(ML_TAG, "mpu6050 %d,%d,%d %d,%d,%d", accell.x, accell.y, accell.z, gyro.x, gyro.y, gyro.z);

    // process this out of onUpdate
    if (requestInitBoard) {
      if (i2cActuallyReady) {  // Only init if I2C is ACTUALLY ready RIGHT NOW
        initBoard();
        requestInitBoard = false;
      }
    }

    // if programming failed, don't try to do anything
    if (!motionTrackingReady) return;

    // read a packet from FIFO
    if (board == 0) {                                 // MPU6050
      if (mpu.dmpGetCurrentFIFOPacket(fifoBuffer)) {  // Get the Latest packet
        mpu.dmpGetQuaternion(&q, fifoBuffer);
        mpu.dmpGetGravity(&gravity, &q);
        mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
        gyro.y = ypr[0] * 180 / M_PI;  // pan = yaw !
        gyro.x = ypr[1] * 180 / M_PI;  // tilt = pitch !
        gyro.z = ypr[2] * 180 / M_PI;  // roll = roll
        sharedData.gravity.x = gravity.x * INT16_MAX;
        sharedData.gravity.y = gravity.y * INT16_MAX;
        sharedData.gravity.z = gravity.z * INT16_MAX;

        EXT_LOGD(ML_TAG, "%f %f %f", gravity.x, gravity.y, gravity.z);

        // needed to repeat the following 3 lines (yes if you look at the output: otherwise not 0): commented out
        // mpu.dmpGetQuaternion(&q, fifoBuffer);
        mpu.dmpGetAccel(&aa, fifoBuffer);
        // mpu.dmpGetGravity(&gravity, &q);

        mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
        // mpu.dmpGetLinearAccelInWorld(&aaWorld, &aaReal, &q); //worked in 0.6.0, not in 1.3.0 anymore

        accell.x = aaReal.x;
        accell.y = aaReal.y;
        accell.z = aaReal.z;
      }
    } else if (board == 1) {  // BMI160 - NEW
      // int gx, gy, gz, ax, ay, az;
      // BMI160.readGyro(gx, gy, gz);
      // BMI160.readAccelerometer(ax, ay, az);

      // // Convert raw values to degrees (BMI160 gyro: 16.4 LSB/°/s at ±2000°/s range)
      // gyro.x = gx / 16.4f;
      // gyro.y = gy / 16.4f;
      // gyro.z = gz / 16.4f;

      // // Convert raw accel values (BMI160 accel: 16384 LSB/g at ±2g range)
      // accell.x = ax;
      // accell.y = ay;
      // accell.z = az;

      // // Calculate gravity vector from accelerometer
      // float norm = sqrt(ax*ax + ay*ay + az*az);
      // if (norm > 0) {
      //   sharedData.gravity.x = (ax / norm) * INT16_MAX;
      //   sharedData.gravity.y = (ay / norm) * INT16_MAX;
      //   sharedData.gravity.z = (az / norm) * INT16_MAX;
      // }
    }
  };

  ~IMUDriver() override { stopBoard(); }

 private:
  MPU6050 mpu;

  // MPU control/status vars
  uint8_t fifoBuffer[64];  // FIFO storage buffer

  // orientation/motion vars
  Quaternion q;        // [w, x, y, z]         quaternion container
  VectorInt16 aa;      // [x, y, z]            accel sensor measurements
  VectorInt16 aaReal;  // [x, y, z]            gravity-free accel sensor measurements
  // VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
  VectorFloat gravity;  // [x, y, z]            gravity vector
  // float euler[3];         // [psi, theta, phi]    Euler angle container
  float ypr[3];  // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector
};

#endif