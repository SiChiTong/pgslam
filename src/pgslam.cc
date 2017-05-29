/*
 * Copyright 2017 Yu Kunlin <yukunlin@mail.ustc.edu.cn>
 */
#include <pgslam/pgslam.h>
#include <pgslam/kdtree2d.h>

#include <float.h>
#include <sys/time.h>
#include <Eigen/Eigen>

#include <iomanip>
#include <iostream>
#include <sstream>

using pgslam::Pose2D;
using pgslam::Echo;
using pgslam::LaserScan;
using pgslam::GraphSlam;
using pgslam::Slam;

Pose2D::Pose2D() {
  this->x_ = 0;
  this->y_ = 0;
  this->theta_ = 0;
}

Pose2D::Pose2D(double x, double y, double theta) {
  x_ = x;
  y_ = y;
  theta_ = theta;
  while (theta_ < -M_PI) theta_ += 2 * M_PI;
  while (theta_ >  M_PI) theta_ -= 2 * M_PI;
}

void Pose2D::set_x(double x) { x_ = x; }

void Pose2D::set_y(double y) { y_ = y; }

void Pose2D::set_theta(double theta) {
  theta_ = theta;
  while (theta_ < -M_PI) theta_ += 2 * M_PI;
  while (theta_ >  M_PI) theta_ -= 2 * M_PI;
}

double Pose2D::x() { return x_; }

double Pose2D::y() { return y_; }

double Pose2D::theta () { return theta_; }

Pose2D Pose2D::operator +(Pose2D p) {
  Eigen::Vector2d v1(x_, y_);
  Eigen::Vector2d v2(p.x_, p.y_);
  Eigen::Rotation2D<double> rot(theta_);
  v2 = rot * v2;
  Eigen::Vector2d v = v1 + v2;
  return Pose2D(v.x(), v.y(), theta_ + p.theta_);
}

Pose2D Pose2D::inverse() {
  Eigen::Vector2d v(-x_ , -y_);
  Eigen::Rotation2D<double> rot(-theta_);
  v = rot * v;
  return Pose2D(v.x(), v.y(), -theta_);
}

Pose2D Pose2D::operator - (Pose2D p) {
  return p.inverse() + *this;
}

Eigen::Vector2d Pose2D::pos() {
  return Eigen::Vector2d(x_, y_);
}

std::string Pose2D::to_string() {
  std::stringstream ss;
  ss << "x:";
  ss << std::setw(7) << setiosflags(std::ios::fixed) << std::setprecision(4);
  ss << x_;
  ss << " y:";
  ss << std::setw(7) << setiosflags(std::ios::fixed) << std::setprecision(4);
  ss << y_;
  ss << " theta:";
  ss << std::setw(7) << setiosflags(std::ios::fixed) << std::setprecision(4);
  ss << theta_;
  return ss.str();
}

Echo::Echo(double range, double angle, double intensity, int64_t time_stamp) {
  range_ = range;
  angle_ = angle;
  intensity_ = intensity;
  time_stamp_ = time_stamp;
}

double Echo::get_range() { return range_; }

double Echo::get_angle() { return angle_; }

double Echo::get_intensity() { return intensity_; }

int64_t Echo::get_time_stamp() { return time_stamp_; }

Eigen::Vector2d Echo::get_point() {
  double x = range_ * cos(angle_);
  double y = range_ * sin(angle_);
  return Eigen::Vector2d(x, y);
}

LaserScan::LaserScan(std::vector<Echo> echos) {
  for (size_t i = 0; i < echos.size(); i++)
    points_self_.push_back(echos[i].get_point());
  world_transformed_flag_ = false;

  match_threshold_ = 0.1;
  dist_threshold_ = 1.0;
}

LaserScan::LaserScan(std::vector<Echo> echos, Pose2D pose) {
  for (size_t i = 0; i < echos.size(); i++)
    points_self_.push_back(echos[i].get_point());
  pose_ = pose;
  world_transformed_flag_ = false;

  match_threshold_ = 0.1;
  dist_threshold_ = 1.0;
}

Pose2D LaserScan::get_pose() const {
  return pose_;
}

void LaserScan::set_pose(Pose2D pose) {
  pose_ = pose;
  world_transformed_flag_ = false;
}

const std::vector<Eigen::Vector2d> & LaserScan::get_points() {
  UpdateToWorld();
  return points_world_;
}

void LaserScan::UpdateToWorld() {
  if (world_transformed_flag_) return;

  std::vector<Eigen::Vector2d>().swap(points_world_);

  max_x_ = 0.0;
  min_x_ = 0.0;
  max_y_ = 0.0;
  min_y_ = 0.0;

  Eigen::Rotation2D<double> rot(pose_.theta_);
  Eigen::Vector2d move = pose_.pos();
  for (size_t i = 0; i < points_self_.size(); i++) {
    Eigen::Vector2d p = rot * points_self_[i] + move;
    points_world_.push_back(p);
    if (p.x() > max_x_) max_x_ = p.x();
    if (p.x() < min_x_) min_x_ = p.x();
    if (p.y() > max_y_) max_y_ = p.y();
    if (p.y() < min_y_) min_y_ = p.y();
  }

  world_transformed_flag_ = true;
}

  std::vector<Eigen::Vector2d>
LaserScan::transform(const std::vector<Eigen::Vector2d> &v, Pose2D pose) {
  std::vector<Eigen::Vector2d> v2(v.size());
  Eigen::Rotation2D<double> rot(pose.theta_);
  Eigen::Vector2d move(pose.x_, pose.y_);
  for (size_t i = 0; i < v.size(); i++) {
    v2[i] = rot * v[i];
    v2[i] += move;
  }
  return v2;
}

Pose2D LaserScan::ICP(const LaserScan &scan_, double *ratio) {
  std::vector<Eigen::Vector2d> scan_ref = points_self_;
  std::vector<Eigen::Vector2d> scan = scan_.points_self_;
  Pose2D reference_pose = scan_.get_pose() - pose_;

  if (scan_ref.size() < 2 || scan.size() < 2) {
    std::cout << "Error: scan.size() < 2";
    return reference_pose;
  }

  size_t insert_num = 7;
  auto scan_cache = scan_ref;
  scan_ref.resize(scan_cache.size()*insert_num);
  for (size_t i = 0; i < scan_cache.size() - 1; i++) {
    for (size_t j = 0; j < insert_num; j++) {
      scan_ref[insert_num*i+j] = (scan_cache[i+1] - scan_cache[i]) /
        insert_num * j + scan_cache[i];
    }
  }

  kd_tree_2d::KDTree2D tree;
  tree.Construct(scan_ref);
  std::vector<Eigen::Vector2d> scan_origin = scan;
  Pose2D pose = reference_pose;
  for (int i = 0; i < 20; i++) {
    scan = transform(scan_origin, pose);

    // store the closest point
    std::vector<Eigen::Vector2d>    near = scan;
    // to trace some points have a same closest point
    std::vector< std::vector<int> > trace_back(scan_ref.size());
    // true: effective point; false: non-effective point;
    std::vector<bool> mask(scan.size());

    // search and save nearest point
    int match_count = 0;
    for (size_t i=0; i < scan.size(); i++) {
      Eigen::Vector2d point = scan[i];

      size_t index = tree.NearestIndex(scan[i]);

      if (index == -1) {
        if (ratio != nullptr)
          *ratio = 0.0;
        return Pose2D();
      }
      trace_back[index].push_back(i);
      Eigen::Vector2d closest = scan_ref[index];

      double distance = (point-closest).norm();
      if (distance < match_threshold_)
        match_count++;
      if (distance < dist_threshold_) {
        near[i] = closest;
        mask[i] = true;
      } else {
        mask[i] = false;
      }
    }
    if (ratio != nullptr)
      *ratio = static_cast<double>(match_count) / scan.size();

    // disable the points have one same nearest point
    for (size_t i = 0; i < trace_back.size(); i++)
      if (trace_back[i].size() > 3) {
        for (size_t j = 0; j < trace_back[i].size(); j++) {
          mask[trace_back[i][j]] = false;
          near[trace_back[i][j]] = scan[trace_back[i][j]];
        }
      }

    // disable the farest 10% point
    std::vector<double> max_distance;
    std::vector<int>    max_index;
    max_distance.resize(scan.size() / 10, 0.0);
    max_index.resize(scan.size() / 10, 0);
    for (size_t i = 0; i < scan.size(); i++) {
      double distance = (scan[i]-near[i]).norm();
      for (size_t j = 1; j < max_distance.size(); j++) {
        if (distance > max_distance[j]) {
          max_distance[j-1] = max_distance[j];
          max_index[j-1] = max_index[j];
          if (j == max_distance.size()-1) {
            max_distance[j] = distance;
            max_index[j] = i;
          }
        } else {
          max_distance[j-1] = distance;
          max_index[j-1] = i;
          break;
        }
      }
    }
    for (size_t i = 1; i < max_index.size(); i++)
      mask[max_index[i]] = false;

    // calc center
    Eigen::Vector2d center(0, 0);
    int count = 0;
    for (size_t i = 0; i < scan.size(); i++)
      if (mask[i]) {
        center += scan[i];
        count++;
      }
    if (count == 0) {
      std::cout << "Error: no valid point, return reference pose." << std::endl;
      if (ratio != nullptr)
        *ratio = 0.0;
      return reference_pose;
    }
    center /= count;

    // calc the tramsform
    Eigen::Vector2d move = Eigen::Vector2d(0.0, 0.0);
    double rot = 0.0;
    for (size_t i = 0; i < scan.size(); i++) {
      if (mask[i] == false) continue;
      Eigen::Vector2d delta = near[i] - scan[i];
      double length = delta.norm();
      if (length > 0) {
        delta.normalize();
        delta *= (length < 0.05) ? length : sqrt(length * 20) / 20;
      }
      move += delta;
      Eigen::Vector2d p = scan[i] - center;
      Eigen::Vector2d q = near[i] - center;
      if (p.norm() < DBL_EPSILON*2) continue;

      rot += (p.x()*q.y() - p.y()*q.x()) / p.norm() / sqrt(p.norm());
    }
    move /= count;
    rot /= count;

    // speed up
    move *= 2.0;
    rot *= 1.0;

    Pose2D pose_delta = Pose2D(move.x(), move.y(), rot);
    pose_delta = (pose.inverse() + pose_delta + pose);
    pose = pose + pose_delta;
  }
  return pose;
}

double LaserScan::get_max_x_in_world() {
  UpdateToWorld();
  return max_x_;
}

double LaserScan::get_min_x_in_world() {
  UpdateToWorld();
  return min_x_;
}

double LaserScan::get_max_y_in_world() {
  UpdateToWorld();
  return max_y_;
}

double LaserScan::get_min_y_in_world() {
  UpdateToWorld();
  return min_y_;
}

#ifdef USE_ISAM
GraphSlam::GraphSlam() {
  slam_ = new isam::Slam();
}

bool GraphSlam::check(size_t id) {
  // size enough
  if (id < pose_nodes_.size()) {
    if (pose_nodes_[id] == NULL) {  // already been removed
      pose_nodes_[id] = new isam::Pose2d_Node();
      return true;
    } else {
      return false;  // still there
    }
  }

  // create new space
  int size = pose_nodes_.size();
  pose_nodes_.resize(id+1);
  for (size_t i = size; i < id + 1; i++)
    pose_nodes_[i] = new isam::Pose2d_Node();
  return true;
}

void GraphSlam::remove(size_t node_id) {
  slam_->remove_node(pose_nodes_[node_id]);
  delete pose_nodes_[node_id];
  pose_nodes_[node_id] = NULL;
  slam_->batch_optimization();
}

void GraphSlam::clear() {
  delete slam_;
  slam_ = new isam::Slam();
  std::vector<isam::Pose2d_Node*>().swap(pose_nodes_);
}

std::vector< std::pair<size_t, Pose2D> > GraphSlam::get_nodes() {
  std::vector< std::pair<size_t, Pose2D> > pose_id;
  // store in response
  for (size_t i = 0; i < pose_nodes_.size(); i++) {
    if (pose_nodes_[i] == NULL) continue;
    Pose2D pose;
    pose.set_x(pose_nodes_[i]->value().x());
    pose.set_y(pose_nodes_[i]->value().y());
    pose.set_theta(pose_nodes_[i]->value().t());
    pose_id.push_back(std::pair<size_t, Pose2D>(i, pose));
  }
  return pose_id;
}

std::vector< std::pair<Eigen::Vector2d, Eigen::Vector2d> >
GraphSlam::get_factors() {
  std::vector< std::pair<Eigen::Vector2d, Eigen::Vector2d> > factors;
  const std::list<isam::Factor*> factors_ = slam_->get_factors();
  for (std::list<isam::Factor*>::const_iterator it=factors_.begin();
      it != factors_.end(); it++) {
    std::vector<isam::Node*> nodes = (*it)->nodes();
    if (nodes.size() == 1) continue;
    Eigen::Vector2d first;
    first.x() = ((isam::Pose2d_Node*)nodes[0])->value().x();
    first.y() = ((isam::Pose2d_Node*)nodes[0])->value().y();
    Eigen::Vector2d second;
    second.x() = ((isam::Pose2d_Node*)nodes[1])->value().x();
    second.y() = ((isam::Pose2d_Node*)nodes[1])->value().y();
    factors.push_back(std::make_pair(first, second));
  }
  return factors;
}

void GraphSlam::AddPose2dFactor(size_t node_id, Pose2D pose_ros, double cov) {
  isam::Pose2d pose(pose_ros.x(), pose_ros.y(), pose_ros.theta());
  if (cov <= 0) {
    cov = 1.0;
  }

  // check new node
  bool ret = check(node_id);
  if (ret) slam_->add_node(pose_nodes_[node_id]);

  // add factor
  isam::Noise noise = isam::Information(cov * isam::eye(3));
  isam::Pose2d_Factor * factor =
    new isam::Pose2d_Factor(pose_nodes_[node_id], pose, noise);
  slam_->add_factor(factor);
  // slam_->batch_optimization();
}

void GraphSlam::AddPose2dPose2dFactor(size_t node_id_ref,
    size_t node_id, Pose2D pose_ros, double cov) {
  isam::Pose2d pose(pose_ros.x(), pose_ros.y(), pose_ros.theta());
  if (cov <= 0) {
    cov = 1.0;
  }

  // check new node
  bool ret = check(node_id_ref);
  if (ret) slam_->add_node(pose_nodes_[node_id_ref]);
  ret = check(node_id);
  if (ret) slam_->add_node(pose_nodes_[node_id]);

  // add factor
  isam::Noise noise = isam::Information(cov * isam::eye(3));
  isam::Pose2d_Pose2d_Factor * factor =
    new isam::Pose2d_Pose2d_Factor(pose_nodes_[node_id_ref],
        pose_nodes_[node_id], pose, noise);
  slam_->add_factor(factor);
  // slam_->batch_optimization();
}

void GraphSlam::Optimization() {
  slam_->batch_optimization();
}
#endif

Slam::Slam() {
  keyscan_threshold_ = 0.4;
  factor_threshold_ = 0.9;
}

void Slam::set_keyscan_threshold(double keyscan_threshold) {
  this->keyscan_threshold_ = keyscan_threshold;
  if (keyscan_threshold_ * 2 > factor_threshold_)
    factor_threshold_ = keyscan_threshold_ * 2;
}

void Slam::set_factor_threshold(double factor_threshold) {
  this->factor_threshold_ = factor_threshold;
  if (keyscan_threshold_ * 2 > factor_threshold_)
    keyscan_threshold_ = factor_threshold_/2;
}

Pose2D Slam::get_pose() const {
  return pose_;
}

const std::vector<LaserScan> & Slam::get_scans() {
  return scans_;
}

#ifdef USE_ISAM
std::vector< std::pair<Eigen::Vector2d, Eigen::Vector2d> > Slam::get_factors() {
  return graph_slam_.get_factors();
}
#endif

Pose2D Slam::EncoderToPose2D(double left, double right, double tread) {
  double theta = (right-left) / tread;
  double theta_2 = theta / 2.0;
  double arc = (right+left) / 2.0;
  double radius = arc / theta;
  double secant = 2 * sin(theta_2) * radius;
  if (theta == 0)
    secant = arc;
  double x = secant * cos(theta_2);
  double y = secant * sin(theta_2);

  return Pose2D(x, y, theta);
}

void Slam::UpdatePoseWithPose(Pose2D pose) {
  this->pose_ = this->pose_ + pose;
}

void Slam::UpdatePoseWithEncoder(double left, double right, double tread) {
  pose_ = pose_ + EncoderToPose2D(left, right, tread);
  if (pose_update_callback)
    pose_update_callback(pose_);
}

void Slam::UpdatePoseWithLaserScan(const LaserScan &scan_) {
  LaserScan scan = scan_;
  scan.set_pose(pose_);

  // first scan
  if (scans_.empty()) {
    scans_.push_back(scan);
#ifdef USE_ISAM
    graph_slam_.AddPose2dFactor(0, pose_, 1);
#endif
    std::cout << "add key scan " << scans_.size() << ": "
      << pose_.to_string() << std::endl;
    if (map_update_callback)
      map_update_callback();
    return;
  }

  // search for the closest scan
  LaserScan *closest_scan = &(scans_[0]);
  double min_dist = DBL_MAX;
  for (size_t i = 0; i < scans_.size(); i++) {
    double dist = (scans_[i].get_pose().pos() -
        scan.get_pose().pos()).norm();
    double delta_theta = fabs(scans_[i].get_pose().theta() -
        scan.get_pose().theta());;
    while (delta_theta < -M_PI) delta_theta += 2 * M_PI;
    while (delta_theta >  M_PI) delta_theta -= 2 * M_PI;
    delta_theta *= keyscan_threshold_ / (M_PI_4 * 3.0);

    dist = sqrt(dist*dist + delta_theta*delta_theta);
    if (dist < min_dist) {
      min_dist = dist;
      closest_scan = &(scans_[i]);
    }
  }


  if (min_dist < keyscan_threshold_) {
    // update pose
    double ratio;
    Pose2D pose_delta = closest_scan->ICP(scan, &ratio);
    pose_ = closest_scan->get_pose() + pose_delta;
  } else {
    // add key scan
#ifdef USE_ISAM
    size_t constrain_count = 0;
    for (size_t i = 0; i < scans_.size(); i++) {
      double distance = (pose_.pos() - scans_[i].get_pose().pos()).norm();
      if (distance < factor_threshold_) {
        constrain_count++;
        double ratio;
        Pose2D pose_delta = scans_[i].ICP(scan, &ratio);
        graph_slam_.AddPose2dPose2dFactor(i, scans_.size(), pose_delta, ratio);
        if (pose_update_callback)
          pose_update_callback(pose_);
      }
    }
    if (constrain_count > 1)
      graph_slam_.Optimization();

    auto nodes = graph_slam_.get_nodes();
    for (size_t i = 0; i < nodes.size(); i++) {
      // update pose of node
      for (size_t j = 0; j < scans_.size(); j++) {
        if (j != nodes[i].first) continue;
        scans_[j].set_pose(nodes[i].second);
        break;
      }
      // add the new scan with pose
      if (nodes[i].first == scans_.size()) {
        pose_ = nodes[i].second;
        scan.set_pose(nodes[i].second);
        scans_.push_back(scan);
      }
    }
#else
    scans_.push_back(scan);
#endif
    std::cout << "add key scan " << scans_.size() << ": "
      << pose_.to_string() << std::endl;

    if (map_update_callback)
      map_update_callback();
  }
  if (pose_update_callback)
    pose_update_callback(pose_);
}

void Slam::RegisterPoseUpdateCallback(std::function<void(Pose2D)> f) {
  pose_update_callback = f;
}
void Slam::RegisterMapUpdateCallback(std::function<void(void)> f) {
  map_update_callback = f;
}

