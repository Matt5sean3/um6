/**
 *
 *  \file
 *  \brief      Main entry point for UM6 driver. Handles serial connection
 *              details, as well as all ROS message stuffing, parameters,
 *              topics, etc.
 *  \author     Mike Purvis <mpurvis@clearpathrobotics.com>
 *  \copyright  Copyright (c) 2013, Clearpath Robotics, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Clearpath Robotics, Inc. nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CLEARPATH ROBOTICS, INC. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Please send comments, questions, or patches to code@clearpathrobotics.com
 *
 */
#include <string>

#include "geometry_msgs/Vector3Stamped.h"
#include "ros/ros.h"
#include "sensor_msgs/Imu.h"
#include "serial/serial.h"
#include "std_msgs/Float32.h"
#include "std_msgs/Header.h"
#include "std_msgs/UInt8.h"
#include "um6/comms.h"
#include "um6/registers.h"
#include "um6/Reset.h"

// Required for full GPS support
#include <math.h>
#include "nav_msgs/Odometry.h"

// Don't try to be too clever. Arrival of this message triggers
// us to publish everything we have.
const uint8_t TRIGGER_PACKET = UM6_TEMPERATURE;


/**
 * Function generalizes the process of writing an XYZ vector into consecutive
 * fields in UM6 registers.
 */
template<typename RegT>
void configureVector3(um6::Comms* sensor, const um6::Accessor<RegT>& reg,
                      std::string param, std::string human_name)
{
  if (reg.length != 3)
  {
    throw std::logic_error("configureVector3 may only be used with 3-field registers!");
  }

  if (ros::param::has(param))
  {
    double x, y, z;
    ros::param::get(param + "/x", x);
    ros::param::get(param + "/y", y);
    ros::param::get(param + "/z", z);
    ROS_INFO_STREAM("Configuring " << human_name << " to ("
                    << x << ", " << y << ", " << z << ")");
    reg.set_scaled(0, x);
    reg.set_scaled(1, y);
    reg.set_scaled(2, z);
    if (sensor->sendWaitAck(reg))
    {
      throw std::runtime_error("Unable to configure vector.");
    }
  }
}

/**
 * Function generalizes the process of commanding the UM6 via one of its command
 * registers.
 */
template<typename RegT>
void sendCommand(um6::Comms* sensor, const um6::Accessor<RegT>& reg, std::string human_name)
{
  ROS_INFO_STREAM("Sending command: " << human_name);
  if (!sensor->sendWaitAck(reg))
  {
    throw std::runtime_error("Command to device failed.");
  }
}

uint8_t baudValueToBitSetting(int32_t baud_rate)
{
  const uint8_t UM6_BAUD_9600 = 0x0;
  const uint8_t UM6_BAUD_14400 = 0x1;
  const uint8_t UM6_BAUD_19200 = 0x2;
  const uint8_t UM6_BAUD_38400 = 0x3;
  const uint8_t UM6_BAUD_57600 = 0x4;
  const uint8_t UM6_BAUD_115200 = 0x5;
  switch(baud_rate) //Determine the GPS baud rate
  {
    case 9600:
      return UM6_BAUD_9600;
    case 14400:
      return UM6_BAUD_14400;
    case 19200:
      return UM6_BAUD_19200;
    case 38400:
      return UM6_BAUD_38400;
    case 57600:
      return UM6_BAUD_57600;
    case 115200:
      return UM6_BAUD_115200;
    default: // Throw an error for bad values
      throw std::runtime_error("Invalid Baud Rate");
  }
}

/**
 * Send configuration messages to the UM6, critically, to turn on the value outputs
 * which we require, and inject necessary configuration parameters.
 */
void configureSensor(um6::Comms* sensor)
{
  um6::Registers r;

  int32_t gps_baud;
  ros::param::param<int32_t>("~gps_baud", gps_baud, 9600);
  // Enable outputs we need.
  uint32_t comm_reg = UM6_BROADCAST_ENABLED |
                      UM6_GYROS_PROC_ENABLED | UM6_ACCELS_PROC_ENABLED | UM6_MAG_PROC_ENABLED |
                      UM6_QUAT_ENABLED | UM6_EULER_ENABLED | UM6_COV_ENABLED | UM6_TEMPERATURE_ENABLED |
                      baudValueToBitSetting(115200) << UM6_BAUD_START_BIT |
                      baudValueToBitSetting(gps_baud) << UM6_GPS_BAUD_START_BIT;

  // Provide options for gps data
  bool gps_enable;
  ros::param::param<bool>("~gps_enable", gps_enable, false);
  if(gps_enable) // enable the gps
  {
    ROS_INFO("gps enabled");
    // enable GPS outputs
    comm_reg |= UM6_GPS_POSITION_ENABLED |
                UM6_GPS_REL_POSITION_ENABLED |
                UM6_GPS_COURSE_SPEED_ENABLED |
                UM6_GPS_SAT_SUMMARY_ENABLED |
                UM6_GPS_SAT_DATA_ENABLED;
  }

  r.communication.set(0, comm_reg);
  if (!sensor->sendWaitAck(r.communication))
  {
    throw std::runtime_error("Unable to set communication register.");
  }
  
  // Optionally disable mag and accel updates in the sensor's EKF.
  bool mag_updates, accel_updates;
  ros::param::param<bool>("~mag_updates", mag_updates, true);
  ros::param::param<bool>("~accel_updates", accel_updates, true);
  uint32_t misc_config_reg = UM6_QUAT_ESTIMATE_ENABLED;
  if (mag_updates)
  {
    misc_config_reg |= UM6_MAG_UPDATE_ENABLED;
  }
  else
  {
    ROS_WARN("Excluding magnetometer updates from EKF.");
  }
  if (accel_updates)
  {
    misc_config_reg |= UM6_ACCEL_UPDATE_ENABLED;
  }
  else
  {
    ROS_WARN("Excluding accelerometer updates from EKF.");
  }
  r.misc_config.set(0, misc_config_reg);
  if (!sensor->sendWaitAck(r.misc_config))
  {
    throw std::runtime_error("Unable to set misc config register.");
  }

  // Optionally disable the gyro reset on startup. A user might choose to do this
  // if there's an external process which can ascertain when the vehicle is stationary
  // and periodically call the /reset service.
  bool zero_gyros;
  ros::param::param<bool>("~zero_gyros", zero_gyros, true);
  if (zero_gyros) sendCommand(sensor, r.cmd_zero_gyros, "zero gyroscopes");

  // Configurable vectors.
  configureVector3(sensor, r.mag_ref, "~mag_ref", "magnetic reference vector");
  configureVector3(sensor, r.accel_ref, "~accel_ref", "accelerometer reference vector");
  configureVector3(sensor, r.mag_bias, "~mag_bias", "magnetic bias vector");
  ROS_INFO("D");
  configureVector3(sensor, r.accel_bias, "~accel_bias", "accelerometer bias vector");
  configureVector3(sensor, r.gyro_bias, "~gyro_bias", "gyroscope bias vector");
  ROS_INFO("F");
  if(gps_enable)
  {
    configureVector3(sensor, r.gps_home, "~gps_home", "gps home position vector");
  }
}


bool handleResetService(um6::Comms* sensor,
                        const um6::Reset::Request& req, const um6::Reset::Response& resp)
{
  um6::Registers r;
  if (req.zero_gyros) sendCommand(sensor, r.cmd_zero_gyros, "zero gyroscopes");
  if (req.reset_ekf) sendCommand(sensor, r.cmd_reset_ekf, "reset EKF");
  if (req.set_mag_ref) sendCommand(sensor, r.cmd_set_mag_ref, "set magnetometer reference");
  if (req.set_accel_ref) sendCommand(sensor, r.cmd_set_accel_ref, "set accelerometer reference");
  return true;
}

/**
 * Uses the register accessors to grab data from the IMU, and populate
 * the ROS messages which are output.
 */
void publishMsgs(um6::Registers& r, ros::NodeHandle* n, const std_msgs::Header& header)
{

  static ros::Publisher imu_pub = n->advertise<sensor_msgs::Imu>("imu/data", 1, false);
  static ros::Publisher mag_pub = n->advertise<geometry_msgs::Vector3Stamped>("imu/mag", 1, false);
  static ros::Publisher rpy_pub = n->advertise<geometry_msgs::Vector3Stamped>("imu/rpy", 1, false);
  static ros::Publisher temp_pub = n->advertise<std_msgs::Float32>("imu/temperature", 1, false);

  if (imu_pub.getNumSubscribers() > 0)
  {
    sensor_msgs::Imu imu_msg;
    imu_msg.header = header;

    // IMU outputs [w,x,y,z] NED, convert to [x,y,z,w] ENU
    imu_msg.orientation.x = r.quat.get_scaled(2);
    imu_msg.orientation.y = r.quat.get_scaled(1);
    imu_msg.orientation.z = -r.quat.get_scaled(3);
    imu_msg.orientation.w = r.quat.get_scaled(0);

    // IMU reports a 4x4 wxyz covariance, ROS requires only 3x3 xyz.
    // NED -> ENU conversion req'd?
    imu_msg.orientation_covariance[0] = r.covariance.get_scaled(5);
    imu_msg.orientation_covariance[1] = r.covariance.get_scaled(6);
    imu_msg.orientation_covariance[2] = r.covariance.get_scaled(7);
    imu_msg.orientation_covariance[3] = r.covariance.get_scaled(9);
    imu_msg.orientation_covariance[4] = r.covariance.get_scaled(10);
    imu_msg.orientation_covariance[5] = r.covariance.get_scaled(11);
    imu_msg.orientation_covariance[6] = r.covariance.get_scaled(13);
    imu_msg.orientation_covariance[7] = r.covariance.get_scaled(14);
    imu_msg.orientation_covariance[8] = r.covariance.get_scaled(15);

    // NED -> ENU conversion.
    imu_msg.angular_velocity.x = r.gyro.get_scaled(1);
    imu_msg.angular_velocity.y = r.gyro.get_scaled(0);
    imu_msg.angular_velocity.z = -r.gyro.get_scaled(2);

    // NED -> ENU conversion.
    imu_msg.linear_acceleration.x = r.accel.get_scaled(1);
    imu_msg.linear_acceleration.y = r.accel.get_scaled(0);
    imu_msg.linear_acceleration.z = -r.accel.get_scaled(2);

    imu_pub.publish(imu_msg);
  }

  if (mag_pub.getNumSubscribers() > 0)
  {
    geometry_msgs::Vector3Stamped mag_msg;
    mag_msg.header = header;
    mag_msg.vector.x = r.mag.get_scaled(1);
    mag_msg.vector.y = r.mag.get_scaled(0);
    mag_msg.vector.z = -r.mag.get_scaled(2);
    mag_pub.publish(mag_msg);
  }

  if (rpy_pub.getNumSubscribers() > 0)
  {
    geometry_msgs::Vector3Stamped rpy_msg;
    rpy_msg.header = header;
    rpy_msg.vector.x = r.euler.get_scaled(1);
    rpy_msg.vector.y = r.euler.get_scaled(0);
    rpy_msg.vector.z = -r.euler.get_scaled(2);
    rpy_pub.publish(rpy_msg);
  }

  if (temp_pub.getNumSubscribers() > 0)
  {
    std_msgs::Float32 temp_msg;
    temp_msg.data = r.temperature.get_scaled(0);
    temp_pub.publish(temp_msg);
  }
  
  bool gps_enable;
  ros::param::param<bool>("~gps_enable", gps_enable, false);
  if (gps_enable)
  {
    static ros::Publisher gps_abs_pub = n->advertise<geometry_msgs::Vector3Stamped>("imu/gps_abs", 1, false); // absolute gps position
    static ros::Publisher gps_rel_pub = n->advertise<geometry_msgs::Vector3Stamped>("imu/gps_rel", 1, false); // relative gps position
    static ros::Publisher gps_num_sat_pub = n->advertise<std_msgs::UInt8>("imu/gps_num_sat", 1, false); // number of satellites
    static ros::Publisher gps_dop_pub = n->advertise<geometry_msgs::Vector3Stamped>("imu/gps_dop", 1, false); // dilution of precision
    static ros::Publisher gps_status_pub = n->advertise<std_msgs::UInt8>("imu/gps_status", 1, false); // gps status: 0, no gps; 1, no fix; 2, 2D fix; 3, 3D fix

    if (gps_status_pub.getNumSubscribers() > 0)
    {
      std_msgs::UInt8 gps_status_msg;
      gps_status_msg.data = (r.gps_status.get(0) >> UM6_GPS_MODE_START_BIT) & UM6_GPS_MODE_MASK;
      gps_status_pub.publish(gps_status_msg);
    }
    
    if (gps_abs_pub.getNumSubscribers() > 0)
    {
      geometry_msgs::Vector3Stamped gps_abs_msg;
      gps_abs_msg.header = header;
      gps_abs_msg.vector.x = r.gps_abs.get(0);
      gps_abs_msg.vector.y = r.gps_abs.get(1);
      gps_abs_msg.vector.z = r.gps_abs.get(2);
      gps_abs_pub.publish(gps_abs_msg);
    }
    
    if (gps_rel_pub.getNumSubscribers() > 0)
    {
      geometry_msgs::Vector3Stamped gps_rel_msg;
      gps_rel_msg.header = header;
      gps_rel_msg.vector.x = r.gps_rel.get(0);
      gps_rel_msg.vector.y = r.gps_rel.get(1);
      gps_rel_msg.vector.z = r.gps_rel.get(2);
      gps_rel_pub.publish(gps_rel_msg);
    }
    
    if (gps_dop_pub.getNumSubscribers() > 0)
    {
      geometry_msgs::Vector3Stamped gps_dop_msg;
      gps_dop_msg.header = header;
      double hdop = (r.gps_status.get(0) >> UM6_GPS_HDOP_START_BIT) & UM6_GPS_HDOP_MASK;
      double vdop = (r.gps_status.get(0) >> UM6_GPS_VDOP_START_BIT) & UM6_GPS_VDOP_MASK;
      gps_dop_msg.vector.x = hdop;
      gps_dop_msg.vector.y = hdop;
      gps_dop_msg.vector.z = vdop;
    }
    
    if (gps_num_sat_pub.getNumSubscribers() > 0)
    {
      std_msgs::UInt8 gps_num_sat_msg;
      gps_num_sat_msg.data = (r.gps_status.get(0) >> UM6_GPS_SAT_COUNT_START_BIT) & UM6_GPS_SAT_COUNT_MASK;
      gps_num_sat_pub.publish(gps_num_sat_msg);
    }
    
    // A gps odometry message  compatible with robot_pose_ekf can be transmitted
    if (ros::param::has("~gps_odom"))
    {
      std::string gps_odom_topic;
      ros::param::get("~gps_odom", gps_odom_topic);
      static ros::Publisher gps_odom_pub = n->advertise<nav_msgs::Odometry>(gps_odom_topic, 1, false);
      if(gps_odom_pub)
      {
        nav_msgs::Odometry gps_odom_msg;
        gps_odom_msg.header = header;
        gps_odom_msg.child_frame_id = "base"; // Not particularly important
        int c;
        for(c = 0; c < 36; c++)
        {
          gps_odom_msg.pose.covariance[c] = 0;
          gps_odom_msg.twist.covariance[c] = 0;
        }
        // Estimating variance as pdop
        double hdop = (r.gps_status.get(0) >> UM6_GPS_HDOP_START_BIT) & UM6_GPS_HDOP_MASK;
        double vdop = (r.gps_status.get(0) >> UM6_GPS_VDOP_START_BIT) & UM6_GPS_VDOP_MASK;
        double pdop = sqrt(hdop * hdop + vdop * vdop);
        gps_odom_msg.pose.covariance[0] = pdop;
        gps_odom_msg.pose.covariance[7] = pdop;
        gps_odom_msg.pose.covariance[14] = pdop;
        // Ignore angular position data
        gps_odom_msg.pose.covariance[21] = 999999;
        gps_odom_msg.pose.covariance[28] = 999999;
        gps_odom_msg.pose.covariance[35] = 999999;
        // Ignore velocity
        gps_odom_msg.twist.covariance[0] = 999999;
        gps_odom_msg.twist.covariance[7] = 999999;
        gps_odom_msg.twist.covariance[14] = 999999;
        gps_odom_msg.twist.covariance[21] = 999999;
        gps_odom_msg.twist.covariance[28] = 999999;
        gps_odom_msg.twist.covariance[35] = 999999;
        
        gps_odom_msg.pose.pose.position.x = r.gps_abs.get(0);
        gps_odom_msg.pose.pose.position.y = r.gps_abs.get(1);
        gps_odom_msg.pose.pose.position.z = r.gps_abs.get(2);
        gps_odom_msg.pose.pose.orientation.x = 0;
        gps_odom_msg.pose.pose.orientation.y = 0;
        gps_odom_msg.pose.pose.orientation.z = 0;
        gps_odom_msg.pose.pose.orientation.w = 1;
        // Calculating heading and speed from the GPS
        double course = double(r.gps_course_speed.get(0)) * 0.0314159265; // hundreths of degrees converted to radians
        double speed = double(r.gps_course_speed.get(1)) / 100; // centimeters per second converted to meters per second
        gps_odom_msg.twist.twist.linear.x = speed * cos(course);
        gps_odom_msg.twist.twist.linear.y = speed * sin(course);
        gps_odom_msg.twist.twist.linear.z = 0;
        gps_odom_msg.twist.twist.angular.x = 0;
        gps_odom_msg.twist.twist.angular.y = 0;
        gps_odom_msg.twist.twist.angular.z = 0;
        gps_odom_pub.publish(gps_odom_msg);
      }
    }
  }
}


/**
 * Node entry-point. Handles ROS setup, and serial port connection/reconnection.
 */
int main(int argc, char **argv)
{
  ros::init(argc, argv, "um6_driver");

  // Load parameters from private node handle.
  std::string port;
  int32_t baud;
  ros::param::param<std::string>("~port", port, "/dev/ttyUSB0");
  ros::param::param<int32_t>("~baud", baud, 115200);

  serial::Serial ser;
  ser.setPort(port);
  ser.setBaudrate(baud);
  serial::Timeout to = serial::Timeout(50, 50, 0, 50, 0);
  ser.setTimeout(to);

  ros::NodeHandle n;
  std_msgs::Header header;
  ros::param::param<std::string>("~frame_id", header.frame_id, "imu_link");

  bool first_failure = true;
  while (ros::ok())
  {
    try
    {
      ser.open();
    }
    catch(const serial::IOException& e)
    {
      ROS_DEBUG("Unable to connect to port.");
    }
    if (ser.isOpen())
    {
      ROS_INFO("Successfully connected to serial port.");
      first_failure = true;
      try
      {
        um6::Comms sensor(&ser);
        configureSensor(&sensor);
        um6::Registers registers;
        ros::ServiceServer srv = n.advertiseService<um6::Reset::Request, um6::Reset::Response>(
                                   "reset", boost::bind(handleResetService, &sensor, _1, _2));

        while (ros::ok())
        {
          if (sensor.receive(&registers) == TRIGGER_PACKET)
          {
            // Triggered by arrival of final message in group.
            header.stamp = ros::Time::now();
            publishMsgs(registers, &n, header);
            ros::spinOnce();
          }
        }
      }
      catch(const std::exception& e)
      {
        if (ser.isOpen()) ser.close();
        ROS_ERROR_STREAM(e.what());
        ROS_INFO("Attempting reconnection after error.");
        ros::Duration(1.0).sleep();
      }
    }
    else
    {
      ROS_WARN_STREAM_COND(first_failure, "Could not connect to serial device "
                           << port << ". Trying again every 1 second.");
      first_failure = false;
      ros::Duration(1.0).sleep();
    }
  }
}
