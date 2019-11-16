//
// Created by ljn on 19-8-16.
//

#include "path_optimizer.hpp"
namespace PathOptimizationNS {

PathOptimizer::PathOptimizer(const std::vector<hmpl::State> &points_list,
                             const hmpl::State &start_state,
                             const hmpl::State &end_state,
                             const hmpl::InternalGridMap &map) :
    grid_map_(map),
    collision_checker_(map),
    points_list_(points_list),
    point_num_(points_list.size()),
    start_state_(start_state),
    end_state_(end_state),
    car_type(ACKERMANN_STEERING),
    rear_axle_to_center_dis(1.45),
    wheel_base(2.85),
    best_sampling_index_(0),
    control_sampling_first_flag_(false),
    enable_control_sampling(true) {}
bool PathOptimizer::solve(std::vector<hmpl::State> *final_path) {
    if (point_num_ == 0) {
        printf("empty input, quit path optimization\n");
        return false;
    }
    auto smoothing_flag = smoothPath(&smoothed_path_);
    if (!smoothing_flag) {
        printf("smoothing stage failed, quit path optimization.\n");
        return false;
    }
    reset(smoothed_path_);
    return optimizePath(final_path);
}

void PathOptimizer::reset(const std::vector<hmpl::State> &points_list) {
    points_list_ = points_list;
    point_num_ = points_list_.size();
    x_list_.clear();
    y_list_.clear();
    s_list_.clear();
    k_list_.clear();
    seg_x_list_.clear();
    seg_y_list_.clear();
    seg_k_list_.clear();
    seg_angle_list_.clear();
    seg_s_list_.clear();
    predicted_path_in_frenet_.clear();
}

bool PathOptimizer::smoothPath(std::vector<hmpl::State> *smoothed_path) {
    double s = 0;
    for (size_t i = 0; i != point_num_; ++i) {
        if (i == 0) {
            s_list_.push_back(0);
        } else {
            double ds = hmpl::distance(points_list_[i], points_list_[i - 1]);
            s += ds;
            s_list_.push_back(s);
        }
        x_list_.push_back(points_list_[i].x);
        y_list_.push_back(points_list_[i].y);
    }
    double max_s = s_list_.back();
    std::cout << "ref path length: " << max_s << std::endl;
    x_spline_.set_points(s_list_, x_list_);
    y_spline_.set_points(s_list_, y_list_);
    // Make the path dense, the interval being 0.3m
    x_list_.clear();
    y_list_.clear();
    s_list_.clear();
    size_t new_points_count = 0;
    for (double new_s = 0; new_s <= max_s; new_s += 0.3) {
        double x = x_spline_(new_s);
        double y = y_spline_(new_s);
        x_list_.push_back(x);
        y_list_.push_back(y);
        s_list_.push_back(new_s);
        ++new_points_count;
    }
    point_num_ = x_list_.size();
    double max_curvature_abs;
    double max_curvature_change_abs;
    getCurvature(x_list_, y_list_, &k_list_, &max_curvature_abs, &max_curvature_change_abs);
    k_spline_.set_points(s_list_, k_list_);
    // Divid the reference path. Intervals are smaller at the beginning.
    double delta_s = 2;
    seg_s_list_.push_back(0);
    while (seg_s_list_.back() < max_s) {
        seg_s_list_.push_back(seg_s_list_.back() + delta_s);
    }
    if (max_s - seg_s_list_.back() > 1) {
        seg_s_list_.push_back(max_s);
    }
    auto N = seg_s_list_.size();
    // Store reference states in vectors. They will be used later.
    for (size_t i = 0; i != N; ++i) {
        double length_on_ref_path = seg_s_list_[i];
        double angle;
        angle = atan2(y_spline_.deriv(1, length_on_ref_path), x_spline_.deriv(1, length_on_ref_path));
        seg_angle_list_.push_back(angle);
        seg_x_list_.push_back(x_spline_(length_on_ref_path));
        seg_y_list_.push_back(y_spline_(length_on_ref_path));
        seg_k_list_.push_back(k_spline_(length_on_ref_path));
    }

    typedef CPPAD_TESTVECTOR(double) Dvector;
    size_t n_vars = N;
    const size_t pq_range_begin = 0;
    Dvector vars(n_vars);
    for (size_t i = 0; i < n_vars; i++) {
        vars[i] = 0;
    }
    // bounds of variables
    Dvector vars_lowerbound(n_vars);
    Dvector vars_upperbound(n_vars);
    for (size_t i = 0; i != n_vars; ++i) {
        vars_lowerbound[i] = -DBL_MAX;
        vars_upperbound[i] = DBL_MAX;
    }
    // Start point is the start position of the vehicle.
    vars_lowerbound[0] = 0;
    vars_upperbound[0] = 0;
    double psi = start_state_.z - seg_angle_list_.front();
//    if (fabs(psi) < 35 * M_PI / 180) {
//        double pq1 = seg_s_list_[1] * tan(psi);
//        vars_lowerbound[1] = pq1 - 0.05;
//        vars_upperbound[1] = pq1 + 0.05;
//    }
    // Costraints inclued N - 2 curvatures and N - 2 shifts for front, center and rear circles each.
    size_t n_constraints = 0;
    Dvector constraints_lowerbound(n_constraints);
    Dvector constraints_upperbound(n_constraints);
    // options for IPOPT solver
    std::string options;
    // Uncomment this if you'd like more print information
    options += "Integer print_level  0\n";
    // NOTE: Setting sparse to true allows the solver to take advantage
    // of sparse routines, this makes the computation MUCH FASTER. If you
    // can uncomment 1 of these and see if it makes a difference or not but
    // if you uncomment both the computation time should go up in orders of
    // magnitude.
    options += "Sparse  true        forward\n";
    options += "Sparse  true        reverse\n";
    // NOTE: Currently the solver has a maximum time limit of 0.1 seconds.
    // Change this as you see fit.
    options += "Numeric max_cpu_time          0.1\n";

    // place to return solution
    CppAD::ipopt::solve_result<Dvector> solution;
    // weights of the cost function
    // TODO: use a config file
    std::vector<double> weights;
    weights.push_back(20); //curvature weight
    weights.push_back(30); //curvature rate weight
    weights.push_back(0.01); //distance to boundary weight
    weights.push_back(1); //path length weight

    FgEvalFrenetSmooth fg_eval_frenet(seg_x_list_,
                                      seg_y_list_,
                                      seg_angle_list_,
                                      seg_k_list_,
                                      seg_s_list_,
                                      N,
                                      weights);
    // solve the problem
    CppAD::ipopt::solve<Dvector, FgEvalFrenetSmooth>(options, vars,
                                                     vars_lowerbound, vars_upperbound,
                                                     constraints_lowerbound, constraints_upperbound,
                                                     fg_eval_frenet, solution);
    // Check if it works
    bool ok = true;
    ok &= solution.status == CppAD::ipopt::solve_result<Dvector>::success;
    if (!ok) {
        LOG(WARNING) << "smoothing solver failed!";
        return false;
    }
    LOG(INFO) << "smoothing solver succeeded!";
    // output
    for (size_t i = 0; i != N; i++) {
        double tmp[2] = {solution.x[i], double(i)};
        std::vector<double> v(tmp, tmp + sizeof tmp / sizeof tmp[0]);
        this->predicted_path_in_frenet_.push_back(v);
    }
    size_t control_points_num = N;
    tinyspline::BSpline b_spline(control_points_num);
    std::vector<tinyspline::real> ctrlp = b_spline.controlPoints();
    for (size_t i = 0; i != N; ++i) {
        double length_on_ref_path = seg_s_list_[i];
        double angle = seg_angle_list_[i];
        double new_angle = constraintAngle(angle + M_PI_2);
        double tmp_x = x_spline_(length_on_ref_path) + predicted_path_in_frenet_[i][0] * cos(new_angle);
        double tmp_y = y_spline_(length_on_ref_path) + predicted_path_in_frenet_[i][0] * sin(new_angle);
        if (std::isnan(tmp_x) || std::isnan(tmp_y)) {
            LOG(WARNING) << "output is not a number, smoothing failed!" << std::endl;
            return false;
        }
        ctrlp[2 * (i)] = tmp_x;
        ctrlp[2 * (i) + 1] = tmp_y;
    }
    // B spline
    b_spline.setControlPoints(ctrlp);
    std::vector<hmpl::State> tmp_final_path;
    double step_t = 1.0 / (3.0 * N);
    std::vector<tinyspline::real> result;
    std::vector<tinyspline::real> result_next;
    for (size_t i = 0; i <= 3 * N; ++i) {
        double t = i * step_t;
        result = b_spline.eval(t).result();
        hmpl::State state;
        state.x = result[0];
        state.y = result[1];
        if (i == 3 * N) {
            state.z = tmp_final_path.back().z;
        } else {
            result_next = b_spline.eval((i + 1) * step_t).result();
            double dx = result_next[0] - result[0];
            double dy = result_next[1] - result[1];
            state.z = atan2(dy, dx);
        }
        tmp_final_path.push_back(state);
    }
    smoothed_path->clear();
    std::copy(tmp_final_path.begin(), tmp_final_path.end(), std::back_inserter(*smoothed_path));
    return true;
}

bool PathOptimizer::optimizePath(std::vector<hmpl::State> *final_path) {
    auto po_start = std::clock();
    if (point_num_ == 0) {
        LOG(INFO) << "path input is empty!";
        return false;
    }
    // Set the car geometry. Use 3 circles to approximate the car.
    // TODO: use a config file.
    // TODO: consider back up situation
    double car_width = 2.0;
    double car_length = 4.9;
    // Vector car_geo is for function getClearanceWithDirection.
    // Radius of each circle.
    double circle_r = sqrt(pow(car_length / 8, 2) + pow(car_width / 2, 2));
    printf("circle r: %f\n", circle_r);
    // Distance to the vehicle center of each circle.
    double d1 = -3.0 / 8.0 * car_length;
    double d2 = -1.0 / 8.0 * car_length;
    double d3 = 1.0 / 8.0 * car_length;
    double d4 = 3.0 / 8.0 * car_length;
    car_geo_.push_back(d1);
    car_geo_.push_back(d2);
    car_geo_.push_back(d3);
    car_geo_.push_back(d4);
    car_geo_.push_back(circle_r);
    car_geo_.push_back(rear_axle_to_center_dis);
    car_geo_.push_back(wheel_base);

    /// If the start state is too close to the obstacle (less than the radius of the circles covering the vehicle,
    /// for example), the optimization might fail. This section does control sampling under this condition.
    /// No need to understand the code.
    auto original_start_state = start_state_;
    std::vector<hmpl::State> best_path;
    double min_distance_for_best_path = 0;
    size_t min_index_for_best_path = 0;
    grid_map::Position start_position(start_state_.x, start_state_.y);
    double start_ref_angle = hmpl::angle(points_list_[0], points_list_[1]);
    double start_angle_diff = fabs(constraintAngle(start_state_.z - start_ref_angle));
    if (enable_control_sampling
        && (grid_map_.getObstacleDistance(start_position) < circle_r || start_angle_diff > 60 * M_PI / 180)) {
        printf("start point is close to obstacle, control sampling first!\n");
        if (start_state_.k < -MAX_CURVATURE || start_state_.k > MAX_CURVATURE) goto normal_procedure;
        double dk = -0.1;
        while (dk <= 0.1) {
            double max_ds = 10.0;
            double max_turn_ds = 0;
            if (dk < 0) {
                max_turn_ds = (start_state_.k + MAX_CURVATURE) / fabs(dk);
            } else if (dk > 0) {
                max_turn_ds = (MAX_CURVATURE - start_state_.k) / fabs(dk);
            } else {
                max_turn_ds = 1;
            }
            G2lib::ClothoidCurve curve;
            curve.build(start_state_.x, start_state_.y, start_state_.z, start_state_.k, dk, max_turn_ds);
            hmpl::State turn_curve_end;
            curve.evaluate(max_turn_ds, turn_curve_end.z, turn_curve_end.k, turn_curve_end.x, turn_curve_end.y);
            G2lib::ClothoidCurve curve_keep;
            curve_keep.build(turn_curve_end.x,
                             turn_curve_end.y,
                             turn_curve_end.z,
                             turn_curve_end.k,
                             0,
                             std::max(4.0, max_ds - max_turn_ds));
            double sampling_length = 2;
            while (sampling_length <= max_ds) {
                hmpl::State tmp_state;
                double delta_s = 0.4;
                std::vector<hmpl::State> sampling_result;
                for (size_t i = 0; i * delta_s < sampling_length; ++i) {
                    if (i * delta_s < max_turn_ds) {
                        curve.evaluate(i * delta_s, tmp_state.z, tmp_state.k, tmp_state.x, tmp_state.y);
                    } else {
                        curve_keep.evaluate(i * delta_s - max_turn_ds,
                                            tmp_state.z,
                                            tmp_state.k,
                                            tmp_state.x,
                                            tmp_state.y);
                    }
                    if (collision_checker_.isSingleStateCollisionFreeImproved(tmp_state)) {
                        sampling_result.push_back(tmp_state);
                    } else {
                        failed_sampling_path_set_.push_back(sampling_result);
                        goto try_new_dk;
                    }
                }
                sampling_path_set_.push_back(sampling_result);
                sampling_length += 1;
            }
            try_new_dk :
            dk += 0.025;
        }
        printf("control sampling get %d paths\n", sampling_path_set_.size());
        if (!sampling_path_set_.empty()) {
//            std::vector<double> path_scoring;
            double min_score = DBL_MAX;
            for (size_t i = 0; i != sampling_path_set_.size(); ++i) {
                const auto &last_state = sampling_path_set_[i].back();
                size_t min_index = 0;
                auto min_distance = DBL_MAX;
                for (size_t i = 0; i != point_num_; ++i) {
                    double tmp_distance = hmpl::distance(last_state, points_list_[i]);
                    if (tmp_distance < min_distance) {
                        min_distance = tmp_distance;
                        min_index = i;
                    } else if (tmp_distance > 15 && min_distance < 15) {
                        break;
                    }
                }
                double ref_angle = hmpl::angle(points_list_[min_index], points_list_[min_index + 1]);
                double angle_diff = fabs(constraintAngle(ref_angle - last_state.z));
                if (fabs(angle_diff) > 45) continue;
                grid_map::Position sampling_end_position(last_state.x, last_state.y);
                double sampling_end_clearance = grid_map_.getObstacleDistance(sampling_end_position);
                // TODO: path choosing strategy should be improved!
                double score = 5 * angle_diff + min_distance + 7 / sampling_end_clearance;
                if (score < min_score) {
                    best_sampling_index_ = i;
                    min_distance_for_best_path = min_distance;
                    min_index_for_best_path = min_index;
                    min_score = score;
                }
            }
            control_sampling_first_flag_ = true;
            best_path.clear();
            for (const auto &state : sampling_path_set_[best_sampling_index_]) {
                best_path.push_back(state);
            }
            start_state_ = best_path.back();
            printf("control sampling before path optimization succeeded!\n");
        } else {
            control_sampling_first_flag_ = false;
            printf("empty path set\n");
        }
    }

    normal_procedure:
    double cte = 0;  // lateral error
    double epsi = 0; // navigable error
    // Get the closest point on the ref path and erase the points before this point.
    auto min_distance = DBL_MAX;
    size_t min_index = 0;
    if (control_sampling_first_flag_) {
        min_index = min_index_for_best_path;
        min_distance = min_distance_for_best_path;
    } else if (hmpl::distance(points_list_.front(), start_state_) < 0.001) {
        min_distance = 0;
        min_index = 0;
    } else {
        for (size_t i = 0; i != point_num_; ++i) {
            double tmp_distance = hmpl::distance(points_list_[i], start_state_);
            if (tmp_distance < min_distance) {
                min_distance = tmp_distance;
                min_index = i;
            } else if (tmp_distance > 15 && min_distance < 15) {
                break;
            }
        }
    }
    if (min_distance != 0 || min_index != 0) {
        points_list_.erase(points_list_.begin(), points_list_.begin() + min_index);
        point_num_ = points_list_.size();
        // Car on the left: cte > 0; car on the right: cte < 0.
        auto first_point_local = hmpl::globalToLocal(start_state_, points_list_.front());
        if (first_point_local.y < 0) {
            cte = min_distance;
        } else {
            cte = -min_distance;
        }
    }
    double s = 0;
    for (size_t i = 0; i != point_num_; ++i) {
        if (i == 0) {
            s_list_.push_back(0);
        } else {
            double ds = hmpl::distance(points_list_[i], points_list_[i - 1]);
            s += ds;
            s_list_.push_back(s);
        }
        x_list_.push_back(points_list_[i].x);
        y_list_.push_back(points_list_[i].y);
    }
    double max_s = s_list_.back();
    std::cout << "ref path length: " << max_s << std::endl;
    x_spline_.set_points(s_list_, x_list_);
    y_spline_.set_points(s_list_, y_list_);
    // Make the path dense, the interval being 0.3m
    x_list_.clear();
    y_list_.clear();
    s_list_.clear();
    size_t new_points_count = 0;
    for (double new_s = 0; new_s <= max_s; new_s += 0.3) {
        double x = x_spline_(new_s);
        double y = y_spline_(new_s);
        x_list_.push_back(x);
        y_list_.push_back(y);
        s_list_.push_back(new_s);
        ++new_points_count;
    }
    point_num_ = x_list_.size();
    double max_curvature_abs;
    double max_curvature_change_abs;
    getCurvature(x_list_, y_list_, &k_list_, &max_curvature_abs, &max_curvature_change_abs);
    k_spline_.set_points(s_list_, k_list_);

    start_ref_angle = atan2(y_spline_.deriv(1, 0), x_spline_.deriv(1, 0));
    epsi = constraintAngle(start_state_.z - start_ref_angle);
    // If the start heading differs a lot with the ref path, quit.
    if (fabs(epsi) > 75 * M_PI / 180) {
        LOG(WARNING) << "initial epsi is larger than 90°, quit mpc path optimization!";
        return false;
    }

    // Divid the reference path. Intervals are smaller at the beginning.
    double delta_s_smaller = 0.5;
    double delta_s_larger = 1.0;
    if (fabs(epsi) < 20 * M_PI / 180) delta_s_smaller = delta_s_larger;
    double tmp_max_s = delta_s_smaller;
    seg_s_list_.push_back(0);
    while (tmp_max_s < max_s) {
        seg_s_list_.push_back(tmp_max_s);
        if (tmp_max_s <= 2) {
            tmp_max_s += delta_s_smaller;
        } else {
            tmp_max_s += delta_s_larger;
        }
    }
    if (max_s - seg_s_list_.back() > 1) {
        seg_s_list_.push_back(max_s);
    }
    auto N = seg_s_list_.size();
    auto original_N = N;

    // Store reference states in vectors. They will be used later.
    for (size_t i = 0; i != N; ++i) {
        double length_on_ref_path = seg_s_list_[i];
        double angle;
        angle = atan2(y_spline_.deriv(1, length_on_ref_path), x_spline_.deriv(1, length_on_ref_path));
        seg_angle_list_.push_back(angle);
        seg_x_list_.push_back(x_spline_(length_on_ref_path));
        seg_y_list_.push_back(y_spline_(length_on_ref_path));
        seg_k_list_.push_back(k_spline_(length_on_ref_path));
    }

    // Get clearance of covering circles.
    for (size_t i = 0; i != N; ++i) {
        hmpl::State center_state;
        center_state.x = seg_x_list_[i];
        center_state.y = seg_y_list_[i];
        center_state.s = seg_s_list_[i];
        center_state.z = seg_angle_list_[i];
        // Function getClearance uses the center position as input.
        if (car_type == ACKERMANN_STEERING) {
            center_state.x += rear_axle_to_center_dis * cos(center_state.z);
            center_state.y += rear_axle_to_center_dis * sin(center_state.z);
        }
        std::vector<double> clearance;
        bool safety_margin_flag;
        if (seg_s_list_[i] < 10) safety_margin_flag = false;
        else safety_margin_flag = true;
        clearance = getClearanceFor3Circles(center_state, car_geo_, safety_margin_flag);
        if ((clearance[0] == clearance[1] || clearance[2] == clearance[3] || clearance[4] == clearance[5]
            || clearance[6] == clearance[7])
            && center_state.s > 0.75 * max_s) {
            printf("some states near end are not satisfying\n");
            N = i;
            seg_x_list_.erase(seg_x_list_.begin() + i, seg_x_list_.end());
            seg_y_list_.erase(seg_y_list_.begin() + i, seg_y_list_.end());
            seg_s_list_.erase(seg_s_list_.begin() + i, seg_s_list_.end());
            seg_k_list_.erase(seg_k_list_.begin() + i, seg_k_list_.end());
            seg_angle_list_.erase(seg_angle_list_.begin() + i, seg_angle_list_.end());
            break;
        }
        seg_clearance_list_.push_back(clearance);
    }
    auto po_pre = std::clock();

    // OSQP:
    OsqpEigen::Solver solver;
    solver.settings()->setVerbosity(false);
    solver.settings()->setWarmStart(true);
    solver.data()->setNumberOfVariables(3 * N - 1);
    solver.data()->setNumberOfConstraints(9 * N - 1);
    // Allocate QP problem matrices and vectors.
    Eigen::SparseMatrix<double> hessian;
    Eigen::VectorXd gradient = Eigen::VectorXd::Zero(3 * N - 1);
    Eigen::SparseMatrix<double> linearMatrix;
    Eigen::VectorXd lowerBound;
    Eigen::VectorXd upperBound;
    setHessianMatrix(N, &hessian);
    std::vector<double> init_state;
    init_state.push_back(epsi);
    init_state.push_back(cte);
    bool constriant_end_psi = false;
    if (N == original_N) constriant_end_psi = true;
    setConstraintMatrix(N, &linearMatrix, &lowerBound, &upperBound, init_state, end_state_.z, constriant_end_psi);
    auto po_osqp_pre = std::clock();
//    std::streamsize prec = std::cout.precision();
//    std::cout << std::setprecision(4);
//    std::cout << "hessian:\n " << hessian << std::endl;
//    std::cout << "constraints:\n " << linearMatrix << std::endl;
//    std::cout << "lb:\n " << lowerBound << std::endl;
//    std::cout << "ub:\n " << upperBound << std::endl;
//    std::cout << std::setprecision(prec);
    // Input to solver.
    if (!solver.data()->setHessianMatrix(hessian)) return false;
    if (!solver.data()->setGradient(gradient)) return false;
    if (!solver.data()->setLinearConstraintsMatrix(linearMatrix)) return false;
    if (!solver.data()->setLowerBound(lowerBound)) return false;
    if (!solver.data()->setUpperBound(upperBound)) return false;
    // Solve.
    if (!solver.initSolver()) return false;
    if (!solver.solve()) return false;
    Eigen::VectorXd QPSolution = solver.getSolution();
    auto po_osqp_solve = std::clock();
//    std::cout << "result: " << QPSolution << std::endl;

//    // IPOPT:
//    typedef CPPAD_TESTVECTOR(double) Dvector;
//    // n_vars: Set the number of model variables.
//    // There are 2 state variables: pq and psi;
//    // and 1 control variable: steer angle.
//    size_t n_vars = 2 * N + N - 1;
//    const size_t pq_range_begin = 0;
//    const size_t psi_range_begin = pq_range_begin + N;
//    const size_t steer_range_begin = psi_range_begin + N;
//    // Set the number of constraints
//    Dvector vars(n_vars);
//    for (size_t i = 0; i < n_vars; i++) {
//        vars[i] = 0;
//    }
//    vars[pq_range_begin] = cte;
//    vars[psi_range_begin] = epsi;
//    vars[steer_range_begin] = atan(start_state_.k * wheel_base);
//    // bounds of variables
//    Dvector vars_lowerbound(n_vars);
//    Dvector vars_upperbound(n_vars);
//    for (size_t i = 0; i != steer_range_begin; ++i) {
//        vars_lowerbound[i] = -DBL_MAX;
//        vars_upperbound[i] = DBL_MAX;
//    }
//    double expected_end_psi = constraintAngle(end_state_.z - seg_angle_list_.back());
//    if (original_N == N && fabs(expected_end_psi) < M_PI_2) {
//        double loose_end_psi = 10 * M_PI / 180;
//        vars_lowerbound[steer_range_begin - 1] = expected_end_psi - loose_end_psi;
//        vars_upperbound[steer_range_begin - 1] = expected_end_psi + loose_end_psi;
//    }
//    for (size_t i = steer_range_begin; i != n_vars; ++i) {
//        vars_lowerbound[i] = -30 * M_PI / 180;
//        vars_upperbound[i] = 30 * M_PI / 180;
//    }
//    vars_lowerbound[steer_range_begin] = atan(start_state_.k * wheel_base);
//    vars_upperbound[steer_range_begin] = atan(start_state_.k * wheel_base);
//    // Costraints inclued the state variable constraints(2 * N) and the covering circles constraints(4 * N).
//    // TODO: add steer change constraint.
//    size_t n_constraints = 2 * N + 4 * N;
//    Dvector constraints_lowerbound(n_constraints);
//    Dvector constraints_upperbound(n_constraints);
//    const size_t cons_pq_range_begin = 0;
//    const size_t cons_psi_range_begin = cons_pq_range_begin + N;
//    const size_t cons_c0_range_begin = cons_psi_range_begin + N;
//    const size_t cons_c1_range_begin = cons_c0_range_begin + N;
//    const size_t cons_c2_range_begin = cons_c1_range_begin + N;
//    const size_t cons_c3_range_begin = cons_c2_range_begin + N;
//    for (size_t i = cons_pq_range_begin; i != cons_c0_range_begin; ++i) {
//        constraints_upperbound[i] = 0;
//        constraints_lowerbound[i] = 0;
//    }
//    constraints_lowerbound[cons_pq_range_begin] = cte;
//    constraints_upperbound[cons_pq_range_begin] = cte;
//    constraints_lowerbound[cons_psi_range_begin] = epsi;
//    constraints_upperbound[cons_psi_range_begin] = epsi;
//    // clearance constraints for covering circles.
//    for (size_t i = 0; i != N; ++i) {
//        constraints_upperbound[cons_c0_range_begin + i] = seg_clearance_list_[i][0];
//        constraints_lowerbound[cons_c0_range_begin + i] = seg_clearance_list_[i][1];
//        constraints_upperbound[cons_c1_range_begin + i] = seg_clearance_list_[i][2];
//        constraints_lowerbound[cons_c1_range_begin + i] = seg_clearance_list_[i][3];
//        constraints_upperbound[cons_c2_range_begin + i] = seg_clearance_list_[i][4];
//        constraints_lowerbound[cons_c2_range_begin + i] = seg_clearance_list_[i][5];
//        constraints_upperbound[cons_c3_range_begin + i] = seg_clearance_list_[i][6];
//        constraints_lowerbound[cons_c3_range_begin + i] = seg_clearance_list_[i][7];
//    }
//
//    // options for IPOPT solver
//    std::string options;
//    // Uncomment this if you'd like more print information
//    options += "Integer print_level  0\n";
//    // NOTE: Setting sparse to true allows the solver to take advantage
//    // of sparse routines, this makes the computation MUCH FASTER. If you
//    // can uncomment 1 of these and see if it makes a difference or not but
//    // if you uncomment both the computation time should go up in orders of
//    // magnitude.
//    options += "Sparse  true        forward\n";
//    options += "Sparse  true        reverse\n";
//    // NOTE: Currently the solver has a maximum time limit of 0.1 seconds.
//    // Change this as you see fit.
////    options += "Numeric max_cpu_time          0.1\n";
//    /// Options for QP problem
//    options += "mehrotra_algorithm  yes\n";
//    options += "hessian_constant  yes\n";
//    options += "jac_c_constant  yes\n";
//    options += "jac_d_constant  yes\n";
//    options += "mu_strategy adaptive\n";
//
//    // place to return solution
//    CppAD::ipopt::solve_result<Dvector> solution;
//    // weights of the cost function
//    // TODO: use a config file
//    std::vector<double> weights;
//    weights.push_back(10); //curvature weight
//    weights.push_back(1000); //curvature rate weight
//    weights.push_back(0.01); //distance to boundary weight
//    weights.push_back(0.01); //offset weight
//
//    FgEvalFrenet fg_eval_frenet(seg_x_list_,
//                                seg_y_list_,
//                                seg_angle_list_,
//                                seg_k_list_,
//                                seg_s_list_,
//                                N,
//                                weights,
//                                start_state_,
//                                car_geo_,
//                                seg_clearance_list_,
//                                epsi);
//    // solve the problem
//    CppAD::ipopt::solve<Dvector, FgEvalFrenet>(options, vars,
//                                               vars_lowerbound, vars_upperbound,
//                                               constraints_lowerbound, constraints_upperbound,
//                                               fg_eval_frenet, solution);
//    // Check if it works
//    bool ok = true;
//    ok &= solution.status == CppAD::ipopt::solve_result<Dvector>::success;
//    if (!ok) {
//        LOG(WARNING) << "path optimization solver failed!";
//        return false;
//    }
//    LOG(INFO) << "path optimization solver succeeded!";
//    // output
//    for (size_t i = 0; i != N; i++) {
//        double tmp[2] = {solution.x[i], double(i)};
//        std::vector<double> v(tmp, tmp + sizeof tmp / sizeof tmp[0]);
//        this->predicted_path_in_frenet_.push_back(v);
//        printf("ip: %d, %f\n", i, v[0]);
//    }
    size_t control_points_num = N;
    if (control_sampling_first_flag_) {
        control_points_num = N + best_path.size() - 1;
    }
    tinyspline::BSpline b_spline(control_points_num);
    std::vector<tinyspline::real> ctrlp = b_spline.controlPoints();
    size_t control_sampling_point_count = 0;
    if (control_sampling_first_flag_) {
        for (; control_sampling_point_count != best_path.size() - 1; ++control_sampling_point_count) {
            ctrlp[2 * control_sampling_point_count] = best_path.at(control_sampling_point_count).x;
            ctrlp[2 * control_sampling_point_count + 1] = best_path.at(control_sampling_point_count).y;
        }
    }
    // The first two points are fixed. They are not in the optimized variables.
    size_t count = 0;
    if (control_sampling_first_flag_) {
        count = 2 * (control_sampling_point_count);
    }
    for (size_t i = 0; i != N; ++i) {
        double length_on_ref_path = seg_s_list_[i];
        double angle = seg_angle_list_[i];
        double new_angle = constraintAngle(angle + M_PI_2);
//        double tmp_x = x_spline_(length_on_ref_path) + predicted_path_in_frenet_[i][0] * cos(new_angle);
//        double tmp_y = y_spline_(length_on_ref_path) + predicted_path_in_frenet_[i][0] * sin(new_angle);
        double tmp_x = x_spline_(length_on_ref_path) + QPSolution(2 * i + 1) * cos(new_angle);
        double tmp_y = y_spline_(length_on_ref_path) + QPSolution(2 * i + 1) * sin(new_angle);
        if (std::isnan(tmp_x) || std::isnan(tmp_y)) {
            LOG(WARNING) << "output is not a number, path opitmization failed!" << std::endl;
            return false;
        }
        ctrlp[count + 2 * i] = tmp_x;
        ctrlp[count + 2 * i + 1] = tmp_y;
    }
    // B spline
    b_spline.setControlPoints(ctrlp);
    std::vector<hmpl::State> tmp_final_path;
    double step_t = 1.0 / (4.0 * control_points_num);
    std::vector<tinyspline::real> result;
    std::vector<tinyspline::real> result_next;
    for (size_t i = 0; i <= 4 * control_points_num; ++i) {
//    for (size_t i = 0; i < control_points_num; ++i) {
        double t = i * step_t;
        if (i == 0) result = b_spline.eval(t).result();
        hmpl::State state;
        state.x = result[0];
        state.y = result[1];
//        state.x = ctrlp[2 * i];
//        state.y = ctrlp[2 * i + 1];
        if (i == 4 * control_points_num) {
            state.z = tmp_final_path.back().z;
        } else {
            result_next = b_spline.eval((i + 1) * step_t).result();
            double dx = result_next[0] - result[0];
            double dy = result_next[1] - result[1];
            state.z = atan2(dy, dx);
//            total_s += sqrt(pow(dx, 2) + pow(dy, 2));
//            state.s = total_s;
        }
//        if (i == N - 1) {
//            state.z = tmp_final_path[i - 1].z;
//        } else {
////            double dx = result[0] - tmp_final_path[i - 1].x;
////            double dy = result[1] - tmp_final_path[i - 1].y;
//            double dx = ctrlp[2 * i + 2] - state.x;
//            double dy = ctrlp[2 * i + 3] - state.y;
//            state.z = atan2(dy, dx);
////            total_s += sqrt(pow(dx, 2) + pow(dy, 2));;
//        }
        result = result_next;
        if (collision_checker_.isSingleStateCollisionFreeImproved(state)) {
            tmp_final_path.push_back(state);
        } else {
            printf("path optimization collision check failed at %d of %d\n", i, 2 * N);
//            if (i >= 2 * N - 2 || state.s > 30) {
            break;
//            }
//            return false;
//            tmp_final_path.push_back(state);
        }
    }
    auto po_BSpline = std::clock();
    printf(
        "*****************************\ntime cost:\npre process: %f\nosqp pre process: %f\nosqp solve: %f\nB spline: %f\n*****************************\n",
        (double) (po_pre - po_start) / CLOCKS_PER_SEC,
        (double) (po_osqp_pre - po_pre) / CLOCKS_PER_SEC,
        (double) (po_osqp_solve - po_osqp_pre) / CLOCKS_PER_SEC,
        (double) (po_BSpline - po_osqp_solve) / CLOCKS_PER_SEC);
    final_path->clear();
    std::copy(tmp_final_path.begin(), tmp_final_path.end(), std::back_inserter(*final_path));
    return true;
}

double PathOptimizer::getClearanceWithDirectionStrict(hmpl::State state, double angle, double radius) {
    double s = 0;
    double delta_s = 0.1;
    size_t n = 5.0 / delta_s;
    for (size_t i = 0; i != n; ++i) {
        s += delta_s;
        double x = state.x + s * cos(angle);
        double y = state.y + s * sin(angle);
        grid_map::Position new_position(x, y);
        double clearance = grid_map_.getObstacleDistance(new_position);
        if (clearance <= radius) {
            return s - delta_s;
        }
    }
    return s;
}

std::vector<double> PathOptimizer::getClearanceWithDirectionStrict(hmpl::State state,
                                                                   double radius,
                                                                   bool safety_margin_flag) {
    double left_bound = 0;
    double right_bound = 0;
    double left_s = 0;
    double delta_s = 0.1;
    double left_angle = constraintAngle(state.z + M_PI_2);
    double right_angle = constraintAngle(state.z - M_PI_2);
    size_t n = 5.0 / delta_s;
    for (size_t i = 0; i != n; ++i) {
        left_s += delta_s;
        double x = state.x + left_s * cos(left_angle);
        double y = state.y + left_s * sin(left_angle);
        grid_map::Position new_position(x, y);
        double clearance = grid_map_.getObstacleDistance(new_position);
        if (clearance <= radius && i != 0) {
            left_bound = left_s - delta_s;
            break;
        } else if (clearance <= radius && i == 0) {
            double right_s = 0;
            for (size_t j = 0; j != n / 2; ++j) {
                right_s += delta_s;
                double x = state.x + right_s * cos(right_angle);
                double y = state.y + right_s * sin(right_angle);
                grid_map::Position new_position(x, y);
                double clearance = grid_map_.getObstacleDistance(new_position);
                if (clearance > radius) {
                    left_bound = -right_s;
                    break;
                } else if (j == n / 2 - 1) {
                    double tmp_s = 0;
                    for (size_t k = 0; k != n; ++k) {
                        tmp_s += delta_s;
                        double x = state.x + tmp_s * cos(left_angle);
                        double y = state.y + tmp_s * sin(left_angle);
                        grid_map::Position new_position(x, y);
                        double clearance = grid_map_.getObstacleDistance(new_position);
                        if (clearance > radius) {
                            right_bound = tmp_s;
                            break;
                        } else if (k == n - 1) {
                            std::vector<double> failed_result{0, 0};
//                            printf("get a failure\n");
                            return failed_result;
                        }
                    }
//                    printf("tmp_s: %f\n", tmp_s);
                    for (size_t k = 0; k != n; ++k) {
                        tmp_s += delta_s;
                        double x = state.x + tmp_s * cos(left_angle);
                        double y = state.y + tmp_s * sin(left_angle);
                        grid_map::Position new_position(x, y);
                        double clearance = grid_map_.getObstacleDistance(new_position);
//                        printf("new tmp_s: %f\n", tmp_s);
                        if (clearance <= radius) {
                            left_bound = tmp_s - delta_s;
                            break;
                        } else if (k == n - 1) {
                            left_bound = tmp_s;
                        }
                    }
//                    printf("get range: %f to %f \n", left_bound, right_bound);
                    std::vector<double> result{left_bound, right_bound};
                    return result;
                }
            }
            break;
        } else if (i == n - 1) {
            left_bound = left_s;
        }
    }
    if (left_bound > 0) n *= 2;
    double right_s = -left_bound;
    for (size_t i = 0; i != n; ++i) {
        right_s += delta_s;
        double x = state.x + right_s * cos(right_angle);
        double y = state.y + right_s * sin(right_angle);
        grid_map::Position new_position(x, y);
        double clearance = grid_map_.getObstacleDistance(new_position);
        if (clearance <= radius) {
            right_bound = -(right_s - delta_s);
            break;
        } else if (i == n - 1) {
            right_bound = -right_s;
        }
    }
    if (safety_margin_flag) {
        double base = std::max(left_bound - right_bound - 0.6, 0.0);
        double safety_margin = std::min(base * 0.24, 0.85);
//        printf("safety margin: %f; bounds: %f, %f\n", safety_margin, left_bound - safety_margin, right_bound + safety_margin);
        left_bound -= safety_margin;
        right_bound += safety_margin;
    }
    std::vector<double> result{left_bound, right_bound};
//    printf("get range: %f to %f \n", left_bound, right_bound);
    return result;
}

std::vector<double> PathOptimizer::getClearanceFor3Circles(const hmpl::State &state,
                                                           const std::vector<double> &car_geometry,
                                                           bool safety_margin_flag) {
//    double rear_front_radius = car_geometry[2];
//    double middle_radius = car_geometry[3];
    double circle_r = car_geometry[4];
    hmpl::State c0, c1, c2, c3;
    double center_x = state.x;
    double center_y = state.y;
    double c0_x = center_x + car_geometry[0] * cos(state.z);
    double c0_y = center_y + car_geometry[0] * sin(state.z);
    double c1_x = center_x + car_geometry[1] * cos(state.z);
    double c1_y = center_y + car_geometry[1] * sin(state.z);
    double c2_x = center_x + car_geometry[2] * cos(state.z);
    double c2_y = center_y + car_geometry[2] * sin(state.z);
    double c3_x = center_x + car_geometry[3] * cos(state.z);
    double c3_y = center_y + car_geometry[3] * sin(state.z);
//    center.x = center_x;
//    center.y = center_y;
//    center.z = state.z;
    c0.x = c0_x;
    c0.y = c0_y;
    c0.z = state.z;
    c1.x = c1_x;
    c1.y = c1_y;
    c1.z = state.z;
    c2.x = c2_x;
    c2.y = c2_y;
    c2.z = state.z;
    c3.x = c3_x;
    c3.y = c3_y;
    c3.z = state.z;
    std::vector<double> result;
    std::vector<double> c0_bounds = getClearanceWithDirectionStrict(c0, circle_r, safety_margin_flag);
    std::vector<double> c1_bounds = getClearanceWithDirectionStrict(c1, circle_r, safety_margin_flag);
    std::vector<double> c2_bounds = getClearanceWithDirectionStrict(c2, circle_r, safety_margin_flag);
    std::vector<double> c3_bounds = getClearanceWithDirectionStrict(c3, circle_r, safety_margin_flag);
//    printf("c0_bounds: %f, %f\n", c0_bounds[0], c0_bounds[1]);
//    printf("c1_bounds: %f, %f\n", c1_bounds[0], c1_bounds[1]);
//    printf("c2_bounds: %f, %f\n", c2_bounds[0], c2_bounds[1]);
//    printf("c3_bounds: %f, %f\n", c3_bounds[0], c3_bounds[1]);
//    printf("use safety margin? %d\n", safety_margin_flag);
    result.push_back(c0_bounds[0]);
    result.push_back(c0_bounds[1]);
    result.push_back(c1_bounds[0]);
    result.push_back(c1_bounds[1]);
    result.push_back(c2_bounds[0]);
    result.push_back(c2_bounds[1]);
    result.push_back(c3_bounds[0]);
    result.push_back(c3_bounds[1]);
    // For visualization purpoose:
    hmpl::State rear_bound_l, center_bound_l, front_bound_l, rear_bound_r, center_bound_r, front_bound_r;
    rear_bound_l.x = c0_x + result[0] * cos(state.z + M_PI_2);
    rear_bound_l.y = c0_y + result[0] * sin(state.z + M_PI_2);
    rear_bound_r.x = c0_x + result[1] * cos(state.z + M_PI_2);
    rear_bound_r.y = c0_y + result[1] * sin(state.z + M_PI_2);
    center_bound_l.x = c2_x + result[4] * cos(state.z + M_PI_2);
    center_bound_l.y = c2_y + result[4] * sin(state.z + M_PI_2);
    center_bound_r.x = c2_x + result[5] * cos(state.z + M_PI_2);
    center_bound_r.y = c2_y + result[5] * sin(state.z + M_PI_2);
    front_bound_l.x = c3_x + result[6] * cos(state.z + M_PI_2);
    front_bound_l.y = c3_y + result[6] * sin(state.z + M_PI_2);
    front_bound_r.x = c3_x + result[7] * cos(state.z + M_PI_2);
    front_bound_r.y = c3_y + result[7] * sin(state.z + M_PI_2);
    rear_bounds_.push_back(rear_bound_l);
    rear_bounds_.push_back(rear_bound_r);
    center_bounds_.push_back(center_bound_l);
    center_bounds_.push_back(center_bound_r);
    front_bounds_.push_back(front_bound_l);
    front_bounds_.push_back(front_bound_r);
    return result;

}

//double PathOptimizer::getClearanceWithDirection(const hmpl::State &state,
//                                                double angle,
//                                                const std::vector<double> &car_geometry) {
//    double s = 0;
//    double delta_s = 0.1;
//    size_t n = 5.0 / delta_s;
//    for (size_t i = 0; i != n; ++i) {
//        s += delta_s;
//        double x = state.x + s * cos(angle);
//        double y = state.y + s * sin(angle);
//        double rear_x = x - car_geometry[0] * cos(state.z);
//        double rear_y = y - car_geometry[0] * sin(state.z);
//        double front_x = x + car_geometry[1] * cos(state.z);
//        double front_y = y + car_geometry[1] * sin(state.z);
//        grid_map::Position new_position(x, y);
//        grid_map::Position new_rear_position(rear_x, rear_y);
//        grid_map::Position new_front_position(front_x, front_y);
//        if (grid_map_.maps.isInside(new_position) && grid_map_.maps.isInside(new_rear_position)
//            && grid_map_.maps.isInside(new_front_position)) {
//            double new_rear_clearance = grid_map_.getObstacleDistance(new_rear_position);
//            double new_front_clearance = grid_map_.getObstacleDistance(new_front_position);
//            double new_middle_clearance = grid_map_.getObstacleDistance(new_position);
//            if (std::min(new_rear_clearance, new_front_clearance) < car_geometry[2]
//                || new_middle_clearance < car_geometry[3]) {
//                return s - delta_s;
//            }
//        } else {
//            return s - delta_s;
//        }
//    }
//    return s;
//}

//std::vector<double> PathOptimizer::getClearance(hmpl::State state,
//                                                double ref_angle,
//                                                const std::vector<double> &car_geometry) {
//    // Check if the current state has collision.
//    double rear_center_distance = car_geometry[0];
//    double front_center_distance = car_geometry[1];
//    double rear_front_radius = car_geometry[2];
//    double middle_radius = car_geometry[3];
//    // Calculate the position of the rear center, middle center and front center on current state.
//    grid_map::Position
//        rear_position(state.x - rear_center_distance * cos(state.z), state.y - rear_center_distance * sin(state.z));
//    grid_map::Position
//        middle_position(state.x, state.y);
//    grid_map::Position
//        front_position(state.x + front_center_distance * cos(state.z), state.y + front_center_distance * sin(state.z));
//    double rear_clearance = grid_map_.getObstacleDistance(rear_position);
//    double front_clearance = grid_map_.getObstacleDistance(front_position);
//    double middle_clearance = grid_map_.getObstacleDistance(middle_position);
//    // If the current state is collision free, then expand to left and right.
//    if (std::min(rear_clearance, front_clearance) > rear_front_radius && middle_clearance > middle_radius) {
//        double left_clearance = getClearanceWithDirection(state, constraintAngle(ref_angle + M_PI_2), car_geometry);
//        double right_clearance = -getClearanceWithDirection(state, constraintAngle(ref_angle - M_PI_2), car_geometry);
//        std::vector<double> clearance{left_clearance, right_clearance};
//        return clearance;
//    } else {
//        // If the current state has collision, then search to left OR right until find a clear range.
//        double left_limit, right_limit;
//        bool left_exploration_succeed_flag = false;
//        bool right_exploration_succeed_flag = false;
//        // explore left:
//        double s = 0;
//        double delta_s = 0.1;
//        size_t n = 5.0 / delta_s;
//        for (size_t i = 0; i != n; ++i) {
//            s += delta_s;
//            double x = state.x + s * cos(constraintAngle(ref_angle + M_PI_2));
//            double y = state.y + s * sin(constraintAngle(ref_angle + M_PI_2));
//            double rear_x = x - rear_center_distance * cos(state.z);
//            double rear_y = y - rear_center_distance * sin(state.z);
//            double front_x = x + front_center_distance * cos(state.z);
//            double front_y = y + front_center_distance * sin(state.z);
//            grid_map::Position new_position(x, y);
//            grid_map::Position new_rear_position(rear_x, rear_y);
//            grid_map::Position new_front_position(front_x, front_y);
//            // Search to left until a clear position is found, then continue searching to left until
//            // a collision position is found.
//            if (grid_map_.maps.isInside(new_position) && grid_map_.maps.isInside(new_rear_position)
//                && grid_map_.maps.isInside(new_front_position)) {
//                double new_rear_clearance = grid_map_.getObstacleDistance(new_rear_position);
//                double new_front_clearance = grid_map_.getObstacleDistance(new_front_position);
//                double new_middle_clearance = grid_map_.getObstacleDistance(new_position);
//                if (std::min(new_rear_clearance, new_front_clearance) > rear_front_radius
//                    && new_middle_clearance > middle_radius) {
//                    left_exploration_succeed_flag = true;
//                    right_limit = s;
//                    hmpl::State new_state;
//                    new_state.x = x;
//                    new_state.y = y;
//                    new_state.z = state.z;
//                    left_limit = right_limit
//                        + getClearanceWithDirection(new_state, constraintAngle(ref_angle + M_PI_2), car_geometry);
//                    break;
//                }
//            }
//        }
//        if (!left_exploration_succeed_flag) {
//            // explore right:
//            double s = 0;
//            double delta_s = 0.1;
//            size_t n = 5.0 / delta_s;
//            for (size_t i = 0; i != n; ++i) {
//                s += delta_s;
//                double x = state.x + s * cos(constraintAngle(ref_angle - M_PI_2));
//                double y = state.y + s * sin(constraintAngle(ref_angle - M_PI_2));
//                double rear_x = x - rear_center_distance * cos(state.z);
//                double rear_y = y - rear_center_distance * sin(state.z);
//                double front_x = x + front_center_distance * cos(state.z);
//                double front_y = y + front_center_distance * sin(state.z);
//                grid_map::Position new_position(x, y);
//                grid_map::Position new_rear_position(rear_x, rear_y);
//                grid_map::Position new_front_position(front_x, front_y);
//                if (grid_map_.maps.isInside(new_position) && grid_map_.maps.isInside(new_rear_position)
//                    && grid_map_.maps.isInside(new_front_position)) {
//                    double new_rear_clearance = grid_map_.getObstacleDistance(new_rear_position);
//                    double new_front_clearance = grid_map_.getObstacleDistance(new_front_position);
//                    double new_middle_clearance = grid_map_.getObstacleDistance(new_position);
//                    if (std::min(new_rear_clearance, new_front_clearance) > rear_front_radius
//                        && new_middle_clearance > middle_radius) {
//                        right_exploration_succeed_flag = true;
//                        left_limit = -s;
//                        hmpl::State new_state;
//                        new_state.x = x;
//                        new_state.y = y;
//                        new_state.z = state.z;
//                        right_limit = left_limit
//                            - getClearanceWithDirection(new_state, constraintAngle(ref_angle - M_PI_2), car_geometry);
//                        break;
//                    }
//
//                }
//            }
//        }
//        if (left_exploration_succeed_flag || right_exploration_succeed_flag) {
//            return std::vector<double>{left_limit, right_limit};
//        } else {
//            return std::vector<double>{0, 0};
//        }
//    }
//}

double PathOptimizer::getClearanceWithDirection(const hmpl::State &state, double angle) {
    double s = 0;
    double delta_s = 0.1;
    size_t n = 5.0 / delta_s;
    for (size_t i = 0; i != n; ++i) {
        s += delta_s;
        double x = state.x + s * cos(angle);
        double y = state.y + s * sin(angle);
        grid_map::Position new_position(x, y);
        if (grid_map_.maps.isInside(new_position)) {
            if (grid_map_.maps.atPosition("obstacle", new_position) == 0) {
//            if (grid_map_.getObstacleDistance(new_position) <= 1.45) {
                return s - delta_s;
            }
        } else {
            return s - delta_s;
        }
    }
    return s;
}

double PathOptimizer::getPointCurvature(const double &x1,
                                        const double &y1,
                                        const double &x2,
                                        const double &y2,
                                        const double &x3,
                                        const double &y3) {
    double_t a, b, c;
    double_t delta_x, delta_y;
    double_t s;
    double_t A;
    double_t curv;
    double_t rotate_direction;

    delta_x = x2 - x1;
    delta_y = y2 - y1;
    a = sqrt(pow(delta_x, 2.0) + pow(delta_y, 2.0));

    delta_x = x3 - x2;
    delta_y = y3 - y2;
    b = sqrt(pow(delta_x, 2.0) + pow(delta_y, 2.0));

    delta_x = x1 - x3;
    delta_y = y1 - y3;
    c = sqrt(pow(delta_x, 2.0) + pow(delta_y, 2.0));

    s = (a + b + c) / 2.0;
    A = sqrt(fabs(s * (s - a) * (s - b) * (s - c)));
    curv = 4 * A / (a * b * c);

    /* determine the sign, using cross product(叉乘)
     * 2维空间中的叉乘是： A x B = |A||B|Sin(\theta)
     * V1(x1, y1) X V2(x2, y2) = x1y2 – y1x2
     */
    rotate_direction = (x2 - x1) * (y3 - y2) - (y2 - y1) * (x3 - x2);
    if (rotate_direction < 0) {
        curv = -curv;
    }
    return curv;
}

void PathOptimizer::getCurvature(const std::vector<double> &local_x,
                                 const std::vector<double> &local_y,
                                 std::vector<double> *pt_curvature_out,
                                 double *max_curvature_abs,
                                 double *max_curvature_change_abs) {
    assert(local_x.size() == local_y.size());
    unsigned long size_n = local_x.size();
    std::vector<double> curvature = std::vector<double>(size_n);
    for (size_t i = 1; i < size_n - 1; ++i) {
        double x1 = local_x.at(i - 1);
        double x2 = local_x.at(i);
        double x3 = local_x.at(i + 1);
        double y1 = local_y.at(i - 1);
        double y2 = local_y.at(i);
        double y3 = local_y.at(i + 1);
        curvature.at(i) = getPointCurvature(x1, y1, x2, y2, x3, y3);
    }
    curvature.at(0) = curvature.at(1);
    curvature.at(size_n - 1) = curvature.at(size_n - 2);
    double final_curvature;
    double max_curvature = 0;
    double max_curvature_change = 0;
    for (size_t j = 0; j < size_n; ++j) {
        final_curvature = curvature[j];
        pt_curvature_out->push_back(final_curvature);
        if (fabs(final_curvature) > max_curvature) {
            max_curvature = fabs(final_curvature);
        }
        if (j != size_n - 1) {
            double curvature_change = fabs(curvature[j] - curvature[j + 1]);
            if (curvature_change > max_curvature_change) {
                max_curvature_change = curvature_change;
            }
        }
    }
    *max_curvature_abs = max_curvature;
    *max_curvature_change_abs = max_curvature_change;
}

const std::vector<std::vector<hmpl::State> > &PathOptimizer::getControlSamplingPathSet() {
    return this->sampling_path_set_;
};

const std::vector<std::vector<hmpl::State> > &PathOptimizer::getControlSamplingFailedPathSet() {
    return this->failed_sampling_path_set_;
};

const std::vector<hmpl::State> &PathOptimizer::getBestSamplingPath() {
    if (control_sampling_first_flag_) {
        return this->sampling_path_set_[best_sampling_index_];
    } else {
        return this->empty_;
    }
}

const std::vector<hmpl::State> &PathOptimizer::getLeftBound() {
    return this->left_bound_;
}

const std::vector<hmpl::State> &PathOptimizer::getRightBound() {
    return this->right_bound_;
}

const std::vector<hmpl::State> &PathOptimizer::getSecondThirdPoint() {
    return this->second_third_point_;
}

const std::vector<hmpl::State> &PathOptimizer::getRearBounds() {
    return this->rear_bounds_;
}

const std::vector<hmpl::State> &PathOptimizer::getCenterBounds() {
    return this->center_bounds_;
}

const std::vector<hmpl::State> &PathOptimizer::getFrontBounds() {
    return this->front_bounds_;
}

const std::vector<hmpl::State> &PathOptimizer::getSmoothedPath() {
    return this->smoothed_path_;
}
}
