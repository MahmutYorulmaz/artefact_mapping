#include "artefact-mapping/object-tracking-pipeline.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <geometry_msgs/PointStamped.h>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <aslam/triangulation/triangulation.h>
#include <cv_bridge/cv_bridge.h>
#include <vi-map/sensor-manager.h>
#include <vi-map/sensor-utils.h>
#include <minkindr_conversions/kindr_tf.h>

DEFINE_int64(object_tracker_detection_period, 20,
             "Number of frames to skip between object detections");
DEFINE_int64(object_tracker_pose_buffer_length, 600,
             "Time of buffered poses.");
DEFINE_int64(image_topic_buffer_size, 200, "Size of image topic buffer.");

DEFINE_string(object_tracker_image_topic, "/camera/color/image_raw",
              "Ros topic on which the object detection and tracking happens");
DEFINE_string(sensor_calibration_file, "share/camchain.yaml",
              "Path to sensor calibration yaml.");
DEFINE_string(sensor_tf_frame, "/blackfly_right_optical_link",
              "Camera TF frame.");
DEFINE_string(odom_tf_frame, "/odom", "Odometry TF frame.");

DEFINE_string(map_tf_frame, "/map_tf_frame", "Map TF frame.");

DEFINE_bool(publish_debug_images, false,
            "Whether to publish the debug image with tracking information.");

ObjectTrackingPipeline::ObjectTrackingPipeline(ros::NodeHandle &node_handle)
    : nh_(node_handle), it_(node_handle),
      tracker_(FLAGS_object_tracker_detection_period) {
  image_subscriber_ =
      it_.subscribe(FLAGS_object_tracker_image_topic,
                    FLAGS_image_topic_buffer_size,
                    &ObjectTrackingPipeline::imageCallback, this);
  if (FLAGS_publish_debug_images) {
      debug_image_publisher_ = it_.advertise("/artefact_mapping/debug_image", 1);
  }

  landmark_publisher_ = nh_.advertise<geometry_msgs::PointStamped>("/W_landmark", 1);
  artefact_publisher_ = nh_.advertise<artefact_msgs::Artefact>("/W_artefact", 1);
  tf_listener_ = new tf::TransformListener(
      ros::Duration(FLAGS_object_tracker_pose_buffer_length));


  // Load sensors.
  CHECK(!FLAGS_sensor_calibration_file.empty())
      << "[Object Tracking] No sensor calibration file was provided!";
  if (!sensor_manager_.deserializeFromFile(FLAGS_sensor_calibration_file)) {
    LOG(FATAL)
        << "[Object Tracking] Failed to read the sensor calibration from '"
        << FLAGS_sensor_calibration_file << "'!";
  }
  CHECK(vi_map::getSelectedNCamera(sensor_manager_))
      << "[Object Tracking] The sensor calibration does not contain a NCamera!";
}

void ObjectTrackingPipeline::imageCallback(
    const sensor_msgs::ImageConstPtr &image_message) {

  cv_bridge::CvImagePtr cv_ptr =
  	//ROS_INFO(image_message->encoding);
      cv_bridge::toCvCopy(image_message, sensor_msgs::image_encodings::BGR8);
  // Image message seems to be misconfigured
  // when requesting rgb the image we get is actually bgr
  //cv_ptr->encoding = "bgr8";

  tracker_.processFrame(cv_ptr->image, image_message->header.stamp);

  std::vector<Observation> observations;
  while (tracker_.getFinishedTrack(&observations)) {
    VLOG(1) << "Triangulate track with size " << observations.size();
    triangulateTracks(observations);
  }

  if (FLAGS_publish_debug_images) {
    tracker_.debugDrawTracks(&cv_ptr->image);
    debug_image_publisher_.publish(cv_ptr->toImageMsg());
  }
}

void ObjectTrackingPipeline::triangulateTracks(
    const std::vector<Observation> &observations) {
  aslam::TransformationVector T_W_Bs;
  Aligned<std::vector, Eigen::Vector2d> normalized_measurements;
  normalized_measurements.reserve(observations.size());
  std::vector<int> class_labels;
  class_labels.reserve(observations.size());

  const aslam::Camera::ConstPtr camera =
      vi_map::getSelectedNCamera(sensor_manager_)->getCameraShared(0u);
  CHECK(camera);
  aslam::Transformation T_B_C =
      vi_map::getSelectedNCamera(sensor_manager_)->get_T_C_B(0u).inverse();
  
  tf::StampedTransform transform_of2cf;
  for (const Observation &observation : observations) {
    VLOG(2) << "Add observation with ts " << observation.timestamp_.sec
            << "." << observation.timestamp_.nsec;

    tf::StampedTransform transform;
    try {
      tf_listener_->lookupTransform(FLAGS_odom_tf_frame, FLAGS_sensor_tf_frame,
          observation.timestamp_, transform);
    }
    catch (tf::TransformException ex) {
      ROS_ERROR("%s", ex.what());
      return;
    }

    aslam::Transformation T_W_B;
    tf::transformTFToKindr(transform, &T_W_B);

    // Obtain the normalized keypoint measurements.;
    Eigen::Vector3d C_ray;
    Eigen::Vector2d centroid = observation.getCentroid();
    if(centroid[0] > camera->imageWidth()|| centroid[1] > camera->imageHeight()) {
      continue;
      VLOG(1) << "Observation outside camera " << centroid;
    }

    camera->backProject3(centroid, &C_ray);
    Eigen::Vector2d normalized_measurement = C_ray.head<2>() / C_ray[2];

    T_W_Bs.emplace_back(T_W_B);
    normalized_measurements.emplace_back(normalized_measurement);

    if (observation.getClass() != -1) {
      class_labels.emplace_back(observation.getClass());
    }
      
    try {
      tf_listener_->lookupTransform(FLAGS_map_tf_frame, FLAGS_odom_tf_frame,
      observation.timestamp_, transform_of2cf);
    }
    catch (tf::TransformException ex) {
      ROS_ERROR("%s", ex.what());
      return;
    }

  }

  Eigen::Vector3d W_landmark;
  VLOG(200) << "Assembled triangulation data.";

  // Triangulate the landmark.
  CHECK_EQ(normalized_measurements.size(), T_W_Bs.size());
  aslam::TriangulationResult triangulation_result =
      aslam::linearTriangulateFromNViews(normalized_measurements, T_W_Bs, T_B_C,
                                         &W_landmark);
  VLOG(1) << "Triangulated landmark at " << W_landmark;


  geometry_msgs::PointStamped landmark_msg_of;
  landmark_msg_of.header.frame_id = FLAGS_odom_tf_frame;
  landmark_msg_of.header.stamp = observations.back().timestamp_;
  landmark_msg_of.point.x = W_landmark[0];
  landmark_msg_of.point.y = W_landmark[1];
  landmark_msg_of.point.z = W_landmark[2];

  geometry_msgs::PointStamped landmark_msg_mf;
  //std::string map = "map"; 
  tf_listener_->transformPoint(FLAGS_map_tf_frame, landmark_msg_of, landmark_msg_mf);

  landmark_publisher_.publish(landmark_msg_mf);

  artefact_msgs::Artefact artefact_msg;
  artefact_msg.header = landmark_msg_mf.header;
  artefact_msg.landmark = landmark_msg_mf;
  std::sort(class_labels.begin(), class_labels.end()); // Report the most observed object class.
  artefact_msg.class_label = (unsigned) class_labels[(int) class_labels.size()/2];
  artefact_msg.quality = 0; // Placeholder, not implemented yet!
  artefact_publisher_.publish(artefact_msg);

  std::ifstream read_in("~/home/yoruseer/artifacts.csv", std::ofstream::app); //csv file saved /home/yoruseer/.ros/artifacts.csv

  if(!read_in) {
    artifacts.open ("~/home/yoruseer/artifacts.csv");
    //if (!artifacts.is_open()) {
    //}
    artifacts<<"timestamp"<<","<<"class"<<","<<"x"<<","<<"y"<<","<<"z"<<"\n";
    ROS_INFO("created csv");
    artifacts.close();
  }
  else {
    artifacts.open ("~/home/yoruseer/artifacts.csv", std::ofstream::app);
    //if (!artifacts.is_open()) {
    //}

    artifacts<<landmark_msg_mf.header.stamp<<","<<artefact_msg.class_label<<","<<landmark_msg_mf.point.x<<","<<landmark_msg_mf.point.y<<","<<landmark_msg_mf.point.z<<"\n";
    artifacts.close();
  }

}
