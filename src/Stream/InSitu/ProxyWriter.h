/**
 * @file ProxyWriter.h
 * @brief Declarations for generating ParaView Catalyst proxy XML for steerable parameters.
 */
// ============= ProxyWriter: Declarations and Doxygen documentation =============
// This header contains only declarations and small structs; templates are
// defined in ProxyWriter.hpp and non-templates in ProxyWriter.cpp

#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Conduit (for optional YAML-backed config of ranges/defaults)
#include <catalyst_conduit.hpp>

namespace ippl {
#define IPPL_PROXYWRITER_DECL 1

/**
 * Generates ParaView Catalyst proxy XML for steerable parameters.
 *
 * - Call one of initialize() overloads to set output path and optional config
 * - Register channels via include()/includeVector()/includeBool()/includeEnum()/includeButton()
 * - Generate the XML with produce() or produceUnified()
 *
 * YAML schema (implicit typing):
 * type_defaults:
 *   scalar: { min: <num>, max: <num>, default: <num> }
 *   vector:
 *     component_defaults: { min: <num>, max: <num>, default: <num> }
 * steer_params:
 *   Efield: { min: <num>, max: <num>, default: <num> }
 *   Bfield:
 *     components:
 *       x: { min: <num>, max: <num>, default: <num> }
 *       y: { ... }
 *       z: { ... }
 *   # Or uniform vector defaults/range for all components
 *   V: { min: <num>, max: <num>, default: <num> }
 */
class ProxyWriter {
public:
  // ------------------------------- Channel model --------------------------------
  struct Channel {
    std::string label;                 // human label and suffix
    // XML property name (for arrays, this strips the 'array:' prefix)
    std::string propertyName;          
    // Namespace/group (struct or array name); for 'a.b' it's 'a', for 'a' it's 'a'
    std::string ns;
    bool        isArray{false};        // true when label starts with "array:"
    double      defaultValue{1.0};     // scalar default or seed for vectors
    bool        isVector{false};
    bool        isBool{false};
    bool        isButton{false};
    bool        isEnum{false};
    bool        isInteger{false};     // true for integral types (excl. bool)
  bool        scalarAsTextBox{false}; // if true, render scalar as text box (no slider)
  bool        preserveDefaults{false}; // if true, keep provided vecDefaults over type defaults
  bool        preserveScalarDefault{false}; // if true, keep provided scalar default over type default
    std::vector<std::pair<std::string,int>> enumEntries{};
    int         defaultInt{0};
    unsigned    vecDim{1};             // 1..3 exposed components

    // Optional per-label overrides populated from YAML/Conduit config
    bool        hasScalarRange{false};
    double      scalarMin{0.0};
    double      scalarMax{1.0};
    bool        hasScalarDefault{false};

    bool        hasVectorRanges{false};
    std::array<double,3> vecDefaults{ {1.0,1.0,1.0} };
    double      vecMin{0.0};
    double      vecMax{1.0};
  };

  // ---------------------------- Public API (decls) ------------------------------

  // Initialize WITHOUT explicit config: attempts to load default YAML (proxys_default_config.yaml or proxy_default_config.yaml).
  // No ranges are emitted by default; ranges will only be included in the prototype if the config provides them.
  void initialize(std::filesystem::path xmlOutputPath,
                  std::string prototypeLabel);

  // Initialize WITH explicit config YAML path. If load fails, proceeds without ranges.
  void initialize(std::filesystem::path xmlOutputPath,
                  const std::string& configYamlPath,
                  std::string prototypeLabel);

  // Register steerable controls
  template <typename T>
  void include(const T& defaultValue, const std::string& label);

  template <typename T, unsigned Dim_v>
  void includeVector(const std::string& label);

  /**
   * @brief Register a boolean switch channel.
   * @param label UI label and XML property name.
   * @param defaultValue Initial value if not overridden by config.
   */
  void includeBool(const std::string& label, bool defaultValue = false);
  /**
   * @brief Register a momentary button channel (edge-triggered action).
   * @param label UI label and XML property name.
   */
  void includeButton(const std::string& label);
  /**
   * @brief Register an enum channel with named entries.
   * @param label UI label and XML property name.
   * @param entries Vector of (display name, integer value) pairs.
   * @param defaultValue Initial selected entry index or value, depending on consumer.
   */
  void includeEnum(const std::string& label,
                   const std::vector<std::pair<std::string,int>>& entries,
                   int defaultValue = 0);

  // Emit XML files
  /**
   * @brief Produce one XML file per registered channel.
   * @return true on success, false otherwise.
   */
  bool produce();
  /**
   * @brief Produce a single XML containing a unified source proxy encompassing all channels.
   * @param unifiedProxyName XML proxy name.
   * @param unifiedGroupLabel Group label in ParaView.
   * @return true on success, false otherwise.
   */
  bool produceUnified(const std::string& unifiedProxyName,
                      const std::string& unifiedGroupLabel);

   // Record desired initial tuple count for a given array namespace (e.g., "LinMaps")
   void setArrayInitialSize(const std::string& ns, std::size_t n) { arrayInitialSize_[ns] = static_cast<unsigned>(n); }
private:
  // Internal XML builders and helpers (implemented in ProxyWriter.cpp)
  void resetStreams();
  void appendSourceProxy(const Channel& ch);
  void appendPrototype();
  void appendUnifiedSourceProxy(const std::string& proxyName,
                                const std::string& groupLabel);
  // Build one per-array source proxy for channels sharing the same namespace
  void appendArraySourceProxy(const std::string& ns,
                              const std::vector<const Channel*>& chans);


  void setConfigNode(const conduit_cpp::Node n);
  bool loadConfigFromYamlFile(const std::string& path);
  bool loadConfigFromYamlString(const std::string& yaml);
  void applyScalarConfig(Channel& ch) const;
  template<unsigned Dim_v>
  void applyVectorConfig(Channel& ch) const;

  // Parsed config cache (populated by YAML loader or setConfigNode)
  struct ScalarCfg { bool has=false; double min=0, max=0, def=0; };
  struct VectorCompCfg { bool has=false; double min=0, max=0, def=0; };
  struct VectorCfg {
    bool has=false; // at least one component or uniform present
    bool uniform=false; double umin=0, umax=0, udef=0;
    VectorCompCfg comp[3];
  };

  // ------------------------------ Data members ---------------------------------
  std::filesystem::path outPath_{};
  // Global range defaults are no longer used for emission; kept only for backward compatibility if needed.
  double rangeMin_{0};
  double rangeMax_{0};
  std::string prototypeLabel_{"SteerableParameters"};

  std::vector<Channel> channels_{};
  std::ostringstream header_{};
  std::ostringstream sources_{};
  std::ostringstream misc_{};
  std::ostringstream footer_{};

  bool hasConfig_{false};
  ScalarCfg typeDefaultScalar_{};
  VectorCompCfg typeDefaultVectorComp_{};
  std::map<std::string, ScalarCfg> labelScalarCfg_{};
  std::map<std::string, VectorCfg> labelVectorCfg_{};

  // Initial lengths for array namespaces; used to pre-populate default_values count
  std::map<std::string, unsigned> arrayInitialSize_{};
};
} // namespace ippl

// Provide template definitions (keep at global scope to avoid injecting std headers into namespace ippl)
#include "ProxyWriter.hpp"

