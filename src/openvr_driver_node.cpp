#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <cmath>

#include <openvr.h>
#include "rclcpp/rclcpp.hpp"

#include "vive_msgs/msg/vive_system.hpp"
#include "tf2/LinearMath/Matrix3x3.h"
#include "tf2/LinearMath/Quaternion.h"

using namespace std::chrono_literals;

struct Device {
  int index;
  vr::ETrackedDeviceClass type;
};

enum ButtonStates {
  Button_Released = 0,
  Button_Touched = 1,
  Button_Pressed = 2
};

const uint64_t buttonBitmasks[4] = {
  (uint64_t)pow(2, (int) vr::k_EButton_ApplicationMenu),
  (uint64_t)pow(2, (int) vr::k_EButton_Grip),
  (uint64_t)pow(2, (int) vr::k_EButton_SteamVR_Touchpad),
  (uint64_t)pow(2, (int) vr::k_EButton_SteamVR_Trigger)
};

Device devices[vr::k_unMaxTrackedDeviceCount];
std::vector<int> controllerIndices;
vr::IVRSystem* ivrSystem;

//*devices must be a pointer to an array of Device structs of size vr::k_unMaxTrackedDeviceCount
void catalogDevices(vr::IVRSystem* ivrSystem, Device* devices, bool printOutput) {
  std::string TrackedDeviceTypes[] = {
    "Invalid (disconnected)", 
    "HMD",
    "Controller",
    "Generic Tracker",
    "Tracking Reference (base station)",
    "Display Redirect"
  };

  for (int i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
    if (printOutput) {
      if (ivrSystem->GetTrackedDeviceClass(i) != 0) {
        std::cout << "Tracked Device " << i << " has type " << TrackedDeviceTypes[ivrSystem->GetTrackedDeviceClass(i)] << "." << std::endl;
      }
    }
    (*(devices + i)).index = i;
    (*(devices + i)).type = ivrSystem->GetTrackedDeviceClass(i);
  }
}

void catalogControllers(Device* devices, bool printOutput) {
  controllerIndices.erase(controllerIndices.begin(), controllerIndices.begin() + controllerIndices.size());
  for (int i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
    if ((devices + i)->type == vr::TrackedDeviceClass_Controller) {
      controllerIndices.push_back((devices + i)->index);
    }
  }

  if (printOutput) {
    for (int i = 0; i < controllerIndices.size(); ++i) {
      std::cout << "There is a controller with index " << controllerIndices[i] << "." << std::endl;
    }
    std::cout << "There are " << controllerIndices.size() << " controllers." << std::endl;
  }
}

class Talker : public rclcpp::Node {
public:
  explicit Talker(const std::string & topic_name) : Node("talker") {
    msg_ = std::make_shared<vive_msgs::msg::ViveSystem>();

    auto publish_message = [this]() -> void {

      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - update >= 1000) {
        catalogDevices(ivrSystem, devices, false);
        catalogControllers(devices, false);
        update += 1000;
      }
      
      for (int controller = 0; controller < controllerIndices.size(); ++controller) {
        if (msg_->controllers.size() != controllerIndices.size()) {
          msg_->controllers.resize(controllerIndices.size());
        }
        while (msg_->controllers[controller].joystick.buttons.size() < 4) {
          msg_->controllers[controller].joystick.buttons.push_back(0);
        }

        while (msg_->controllers[controller].joystick.axes.size() < 3) {
          msg_->controllers[controller].joystick.axes.push_back(0.0);
        }

        vr::VRControllerState_t state;
        vr::TrackedDevicePose_t pose;

        ivrSystem->GetControllerStateWithPose(vr::TrackingUniverseStanding, controllerIndices[controller], &state, sizeof(state), &pose);

        // Buttons
        for (int button = 0; button < 4; ++button) {
          if ((state.ulButtonPressed & buttonBitmasks[button]) != 0) {
            msg_->controllers[controller].joystick.buttons[button] = Button_Pressed;
          } else if ((state.ulButtonTouched & buttonBitmasks[button]) != 0) {
              msg_->controllers[controller].joystick.buttons[button] = Button_Touched;
          } else {
            msg_->controllers[controller].joystick.buttons[button] = Button_Released;
          }
        }
        
        // Axis
        msg_->controllers[controller].joystick.axes[0] = state.rAxis[0].x;
        msg_->controllers[controller].joystick.axes[1] = state.rAxis[0].y;
        msg_->controllers[controller].joystick.axes[2] = state.rAxis[1].x;

        // Position
        msg_->controllers[controller].posestamped.pose.position.x = pose.mDeviceToAbsoluteTracking.m[0][3];
        msg_->controllers[controller].posestamped.pose.position.y = pose.mDeviceToAbsoluteTracking.m[1][3];
        msg_->controllers[controller].posestamped.pose.position.z = pose.mDeviceToAbsoluteTracking.m[2][3];
        
        // Orientation
        tf2::Matrix3x3 rotMatrix;
        tf2::Quaternion quaternion;

        rotMatrix.setValue(pose.mDeviceToAbsoluteTracking.m[0][0], pose.mDeviceToAbsoluteTracking.m[0][1], pose.mDeviceToAbsoluteTracking.m[0][2],
                           pose.mDeviceToAbsoluteTracking.m[1][0], pose.mDeviceToAbsoluteTracking.m[1][1], pose.mDeviceToAbsoluteTracking.m[1][2],
                           pose.mDeviceToAbsoluteTracking.m[2][0], pose.mDeviceToAbsoluteTracking.m[2][1], pose.mDeviceToAbsoluteTracking.m[2][2]
        );

        rotMatrix.getRotation(quaternion);
        msg_->controllers[controller].posestamped.pose.orientation.x = quaternion.x();
        msg_->controllers[controller].posestamped.pose.orientation.y = quaternion.y();
        msg_->controllers[controller].posestamped.pose.orientation.z = quaternion.z();
        msg_->controllers[controller].posestamped.pose.orientation.w = quaternion.w();

        // Fill PoseStamped header
        msg_->controllers[controller].posestamped.header.stamp = rclcpp::Node::now();
        msg_->controllers[controller].posestamped.header.frame_id = "vive_base";

        msg_->controllers[controller].id = controllerIndices[controller];
      }

		  pub_->publish(msg_);
    };

    rmw_qos_profile_t custom_qos_profile = rmw_qos_profile_default;
    custom_qos_profile.depth = 7;
    pub_ = this->create_publisher<vive_msgs::msg::ViveSystem>(topic_name, custom_qos_profile);

    // Update rate of openvr motion tracking is ~250Hz
    timer_ = this->create_wall_timer(10ms, publish_message);
  }

private:
  size_t count_ = 1;
  std::shared_ptr<vive_msgs::msg::ViveSystem> msg_;
  rclcpp::Publisher<vive_msgs::msg::ViveSystem>::SharedPtr pub_;
  rclcpp::TimerBase::SharedPtr timer_;

  int64_t update = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
};

int main(int argc, char * argv[]) {
  rclcpp::init(argc, argv);

  auto node = std::make_shared<Talker>("vive");

  if (!vr::VR_IsHmdPresent()) {
    std::cout << "No HMD was found in the system, quitting app" << std::endl;
    return -1;
  } else {
    std::cout << "An HMD was successfully found in the system" << std::endl;
  }

  if (!vr::VR_IsRuntimeInstalled()) {
    std::cout << "Runtime was not found, quitting app" << std::endl;
    return -1;
  } else {
    const char* runtime_path = vr::VR_RuntimePath();
    std::cout << "Runtime correctly installed at '" << runtime_path << "'" << std::endl;
  }

  vr::HmdError error;
  ivrSystem = vr::VR_Init(&error, vr::VRApplication_Other);
  std::cout << "VR Init status: " << vr::VR_GetVRInitErrorAsSymbol(error) << std::endl;
  std::cout << "Pointer to the IVRSystem is " << ivrSystem << std::endl;

  catalogDevices(ivrSystem, devices, false);
  catalogControllers(devices, true);

  while (true) {
    std::cout << "> ";
    std::string input;
    std::cin >> input;
    
    if (input == "refresh") {
      catalogDevices(ivrSystem, devices, true);
      catalogControllers(devices, true);
    } else if (input == "start") {
      rclcpp::spin(node);
    } else if (input == "exit") {
      break;
    } else if (input == "help") {
      std::cout << "Available commands:" << std::endl;
      std::cout << "  help - shows this" << std::endl;
      std::cout << "  refresh - requeries system for VR devices" << std::endl;
      std::cout << "  start - starts broadcasting VR data" << std::endl;
      std::cout << "  exit - quits program" << std::endl;
    } else {
      std::cout << "Command not found, type 'help' for help" << std::endl;
    }
  }
  
  vr::VR_Shutdown();
  rclcpp::shutdown();

  return 0;
}
