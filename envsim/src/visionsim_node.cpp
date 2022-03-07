#include <ros/ros.h>

#include "agiros_msgs/QuadState.h"
#include "envsim/visionsim.hpp"
#include "nav_msgs/Odometry.h"
#include "rosgraph_msgs/Clock.h"

using namespace agi;

VisionSim::VisionSim(const ros::NodeHandle &nh, const ros::NodeHandle &pnh)
  : nh_(nh), pnh_(pnh), frame_id_(0) {
  // Logic subscribers
  reset_sub_ = pnh_.subscribe("reset_sim", 1, &VisionSim::resetCallback, this);
  reload_quad_sub_ = pnh_.subscribe("reload_quadrotor", 1,
                                    &VisionSim::loadQuadrotorCallback, this);

  // Publishers
  clock_pub_ = nh_.advertise<rosgraph_msgs::Clock>("/clock", 1);
  odometry_pub_ = pnh_.advertise<nav_msgs::Odometry>("groundtruth/odometry", 1);
  state_pub_ = pnh_.advertise<agiros_msgs::QuadState>("groundtruth/state", 1);

  image_transport::ImageTransport it(pnh_);

  obstacle_pub_ =
    pnh_.advertise<std_msgs::Float32MultiArray>("groundtruth/obstacles", 1);

  image_pub_ = it.advertise("unity/image", 1);
  depth_pub_ = it.advertise("unity/depth", 1);
  opticalflow_pub_ = it.advertise("unity/opticalflow", 1);

  ros_pilot_.getQuadrotor(&quad_);
  simulator_.updateQuad(quad_);
  simulator_.addModel(ModelInit{quad_});
  simulator_.addModel(ModelMotor{quad_});

  bool use_bem = false;
  std::string low_level_ctrl;
  pnh_.getParam("render", render_);
  pnh_.getParam("agi_param_dir", agi_param_directory_);
  pnh_.getParam("ros_param_dir", ros_param_directory_);
  pnh_.getParam("use_bem_propeller_model", use_bem);
  pnh_.getParam("real_time_factor", real_time_factor_);
  pnh_.getParam("low_level_controller", low_level_ctrl);
  const bool got_directory =
    pnh_.getParam("agi_param_dir", agi_param_directory_);

  if (use_bem) {
    std::shared_ptr<ModelPropellerBEM> bem_model =
      simulator_.addModel(ModelPropellerBEM{quad_});
    std::shared_ptr<ModelBodyDrag> drag_model =
      simulator_.addModel(ModelBodyDrag{quad_});
    std::string bem_quad;
    const bool got_bem_quad = pnh_.getParam("bem_quad", bem_quad);
    if (got_directory && got_bem_quad) {
      fs::path bem_param_file = agi_param_directory_ + "/quads/" + bem_quad;
      Yaml node{bem_param_file};
      bem_model->setParameters(node);
      drag_model->setParameters(node);
    } else {
      ROS_WARN("Could not load BEM parameters!");
    }
  } else {
    std::cout << "Not using BEM to model thrust" << std::endl;
    simulator_.addModel(ModelThrustTorqueSimple{quad_});
  }
  simulator_.addModel(ModelRigidBody{quad_});


  if (!simulator_.setLowLevelController(low_level_ctrl)) {
    ROS_WARN("Could not set low level controller!");
  }
  std::shared_ptr<LowLevelControllerBase> low_level_ctrl_ptr =
    simulator_.getLowLevelController();
  ROS_INFO("Setting LLC param dir to %s", agi_param_directory_.c_str());
  low_level_ctrl_ptr->setParamDir(agi_param_directory_);

  //
  std::cout << "creating vision environment" << std::endl;
  std::string cfg_path = getenv("FLIGHTMARE_PATH") +
                         std::string("/flightpy/configs/vision/config.yaml");
  YAML::Node env_cfg = YAML::LoadFile(cfg_path);
  cmd_.setZeros();
  quad_ptr_ = std::make_shared<flightlib::Quadrotor>();
  int env_id = 0;
  std::cout << env_cfg << std::endl;
  // vision_env_ptr_ = std::make_unique<flightlib::VisionEnv>(cfg_path, env_id);
  // if (render_) {
  //   std::string camera_config = ros_param_directory_ + "/camera_config.yaml";
  //   if (!(std::filesystem::exists(camera_config))) {
  //     ROS_ERROR("Configuration file [%s] does not exists.",
  //               camera_config.c_str());
  //   }
  //   YAML::Node cfg_node = YAML::LoadFile(camera_config);
  //   vision_env_ptr_->configCamera(cfg_node);
  //   vision_env_ptr_->setUnity(render_);
  //   vision_env_ptr_->connectUnity();
  // }
  ros::WallDuration(5.0).sleep();
  std::cout << env_cfg << std::endl;

  t_start_ = ros::WallTime::now();
  sim_thread_ = std::thread(&VisionSim::simLoop, this);
}

VisionSim::~VisionSim() {
  if (sim_thread_.joinable()) sim_thread_.join();
  if (render_thread_.joinable()) render_thread_.join();
}

void VisionSim::resetCallback(const std_msgs::EmptyConstPtr &msg) {
  ROS_INFO("Resetting simulator!");
  QuadState reset_state;
  {
    const std::lock_guard<std::mutex> lock(sim_mutex_);
    simulator_.reset(false);
    simulator_.setCommand(Command(0.0, 0.0, Vector<3>::Zero()));
    simulator_.getState(&reset_state);
  }

  reset_state.t += t_start_.toSec();
}

void VisionSim::loadQuadrotorCallback(const std_msgs::StringConstPtr &msg) {
  // this changes the simulated quadrotor, the controllers or the pilot are not
  // aware of the change
  ROS_INFO("Reloading quadrotor from [%s]", msg->data.c_str());

  Quadrotor quad;
  if (!quad.load(msg->data)) {
    ROS_FATAL("Could not load quadrotor.");
    ros::shutdown();
    return;
  }

  {
    const std::lock_guard<std::mutex> lock(sim_mutex_);
    if (!simulator_.updateQuad(quad)) {
      ROS_FATAL("Failed to update quadrotor.");
      ros::shutdown();
      return;
    }
  }
}


void VisionSim::simLoop() {
  while (ros::ok()) {
    ros::WallTime t_start_sim = ros::WallTime::now();
    QuadState quad_state;
    {
      const std::lock_guard<std::mutex> lock(sim_mutex_);
      simulator_.getState(&quad_state);
    }

    // we add an offset to have realistic timestamps
    Scalar sim_time = quad_state.t;
    quad_state.t += t_start_.toSec();

    rosgraph_msgs::Clock curr_time;
    curr_time.clock.fromSec(quad_state.t);
    clock_pub_.publish(curr_time);

    // sleep for 1ms
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    publishState(quad_state);

    ros_pilot_.getPilot().odometryCallback(quad_state);

    Command cmd = ros_pilot_.getCommand();
    cmd.t -= t_start_.toSec();
    if (cmd.valid()) {
      {
        const std::lock_guard<std::mutex> lock(sim_mutex_);
        simulator_.setCommand(cmd);
      }
    } else {
      Command zero_cmd;
      zero_cmd.t = sim_time;  // quad_state.t;
      zero_cmd.thrusts.setZero();
      {
        const std::lock_guard<std::mutex> lock(sim_mutex_);
        simulator_.setCommand(zero_cmd);
      }
    }
    {
      const std::lock_guard<std::mutex> lock(sim_mutex_);
      if (!simulator_.run(sim_dt_))
        ROS_WARN_THROTTLE(1.0, "Simulation failed!");
    }
    // Render here stuff
    if (render_) {
      if ((step_counter_ + 1) % render_every_n_steps_ == 0) {
        publishImages(quad_state);
        step_counter_ = 0;
      } else {
        step_counter_ += 1;
      }
    }

    // simulate dynamic obstacles
    // std::vector<std::shared_ptr<flightlib::UnityObject>> dynamic_objects =
    //   vision_env_ptr_->getDynamicObjects();
    // for (int i = 0; i < int(dynamic_objects.size()); i++) {
    //   dynamic_objects[i]->run(sim_dt_);
    // }
    // publishObstacles(quad_state);

    Scalar sleep_time = 1.0 / real_time_factor_ * sim_dt_ -
                        (ros::WallTime::now() - t_start_sim).toSec();
    ros::WallDuration(std::max(sleep_time, 0.0)).sleep();
  }
}

void VisionSim::publishState(const QuadState &state) {
  agiros_msgs::QuadState msg_state;
  msg_state.header.frame_id = "world";
  msg_state.header.stamp = ros::Time(state.t);
  msg_state.t = state.t;
  msg_state.pose.position = toRosPoint(state.p);
  msg_state.pose.orientation = toRosQuaternion(state.q());
  msg_state.velocity.linear = toRosVector(state.v);
  msg_state.velocity.angular = toRosVector(state.w);
  msg_state.acceleration.linear = toRosVector(state.a);
  msg_state.acceleration.angular = toRosVector(state.tau);

  nav_msgs::Odometry msg_odo;
  msg_odo.header.frame_id = "world";
  msg_odo.header.stamp = ros::Time(state.t);
  msg_odo.pose.pose = msg_state.pose;
  msg_odo.twist.twist = msg_state.velocity;

  odometry_pub_.publish(msg_odo);
  state_pub_.publish(msg_state);
}

void VisionSim::publishObstacles(const QuadState &state) {
  // flightlib::QuadState unity_quad_state;
  // unity_quad_state.setZero();
  // unity_quad_state.p = state.p.cast<flightlib::Scalar>();
  // unity_quad_state.qx = state.qx.cast<flightlib::Scalar>();

  // vision_env_ptr_->getQuadrotor()->setState(unity_quad_state);

  // //
  // std_msgs::Float32MultiArray msg_obstacle_state;
  // num_detected_obstacles_ = vision_env_ptr_->getNumDetectedObstacles();
  // flightlib::Vector<> obstacle_state;
  // obstacle_state.resize(3 * num_detected_obstacles_);
  // vision_env_ptr_->getObstacleState(obstacle_state);


  // for (int i = 0; i < obstacle_state.size(); i++) {
  //   msg_obstacle_state.data.push_back(obstacle_state[i]);
  // }
  // obstacle_pub_.publish(msg_obstacle_state);
}


void VisionSim::publishImages(const QuadState &state) {
  // sensor_msgs::ImagePtr rgb_msg;
  // frame_id_ += 1;
  // // render the frame
  // flightlib::QuadState unity_quad_state;
  // unity_quad_state.setZero();
  // unity_quad_state.p = state.p.cast<flightlib::Scalar>();
  // unity_quad_state.qx = state.qx.cast<flightlib::Scalar>();

  // std::shared_ptr<flightlib::Quadrotor> unity_quad =
  //   vision_env_ptr_->getQuadrotor();
  // unity_quad->setState(unity_quad_state);


  // vision_env_ptr_->updateUnity(frame_id_);

  // // Warning, delay
  // cv::Mat img, depth, of;

  // // RGB Image
  // unity_quad->getCameras()[0]->getRGBImage(img);
  // rgb_msg = cv_bridge::CvImage(std_msgs::Header(), "bgr8", img).toImageMsg();
  // rgb_msg->header.stamp = ros::Time(state.t);
  // image_pub_.publish(rgb_msg);


  // // Depth Image
  // unity_quad->getCameras()[0]->getDepthMap(depth);
  // sensor_msgs::ImagePtr depth_msg =
  //   cv_bridge::CvImage(std_msgs::Header(), "32FC1", depth).toImageMsg();
  // depth_msg->header.stamp = ros::Time(state.t);
  // depth_pub_.publish(depth_msg);

  // // Optical Flow
  // unity_quad->getCameras()[0]->getOpticalFlow(of);
  // sensor_msgs::ImagePtr of_msg =
  //   cv_bridge::CvImage(std_msgs::Header(), "bgr8", of).toImageMsg();
  // of_msg->header.stamp = ros::Time(state.t);
  // opticalflow_pub_.publish(of_msg);
}


int main(int argc, char **argv) {
  ros::init(argc, argv, "visionsim_node");

  VisionSim vision_sim;

  ros::spin();
  return 0;
}