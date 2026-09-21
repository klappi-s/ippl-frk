#pragma once
#include "Ippl.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace electrostatic_fem {
    struct ExperimentConfig {
        std::map<std::string, std::string> options;
        unsigned dimension = 1, order = 3, steps = 10, seed = 42, mode = 1, diagnosticsEvery = 1;
        size_t particles = 32768;
        double dt = 0.05, alpha = 0.05, thermalSpeed = 1, tolerance = 1e-10;
        int maxIterations  = 10000;
        std::string solver = "plain", preconditioner = "jacobi", family = "gll", quadrature = "9";
        std::string stiffness = "constant_preserving", reuse = "full", initialGuess = "previous";
        std::string initialCondition = "single", output = "landau-fem", replay,
                    timerLevel = "coarse";
        std::vector<unsigned> cells;
        std::vector<double> lengths;

        template <typename T>
        static std::vector<T> list(std::string value) {
            for (auto& c : value)
                if (c == ',')
                    c = ' ';
            std::istringstream input(value);
            std::vector<T> result;
            T item;
            while (input >> item)
                result.push_back(item);
            if (!input.eof() || result.empty())
                throw std::runtime_error("Invalid axis list: " + value);
            return result;
        }
        std::string get(const std::string& key, const std::string& fallback) const {
            auto it = options.find(key);
            return it == options.end() ? fallback : it->second;
        }
        static ExperimentConfig parse(int argc, char** argv) {
            ExperimentConfig c;
            const std::string allowed =
                " dim order cells lengths particles steps seed mode dt alpha thermal_speed "
                "tolerance max_iterations solver preconditioner_type interpolation_nodes "
                "quadrature_points poisson_stiffness_mode reuse initial_guess initial_condition "
                "output replay diagnostics_every newton_level chebyshev_degree "
                "richardson_iterations gauss_seidel_inner_iterations gauss_seidel_outer_iterations "
                "ssor_omega timer_level info ";
            for (int i = 1; i < argc; ++i) {
                std::string key = argv[i];
                if (!key.starts_with("--") || i + 1 == argc
                    || allowed.find(" " + key.substr(2) + " ") == std::string::npos)
                    throw std::runtime_error("Unknown/incomplete option: " + key);
                if (!c.options.emplace(key.substr(2), argv[++i]).second)
                    throw std::runtime_error("Duplicate option: " + key);
            }
            auto integer = [&](const char* key, int value) {
                const auto text  = c.get(key, std::to_string(value));
                size_t used      = 0;
                const int result = std::stoi(text, &used);
                if (used != text.size() || result < 0)
                    throw std::runtime_error(std::string("Invalid ") + key);
                return result;
            };
            auto real = [&](const char* key, double value) {
                std::ostringstream fallback;
                fallback << std::setprecision(17) << value;
                const auto text     = c.get(key, fallback.str());
                size_t used         = 0;
                const double result = std::stod(text, &used);
                if (used != text.size() || !std::isfinite(result))
                    throw std::runtime_error(std::string("Invalid ") + key);
                return result;
            };
            c.dimension        = integer("dim", 1);
            c.order            = integer("order", 3);
            c.steps            = integer("steps", 10);
            c.seed             = integer("seed", 42);
            c.mode             = integer("mode", 1);
            c.diagnosticsEvery = integer("diagnostics_every", 1);
            c.particles        = integer("particles", 32768);
            c.maxIterations    = integer("max_iterations", 10000);
            c.dt               = real("dt", 0.05);
            c.alpha            = real("alpha", 0.05);
            c.thermalSpeed     = real("thermal_speed", 1);
            // Preserve scientific notation: std::to_string(1e-10) would round to zero.
            c.tolerance        = real("tolerance", 1e-10);
            c.timerLevel       = c.get("timer_level", c.timerLevel);
            c.solver           = c.get("solver", c.solver);
            c.preconditioner   = c.get("preconditioner_type", c.preconditioner);
            c.family           = c.get("interpolation_nodes", c.family);
            c.quadrature       = c.get("quadrature_points", c.quadrature);
            c.stiffness        = c.get("poisson_stiffness_mode", c.stiffness);
            c.reuse            = c.get("reuse", c.reuse);
            c.initialGuess     = c.get("initial_guess", c.initialGuess);
            c.initialCondition = c.get("initial_condition", c.initialCondition);
            c.output           = c.get("output", c.output);
            c.replay           = c.get("replay", "");
            if (c.dimension < 1 || c.dimension > 3 || c.order < 1 || c.order > 5 || !c.particles
                || !c.mode || !c.maxIterations || !c.diagnosticsEvery || c.dt <= 0
                || std::abs(c.alpha) >= 1 || c.thermalSpeed <= 0 || !(c.tolerance > 0)
                || !std::isfinite(c.tolerance))
                throw std::runtime_error("Invalid dimension/order/physics/solver configuration.");
            auto choice = [](const std::string& value, std::initializer_list<const char*> choices) {
                for (auto item : choices)
                    if (value == item)
                        return;
                throw std::runtime_error("Unsupported value: " + value);
            };
            choice(c.solver, {"plain", "preconditioned"});
            choice(c.family, {"gll", "equispaced"});
            choice(c.quadrature, {"9", "exact"});
            choice(c.stiffness, {"standard", "constant_preserving"});
            choice(c.timerLevel, {"coarse", "detailed"});
            choice(c.reuse, {"off", "operator", "full"});
            choice(c.initialGuess, {"zero", "previous"});
            choice(c.initialCondition, {"single", "alpine_product"});
            choice(c.preconditioner, {"jacobi", "newton", "chebyshev", "richardson",
                                      "richardson_alt", "gauss-seidel", "ssor"});
            for (auto count : list<long long>(c.get("cells", c.dimension == 1   ? "64"
                                                             : c.dimension == 2 ? "16"
                                                                                : "8"))) {
                if (count < 2 || count >= std::numeric_limits<int>::max())
                    throw std::runtime_error("Invalid cell count.");
                c.cells.push_back(static_cast<unsigned>(count));
            }
            c.lengths = list<double>(c.get("lengths", "12.566370614359172"));
            if (c.cells.size() == 1)
                c.cells.resize(c.dimension, c.cells[0]);
            if (c.lengths.size() == 1)
                c.lengths.resize(c.dimension, c.lengths[0]);
            if (c.cells.size() != c.dimension || c.lengths.size() != c.dimension)
                throw std::runtime_error("Axis lists must have one entry or one per dimension.");
            for (unsigned d = 0; d < c.dimension; ++d)
                if (c.cells[d] < 2 || !std::isfinite(c.lengths[d]) || c.lengths[d] <= 0)
                    throw std::runtime_error("Invalid mesh cell count or length.");
            return c;
        }
        ippl::ParameterList parameters(const std::string& method) const {
            ippl::ParameterList p;
            p.add("preconditioned", method != "plain");
            p.add("preconditioner_type", method == "plain" ? std::string("jacobi") : method);
            p.add("interpolation_nodes", family);
            p.add("quadrature_nodes", std::string("gauss_legendre"));
            p.add("poisson_stiffness_mode", stiffness);
            p.add("fem_reuse", reuse);
            p.add("fem_detailed_timing", timerLevel == "detailed");
            p.add("tolerance", tolerance);
            p.add("max_iterations", maxIterations);
            for (const auto& [name, defaultValue] :
                 std::map<std::string, std::string>{{"newton_level", "2"},
                                                    {"chebyshev_degree", "8"},
                                                    {"richardson_iterations", "4"},
                                                    {"gauss_seidel_inner_iterations", "2"},
                                                    {"gauss_seidel_outer_iterations", "2"}}) {
                size_t used     = 0;
                const auto text = get(name, defaultValue);
                const int value = std::stoi(text, &used);
                if (used != text.size() || value < 1)
                    throw std::runtime_error("Preconditioner counts must be positive.");
                p.add(name, value);
            }
            size_t used        = 0;
            const auto text    = get("ssor_omega", "1");
            const double omega = std::stod(text, &used);
            if (used != text.size() || !(omega > 0 && omega < 2))
                throw std::runtime_error("ssor_omega must be in (0,2).");
            p.add("ssor_omega", omega);
            return p;
        }
    };
}  // namespace electrostatic_fem
