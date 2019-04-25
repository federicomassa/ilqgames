/*
 * Copyright (c) 2019, The Regents of the University of California (Regents).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *    3. Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS AS IS
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Please contact the author(s) of this library if you have any questions.
 * Authors: David Fridovich-Keil   ( dfk@eecs.berkeley.edu )
 */

///////////////////////////////////////////////////////////////////////////////
//
// Base class for all iterative LQ game solvers.
// Structured so that derived classes may only modify the `ModifyLQStrategies`
// and `HasConverged` virtual functions.
//
///////////////////////////////////////////////////////////////////////////////

#include <ilqgames/cost/player_cost.h>
#include <ilqgames/solver/ilqgame.h>
#include <ilqgames/solver/solve_lq_game.h>
#include <ilqgames/utils/linear_dynamics_approximation.h>
#include <ilqgames/utils/operating_point.h>
#include <ilqgames/utils/quadratic_cost_approximation.h>
#include <ilqgames/utils/strategy.h>
#include <ilqgames/utils/types.h>

#include <memory>
#include <vector>

namespace ilqgames {

bool ILQGame::Solve(const VectorXf& x0,
                    const OperatingPoint& initial_operating_point,
                    const std::vector<Strategy>& initial_strategies,
                    OperatingPoint* final_operating_point,
                    std::vector<Strategy>* final_strategies, Log* log) {
  CHECK_NOTNULL(final_strategies);
  CHECK_NOTNULL(final_operating_point);

  // Make sure we have enough strategies for each time step.
  DCHECK_EQ(dynamics_->NumPlayers(), initial_strategies.size());
  DCHECK(std::accumulate(
      initial_strategies.begin(), initial_strategies.end(), true,
      [this](bool correct_so_far, const Strategy& strategy) {
        return correct_so_far &=
               strategy.Ps.size() == this->num_time_steps_ &&
               strategy.alphas.size() == this->num_time_steps_;
      }));

  // Last and current operating points.
  OperatingPoint last_operating_point(num_time_steps_, dynamics_->NumPlayers());
  OperatingPoint current_operating_point(initial_operating_point);

  std::cout << "set up operating points" << std::endl;

  // Current strategies.
  std::vector<Strategy> current_strategies(initial_strategies);

  // Preallocate vectors for linearizations and quadraticizations.
  // Both are time-indexed (and quadraticizations' inner vector is indexed by
  // player).
  std::vector<LinearDynamicsApproximation> linearization(num_time_steps_);
  std::vector<std::vector<QuadraticCostApproximation>> quadraticization(
      num_time_steps_);
  for (auto& quads : quadraticization)
    quads.resize(dynamics_->NumPlayers(),
                 QuadraticCostApproximation(dynamics_->XDim()));

  std::cout << "set up quadraticizations and linearizations" << std::endl;

  // Number of iterations, starting from 0.
  size_t num_iterations = 0;

  // Log initial iterate.
  if (log) log->AddSolverIterate(initial_operating_point, initial_strategies);

  std::cout << "added solver iterate to log" << std::endl;

  // Keep iterating until convergence.
  while (!HasConverged(num_iterations, last_operating_point,
                       current_operating_point)) {
    num_iterations++;

    std::cout << "Iteration " << num_iterations << std::endl;

    // Swap operating points and compute new current operating point.
    last_operating_point.swap(current_operating_point);
    CurrentOperatingPoint(x0, last_operating_point, current_strategies,
                          &current_operating_point);

    std::cout << "Got new opertaing point" << std::endl;

    // Linearize dynamics and quadraticize costs for all players about the new
    // operating point.
    for (size_t kk = 0; kk < num_time_steps_; kk++) {
      const Time t = ComputeTimeStamp(kk);
      const auto& x = current_operating_point.xs[kk];
      const auto& us = current_operating_point.us[kk];

      std::cout << "state: " << x.transpose() << std::endl;
      for (PlayerIndex ii = 0; ii < us.size(); ii++)
        std::cout << "control " << ii << ": " << us[ii].transpose()
                  << std::endl;

      // Linearize dynamics.
      linearization[kk] = dynamics_->Linearize(t, time_step_, x, us);

      std::cout << "lin: " << kk << " done." << std::endl;

      // Quadraticize costs.
      std::transform(player_costs_.begin(), player_costs_.end(),
                     quadraticization[kk].begin(),
                     [&t, &x, &us](const PlayerCost& cost) {
                       return cost.Quadraticize(t, x, us);
                     });
    }

    std::cout << "populated lins and quads" << std::endl;

    // Solve LQ game.
    current_strategies =
        SolveLQGame(*dynamics_, linearization, quadraticization);

    std::cout << "solved lq game" << std::endl;

    // Modify this LQ solution.
    if (!ModifyLQStrategies(current_operating_point, &current_strategies))
      return false;

    // Log current iterate.
    if (log) log->AddSolverIterate(current_operating_point, current_strategies);
  }

  // Set final strategies and operating point.
  final_strategies->swap(current_strategies);
  final_operating_point->swap(current_operating_point);

  return true;
}

void ILQGame::CurrentOperatingPoint(
    const VectorXf& x0, const OperatingPoint& last_operating_point,
    const std::vector<Strategy>& current_strategies,
    OperatingPoint* current_operating_point) const {
  CHECK_NOTNULL(current_operating_point);

  // Integrate dynamics and populate operating point, one time step at a time.
  VectorXf x(x0);
  for (size_t kk = 0; kk < num_time_steps_; kk++) {
    Time t = ComputeTimeStamp(kk);

    // Unpack.
    const VectorXf delta_x = x - last_operating_point.xs[kk];
    const auto& last_us = last_operating_point.us[kk];
    auto& current_us = current_operating_point->us[kk];

    // Record state.
    current_operating_point->xs[kk] = x;
    std::cout << "next x: " << x.transpose() << std::endl;
    std::cout << "delta x: " << delta_x.transpose() << std::endl;

    // Compute and record control for each player.
    for (PlayerIndex jj = 0; jj < dynamics_->NumPlayers(); jj++) {
      const auto& strategy = current_strategies[jj];
      std::cout << "last u" << jj << ": " << last_us[jj] << std::endl;

      current_us[jj] = strategy(kk, delta_x, last_us[jj]);
      std::cout << "next u" << jj << ": " << current_us[jj].transpose()
                << std::endl;
    }

    // Integrate dynamics for one time step.
    if (kk < num_time_steps_ - 1)
      x = dynamics_->Integrate(t, time_step_, x, current_us);
  }
}

bool ILQGame::HasConverged(
    size_t iteration, const OperatingPoint& last_operating_point,
    const OperatingPoint& current_operating_point) const {
  // As a simple starting point, we'll say that we've converged if it's been
  // at least 50 iterations or the current operating_point and last operating
  // point are within 0.1 in every dimension at every time.
  constexpr size_t kMaxIterations = 50;
  constexpr float kMaxElementwiseDifference = 0.1;

  // Check iterations.
  if (iteration >= kMaxIterations) return true;
  if (iteration == 0) return false;

  // Check operating points.
  for (size_t kk = 0; kk < num_time_steps_; kk++) {
    VectorXf delta =
        current_operating_point.xs[kk] - last_operating_point.xs[kk];
    if (delta.cwiseAbs().maxCoeff() > kMaxElementwiseDifference) return false;

    const auto& current_us = current_operating_point.us[kk];
    const auto& last_us = last_operating_point.us[kk];
    for (PlayerIndex jj = 0; jj < dynamics_->NumPlayers(); jj++) {
      delta = current_us[jj] - last_us[jj];
      if (delta.cwiseAbs().maxCoeff() > kMaxElementwiseDifference) return false;
    }
  }

  return true;
}

bool ILQGame::ModifyLQStrategies(const OperatingPoint& current_operating_point,
                                 std::vector<Strategy>* strategies) const {
  CHECK_NOTNULL(strategies);

  // As a simple starting point, just scale all the 'alphas' in the strategy to
  // 0.05 of their original value.
  constexpr float kAlphaScalingFactor = 0.05;
  for (auto& strategy : *strategies) {
    for (auto& alpha : strategy.alphas) alpha *= kAlphaScalingFactor;
  }

  return true;
}

}  // namespace ilqgames