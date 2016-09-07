/*
 * Copyright 2014 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <sstream>
#include <string>

#include <tango-gl/conversions.h>
#include <tango_support_api.h>

#include "tango-augmented-reality/augmented_reality_app.h"

namespace {
const int kVersionStringLength = 128;
// The minimum Tango Core version required from this application.
const int kTangoCoreMinimumVersion = 9377;

// Far clipping plane of the AR camera.
const float kArCameraNearClippingPlane = 0.1f;
const float kArCameraFarClippingPlane = 100.0f;

int CombineSensorRotation(int activity_orientation, int sensor_orientation) {
  int sensor_orientation_n = 0;
  switch (sensor_orientation) {
    case 90:
      sensor_orientation_n = 1;
      break;
    case 180:
      sensor_orientation_n = 2;
      break;
    case 270:
      sensor_orientation_n = 3;
      break;
    default:
      sensor_orientation_n = 0;
      break;
  }

  int ret = activity_orientation - sensor_orientation_n;
  if (ret < 0) {
    ret += 4;
  }
  return (ret % 4);
}

// This function routes onTangoEvent callbacks to the application object for
// handling.
//
// @param context, context will be a pointer to a AugmentedRealityApp
//        instance on which to call callbacks.
// @param event, TangoEvent to route to onTangoEventAvailable function.
void onTangoEventAvailableRouter(void* context, const TangoEvent* event) {
  tango_augmented_reality::AugmentedRealityApp* app =
      static_cast<tango_augmented_reality::AugmentedRealityApp*>(context);
  app->onTangoEventAvailable(event);
}

// This function routes texture callbacks to the application object for
// handling.
//
// @param context, context will be a pointer to a AugmentedRealityApp
//        instance on which to call callbacks.
// @param id, id of the updated camera..
void onTextureAvailableRouter(void* context, TangoCameraId id) {
  tango_augmented_reality::AugmentedRealityApp* app =
      static_cast<tango_augmented_reality::AugmentedRealityApp*>(context);
  app->onTextureAvailable(id);
}

// This function routes onPoseAvailable callbacks to the application object for
// handling.
//
// @param context, context will be a pointer to a AreaLearningApp
//        instance on which to call callbacks.
// @param pose, pose data to route to onPoseAvailable function.
void onPoseAvailableRouter(void* context, const TangoPoseData* pose) {
  tango_augmented_reality::AugmentedRealityApp* app =
      static_cast<tango_augmented_reality::AugmentedRealityApp*>(context);
  app->onPoseAvailable(pose);
  }
}  // namespace

namespace tango_augmented_reality {

void AugmentedRealityApp::onTangoEventAvailable(const TangoEvent* event) {
  std::lock_guard<std::mutex> lock(tango_event_mutex_);
  tango_event_data_.UpdateTangoEvent(event);
}

void AugmentedRealityApp::onTextureAvailable(TangoCameraId id) {
  if (id == TANGO_CAMERA_COLOR) {
    RequestRender();
  }
}

void AugmentedRealityApp::onPoseAvailable(const TangoPoseData* pose) {
  //std::lock_guard<std::mutex> lock(pose_mutex_);
/*
    if (pose->frame.base == TANGO_COORDINATE_FRAME_AREA_DESCRIPTION &&
             pose->frame.target == TANGO_COORDINATE_FRAME_DEVICE) {
        main_scene_.RotateEarthByPose(*pose);
        main_scene_.RotateMoonByPose(*pose);
        main_scene_.TranslateMoonByPose(*pose);
    }
    //LOGD("AugmentedRealityApp::onPoseAvailable: updated pose");
    */
}


void AugmentedRealityApp::OnCreate(JNIEnv* env, jobject activity,
                                   int activity_orientation,
                                   int sensor_orientation) {
  // Check the installed version of the TangoCore.  If it is too old, then
  // it will not support the most up to date features.
  int version;
  TangoErrorType err = TangoSupport_GetTangoVersion(env, activity, &version);
  if (err != TANGO_SUCCESS || version < kTangoCoreMinimumVersion) {
    LOGE("AugmentedRealityApp::OnCreate, Tango Core version is out of date.");
    std::exit(EXIT_SUCCESS);
  }

  // We want to be able to trigger rendering on demand in our Java code.
  // As such, we need to store the activity we'd like to interact with and the
  // id of the method we'd like to call on that activity.
  calling_activity_obj_ = env->NewGlobalRef(activity);
  jclass cls = env->GetObjectClass(activity);
  on_demand_render_ = env->GetMethodID(cls, "requestRender", "()V");

  is_service_connected_ = false;
  is_texture_id_set_ = false;

  activity_rotation_ = activity_orientation;
  sensor_rotation_ = sensor_orientation;
}

void AugmentedRealityApp::OnTangoServiceConnected(JNIEnv* env,
                                                  jobject iBinder) {
  TangoErrorType ret = TangoService_setBinder(env, iBinder);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: Failed to set Tango binder with"
        "error code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }

  TangoSetupConfig();
  TangoConnectCallbacks();
  TangoConnect();

  is_service_connected_ = true;
}

void AugmentedRealityApp::OnDestroy() {
  JNIEnv* env;
  java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  env->DeleteGlobalRef(calling_activity_obj_);

  calling_activity_obj_ = nullptr;
  on_demand_render_ = nullptr;
}

void AugmentedRealityApp::TangoSetupConfig() {
  // Here, we'll configure the service to run in the way we'd want. For this
  // application, we'll start from the default configuration
  // (TANGO_CONFIG_DEFAULT). This enables basic motion tracking capabilities.
  tango_config_ = TangoService_getConfig(TANGO_CONFIG_DEFAULT);
  if (tango_config_ == nullptr) {
    LOGE("AugmentedRealityApp: Failed to get default config form");
    std::exit(EXIT_SUCCESS);
  }

  // Set auto-recovery for motion tracking as requested by the user.
  int ret =
      TangoConfig_setBool(tango_config_, "config_enable_auto_recovery", true);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: config_enable_auto_recovery() failed with error"
        "code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }

  // Enable color camera from config.
  ret = TangoConfig_setBool(tango_config_, "config_enable_color_camera", true);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: config_enable_color_camera() failed with error"
        "code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }

  // Low latency IMU integration enables aggressive integration of the latest
  // inertial measurements to provide lower latency pose estimates. This will
  // improve the AR experience.
  ret = TangoConfig_setBool(tango_config_,
                            "config_enable_low_latency_imu_integration", true);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: config_enable_low_latency_imu_integration() "
        "failed with error code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }

/*
  // Drift correction allows motion tracking to recover after it loses tracking.
  //
  // The drift corrected pose is is available through the frame pair with
  // base frame AREA_DESCRIPTION and target frame DEVICE.
  ret = TangoConfig_setBool(tango_config_, "config_enable_drift_correction",
                            true);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: enabling config_enable_drift_correction "
        "failed with error code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }
*/
/*
    ret = TangoConfig_setBool(tango_config_, "config_enable_learning_mode",
                              true);
    if (ret != TANGO_SUCCESS) {
      LOGE(
          "AugmentedRealityApp: enabling config_enable_learning_mode "
          "failed with error code: %d",
          ret);
      std::exit(EXIT_SUCCESS);
    }
*/
    ret = TangoConfig_setString(tango_config_, "config_load_area_description_UUID",
                              "4fb33503-aa1e-4174-9469-c62d3378c27b");
    if (ret != TANGO_SUCCESS) {
      LOGE(
          "AugmentedRealityApp: enabling config_load_area_description_UUID "
          "failed with error code: %d",
          ret);
      std::exit(EXIT_SUCCESS);
    }

    ret = TangoConfig_setBool(tango_config_, "config_enable_debug_logging",
                              false);
    if (ret != TANGO_SUCCESS) {
      LOGE(
          "AugmentedRealityApp: enabling config_enable_debug_logging "
          "failed with error code: %d",
          ret);
      std::exit(EXIT_SUCCESS);
    }

        // Setting up the frame pair for the onPoseAvailable callback.
     TangoCoordinateFramePair pairs[3] = {
         {TANGO_COORDINATE_FRAME_START_OF_SERVICE,
          TANGO_COORDINATE_FRAME_DEVICE},
         {TANGO_COORDINATE_FRAME_AREA_DESCRIPTION,
          TANGO_COORDINATE_FRAME_DEVICE},
         {TANGO_COORDINATE_FRAME_AREA_DESCRIPTION,
          TANGO_COORDINATE_FRAME_START_OF_SERVICE}};

        // Attach onPoseAvailable callback.
        // The callback will be called after the service is connected.
        ret = TangoService_connectOnPoseAvailable(3, pairs, onPoseAvailableRouter);
        if (ret != TANGO_SUCCESS) {
          LOGE(
              "AreaLearningApp: Failed to connect to pose callback with error"
              "code: %d",
              ret);
          std::exit(EXIT_SUCCESS);
        }
/*
  ret = TangoConfig_setInt32(tango_config_, "config_dataset_recording_mode",
                            2);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: enabling config_enable_dataset_recording "
        "failed with error code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }

  ret = TangoConfig_setBool(tango_config_, "config_enable_dataset_recording",
                            true);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: enabling config_enable_dataset_recording "
        "failed with error code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }
*/
  // Get TangoCore version string from service.
  char tango_core_version[kVersionStringLength];
  ret = TangoConfig_getString(tango_config_, "tango_service_library_version",
                              tango_core_version, kVersionStringLength);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: get tango core version failed with error"
        "code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }
  tango_core_version_string_ = tango_core_version;
}

void AugmentedRealityApp::TangoConnectCallbacks() {
  // Connect color camera texture.
  TangoErrorType ret = TangoService_connectOnTextureAvailable(
      TANGO_CAMERA_COLOR, this, onTextureAvailableRouter);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: Failed to connect texture callback with error"
        "code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }

  // Attach onEventAvailable callback.
  // The callback will be called after the service is connected.
  ret = TangoService_connectOnTangoEvent(onTangoEventAvailableRouter);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: Failed to connect to event callback with error"
        "code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }
}

// Connect to Tango Service, service will start running, and
// pose can be queried.
void AugmentedRealityApp::TangoConnect() {
  TangoErrorType ret = TangoService_connect(this, tango_config_);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AugmentedRealityApp: Failed to connect to the Tango service with"
        "error code: %d",
        ret);
    std::exit(EXIT_SUCCESS);
  }

  // Initialize TangoSupport context.
  TangoSupport_initializeLibrary();
}

void AugmentedRealityApp::OnPause() {
  TangoDisconnect();
  DeleteResources();
}

void AugmentedRealityApp::TangoDisconnect() {
  // When disconnecting from the Tango Service, it is important to make sure to
  // free your configuration object. Note that disconnecting from the service,
  // resets all configuration, and disconnects all callbacks. If an application
  // resumes after disconnecting, it must re-register configuration and
  // callbacks with the service.
  is_service_connected_ = false;
  TangoConfig_free(tango_config_);
  tango_config_ = nullptr;
  TangoService_disconnect();
}

void AugmentedRealityApp::OnSurfaceCreated(AAssetManager* aasset_manager) {
  main_scene_.InitGLContent(aasset_manager, activity_rotation_,
                            sensor_rotation_);
}

void AugmentedRealityApp::OnSurfaceChanged(int width, int height) {
  viewport_width_ = width;
  viewport_height_ = height;
  UpdateViewporAndProjectionMatrix();
}

void AugmentedRealityApp::UpdateViewporAndProjectionMatrix() {
  // Query intrinsics for the color camera from the Tango Service, because we
  // want to match the virtual render camera's intrinsics to the physical
  // camera, we will compute the actually projection matrix and the view port
  // ratio for the render.
  float image_plane_ratio = 0.0f;
  if (is_service_connected_) {
    int ret = TangoService_getCameraIntrinsics(TANGO_CAMERA_COLOR,
                                               &color_camera_intrinsics_);
    if (ret != TANGO_SUCCESS) {
      LOGE(
          "AugmentedRealityApp: Failed to get camera intrinsics with error"
          "code: %d",
          ret);
    }

    float image_width = static_cast<float>(color_camera_intrinsics_.width);
    float image_height = static_cast<float>(color_camera_intrinsics_.height);
    float fx = static_cast<float>(color_camera_intrinsics_.fx);
    float fy = static_cast<float>(color_camera_intrinsics_.fy);
    float cx = static_cast<float>(color_camera_intrinsics_.cx);
    float cy = static_cast<float>(color_camera_intrinsics_.cy);

    glm::mat4 projection_mat_ar;
    if (viewport_width_ > viewport_height_) {
      projection_mat_ar = tango_gl::Camera::ProjectionMatrixForCameraIntrinsics(
          image_width, image_height, fx, fy, cx, cy, kArCameraNearClippingPlane,
          kArCameraFarClippingPlane);
      image_plane_ratio = image_height / image_width;
    } else {
      projection_mat_ar = tango_gl::Camera::ProjectionMatrixForCameraIntrinsics(
          image_height, image_width, fy, fx, cy, cx, kArCameraNearClippingPlane,
          kArCameraFarClippingPlane);
      image_plane_ratio = image_width / image_height;
    }
    main_scene_.SetProjectionMatrix(projection_mat_ar);
  }

  float screen_ratio = static_cast<float>(viewport_height_) /
                       static_cast<float>(viewport_width_);
  // In the following code, we place the view port at (0, 0) from the bottom
  // left corner of the screen. By placing it at (0,0), the view port may not
  // be exactly centered on the screen. However, this won't affect AR
  // visualization as the correct registration of AR objects relies on the
  // aspect ratio of the screen and video overlay, but not the position of the
  // view port.
  //
  // To place the view port in the center of the screen, please use following
  // code:
  //
  // if (image_plane_ratio < screen_ratio) {
  //   glViewport(-(h / image_plane_ratio - w) / 2, 0,
  //              h / image_plane_ratio, h);
  // } else {
  //   glViewport(0, -(w * image_plane_ratio - h) / 2, w,
  //              w * image_plane_ratio);
  // }

  if (image_plane_ratio < screen_ratio) {
    glViewport(0, 0, viewport_height_ / image_plane_ratio, viewport_height_);
  } else {
    glViewport(0, 0, viewport_width_, viewport_width_ * image_plane_ratio);
  }
}

void AugmentedRealityApp::OnDeviceRotationChanged(int activity_orientation,
                                                  int sensor_orientation) {
  activity_rotation_ = activity_orientation;
  sensor_rotation_ = sensor_orientation;
  main_scene_.SetVideoOverlayOrientation(activity_orientation,
                                         sensor_orientation);
}

void AugmentedRealityApp::OnDrawFrame() {
  if (is_service_connected_ && !is_texture_id_set_) {
    is_texture_id_set_ = true;
    UpdateViewporAndProjectionMatrix();
  }

  double video_overlay_timestamp;
  TangoErrorType status = TangoService_updateTextureExternalOes(
      TANGO_CAMERA_COLOR, main_scene_.GetVideoOverlayTextureId(),
      &video_overlay_timestamp);

  if (status == TANGO_SUCCESS) {
    // When drift correction mode is enabled in config file, we need to query
    // the device with respect to Area Description pose in order to use the
    // drift corrected pose.
    //
    // Note that if you don't want to use the drift corrected pose, the
    // normal device with respect to start of service pose is still available.
    int combined_orientation =
        CombineSensorRotation(activity_rotation_, sensor_rotation_);

    TangoDoubleMatrixTransformData matrix_transform;

    status = TangoSupport_getDoubleMatrixTransformAtTime(
        video_overlay_timestamp, TANGO_COORDINATE_FRAME_AREA_DESCRIPTION,
        TANGO_COORDINATE_FRAME_CAMERA_COLOR, TANGO_SUPPORT_ENGINE_OPENGL,
        TANGO_SUPPORT_ENGINE_OPENGL,
        static_cast<TangoSupportDisplayRotation>(combined_orientation),
        &matrix_transform);

    if (matrix_transform.status_code == TANGO_POSE_VALID) {
      {
        std::lock_guard<std::mutex> lock(transform_mutex_);
        UpdateTransform(matrix_transform.matrix, video_overlay_timestamp);
      }

      TangoPoseData pose;
      TangoSupport_getPoseAtTime(
          0.0, TANGO_COORDINATE_FRAME_AREA_DESCRIPTION,
          TANGO_COORDINATE_FRAME_DEVICE, TANGO_SUPPORT_ENGINE_OPENGL,
          static_cast<TangoSupportDisplayRotation>(combined_orientation),
          &pose);

      if (pose.status_code != TANGO_POSE_VALID) {
        LOGE("AugmentedRealityApp: Tango pose is not valid.");
        return;
      }

      main_scene_.RotateEarthByPose(pose);
      main_scene_.RotateMoonByPose(pose);
      main_scene_.TranslateMoonByPose(pose);

      main_scene_.Render(cur_start_service_T_camera_);
    } else {
      // When the pose status is not valid, it indicates the tracking has
      // been lost. In this case, we simply stop rendering.
      //
      // This is also the place to display UI to suggest the user walk
      // to recover tracking.
      LOGE(
          "AugmentedRealityApp: Could not find a valid matrix transform at "
          "time %lf for the color camera; status %d",
          video_overlay_timestamp, matrix_transform.status_code);
    }
  } else {
    LOGE(
        "AugmentedRealityApp: Failed to update video overlay texture with "
        "error code: %d",
        status);
  }
}

void AugmentedRealityApp::DeleteResources() {
  is_texture_id_set_ = false;
  main_scene_.DeleteResources();
}

std::string AugmentedRealityApp::GetTransformString() {
  std::lock_guard<std::mutex> lock(transform_mutex_);
  return transform_string_;
}

std::string AugmentedRealityApp::GetEventString() {
  std::lock_guard<std::mutex> lock(tango_event_mutex_);
  return tango_event_data_.GetTangoEventString().c_str();
}

std::string AugmentedRealityApp::GetVersionString() {
  return tango_core_version_string_.c_str();
}

void AugmentedRealityApp::RequestRender() {
  if (calling_activity_obj_ == nullptr || on_demand_render_ == nullptr) {
    LOGE("Can not reference Activity to request render");
    return;
  }

  // Here, we notify the Java activity that we'd like it to trigger a render.
  JNIEnv* env;
  java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  env->CallVoidMethod(calling_activity_obj_, on_demand_render_);
}

void AugmentedRealityApp::UpdateTransform(const double transform[16],
                                          double timestamp) {
  prev_start_service_T_camera_ = cur_start_service_T_camera_;
  cur_start_service_T_camera_ = glm::make_mat4(transform);
  // Increase pose counter.
  ++transform_counter_;
  prev_timestamp_ = cur_timestamp_;
  cur_timestamp_ = timestamp;
  FormatTransformString();
}

void AugmentedRealityApp::FormatTransformString() {
  const float* transform =
      (const float*)glm::value_ptr(cur_start_service_T_camera_);
  std::stringstream string_stream;
  string_stream.setf(std::ios_base::fixed, std::ios_base::floatfield);
  string_stream.precision(3);
  string_stream << "count: " << transform_counter_
                << ", delta time (ms): " << (cur_timestamp_ - prev_timestamp_)
                << std::endl << "position (m): [" << transform[12] << ", "
                << transform[13] << ", " << transform[14] << "]" << std::endl
                << "rotation matrix: [" << transform[0] << ", " << transform[1]
                << ", " << transform[2] << ", " << transform[4] << ", "
                << transform[5] << ", " << transform[6] << ", " << transform[8]
                << ", " << transform[9] << ", " << transform[10] << "]";
  transform_string_ = string_stream.str();
  string_stream.flush();
}
}  // namespace tango_augmented_reality
