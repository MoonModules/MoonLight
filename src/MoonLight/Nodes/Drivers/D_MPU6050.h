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

  #include <MPU6050_6Axis_MotionApps20.h>

class MPU6050Driver : public Node {
 public:
  static const char* name() { return "MPU6050 driver"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  bool motionTrackingReady = false;  // set true if DMP init was successful

  Coord3D gyro;  // in degrees (not radians)
  Coord3D accell;
  VectorFloat gravityVector;

  void setup() override {
    // controls will show in the UI
    // for different type of controls see other Nodes
    // addControl(pin, "pin", "slider", 1, SOC_GPIO_PIN_COUNT - 1);
    addControl(gyro, "gyro", "coord3D");
    addControl(accell, "accell", "coord3D");
    // isEnabled = false;  // need to enable after fresh setup
  }

  bool initI2S() {
    // tbd: set pins in ui!!
    // allocatePin(21, "Pins", "I2S SDA");
    // allocatePin(22, "Pins", "I2S SCL");
    bool success = Wire.begin(5, 6);
    EXT_LOGI(ML_TAG, "initI2S Wire begin %s", success ? "success" : "failure");
    return success;
  }

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) {
    // add your custom onUpdate code here
    if (!control["on"].isNull()) { // control is the node n case of on!
      if (control["on"] == true) {
        if (initI2S()) {
          mpu.initialize();

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
              EXT_LOGI(ML_TAG, "DMP Initialization failed (code %d)", devStatus);
            }
          } else
            EXT_LOGI(ML_TAG, "Testing device connections MPU6050 connection failed");
        }
      }
    }
  }

  bool hasOnLayout() const override { return true; }  // so the mapping system knows this node has onLayout, eg each time a modifier changes
  void onLayout() override {};                        // onLayout for drivers is used to init or update the driver based on the layouts, a driver will use the layout nodes which are defined before the driver node, so order matters

  // use for continuous actions, e.g. reading data from sensors or sending data to lights (e.g. LED drivers or Art-Net)
  void loop20ms() override {
    // mpu.getMotion6(&accell.x, &accell.y, &accell.z, &gyro.x, &gyro.y, &gyro.z);
    // // display tab-separated accel/gyro x/y/z values
    // EXT_LOGI(ML_TAG, "mpu6050 %d,%d,%d %d,%d,%d", accell.x, accell.y, accell.z, gyro.x, gyro.y, gyro.z);

    // if programming failed, don't try to do anything
    if (!motionTrackingReady) return;
    // read a packet from FIFO
    if (mpu.dmpGetCurrentFIFOPacket(fifoBuffer)) {  // Get the Latest packet
      mpu.dmpGetQuaternion(&q, fifoBuffer);
      mpu.dmpGetGravity(&gravity, &q);
      mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
      gyro.y = ypr[0] * 180 / M_PI;  // pan = yaw !
      gyro.x = ypr[1] * 180 / M_PI;  // tilt = pitch !
      gyro.z = ypr[2] * 180 / M_PI;  // roll = roll
      sharedData.gravity.x = gravity.x;
      sharedData.gravity.y = gravity.y;
      sharedData.gravity.z = gravity.z;
      // display real acceleration, adjusted to remove gravity

      // needed to repeat the following 3 lines (yes if you look at the output: otherwise not 0)
      mpu.dmpGetQuaternion(&q, fifoBuffer);
      mpu.dmpGetAccel(&aa, fifoBuffer);
      mpu.dmpGetGravity(&gravity, &q);

      mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
      // mpu.dmpGetLinearAccelInWorld(&aaWorld, &aaReal, &q); //worked in 0.6.0, not in 1.3.0 anymore

      accell.x = aaReal.x;
      accell.y = aaReal.y;
      accell.z = aaReal.z;
    }
  };

  ~MPU6050Driver() override {};  // e.g. to free allocated memory

 private:
  MPU6050 mpu;

  // MPU control/status vars
  uint8_t devStatus;       // return status after each device operation (0 = success, !0 = error)
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