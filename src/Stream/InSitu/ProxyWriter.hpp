// ProxyWriter.hpp - template definitions
#pragma once

#ifdef IPPL_PROXYWRITER_DECL

#include <algorithm>
#include <type_traits>

namespace ippl {

// Arithmetic scalar include
template <typename T>
inline void ProxyWriter::include(const T& defaultValue, const std::string& label) {
  static_assert(std::is_arithmetic_v<T>, "ProxyWriter::include requires a scalar arithmetic type");
  Channel ch; 
  ch.label = label; 
  // Parse label for array prefix and namespace; compute propertyName
  {
    ch.isArray = false;
    std::string work = label;
    if (work.rfind("array:", 0) == 0) { ch.isArray = true; work = work.substr(6); }
    ch.propertyName = work;
    auto dp = work.find('.');
    if (dp != std::string::npos) ch.ns = work.substr(0, dp); else ch.ns = work;
  }
  ch.defaultValue = static_cast<double>(defaultValue); ch.isVector = false; ch.vecDim = 1;
  // Mark integer scalars (exclude bool which is handled via includeBool)
  if constexpr (std::is_integral_v<T> && !std::is_same_v<std::decay_t<T>, bool>) {
    ch.isInteger = true;
  }
  // Heuristic: treat button-like labels as buttons (Int checkbox) even if numeric
  {
    auto ends_with = [](const std::string& s, const std::string& suf){ return s.size()>=suf.size() && s.compare(s.size()-suf.size(), suf.size(), suf)==0; };
    if (ends_with(ch.propertyName, ".reset_btn") || ends_with(ch.propertyName, "_btn") || ch.propertyName.find("btn") != std::string::npos) {
      ch.isButton = true;
      ch.isVector = false;
    }
    // Heuristic: detect booleans when users accidentally call include<T> for bool arrays
    // Common patterns: names starting with 'bool_' or members ending with '.switch'
    if (!ch.isButton) {
      bool looksBool = false;
      if (ch.propertyName.rfind("bool_", 0) == 0) looksBool = true; // starts with bool_
      if (ends_with(ch.propertyName, ".switch")) looksBool = true;   // struct member switch
      if (looksBool) {
        ch.isBool = true;
        ch.isInteger = false; // treat as boolean domain rather than integer slider
      }
    }
  }
  if (hasConfig_) {
    applyScalarConfig(ch);
  }
  channels_.emplace_back(std::move(ch));
}

// Vector include (up to 3 components exposed)
template <typename T, unsigned Dim_v>
inline void ProxyWriter::includeVector(const std::string& label) {
  Channel ch; 
  ch.label = label; 
  // Parse label for array prefix and namespace; compute propertyName
  {
    ch.isArray = false;
    std::string work = label;
    if (work.rfind("array:", 0) == 0) { ch.isArray = true; work = work.substr(6); }
    ch.propertyName = work;
    auto dp = work.find('.');
    if (dp != std::string::npos) ch.ns = work.substr(0, dp); else ch.ns = work;
  }
  ch.defaultValue = 1.0; ch.isVector = true; ch.vecDim = (Dim_v > 3 ? 3u : Dim_v);
  if (hasConfig_) {
    applyVectorConfig<Dim_v>(ch);
  }
  channels_.emplace_back(std::move(ch));
}

// Apply vector config (ranges/defaults)
template <unsigned Dim_v>
inline void ProxyWriter::applyVectorConfig(Channel& ch) const {
  double d0 = (ch.hasVectorRanges || ch.preserveDefaults) ? ch.vecDefaults[0]
            : (typeDefaultVectorComp_.has ? typeDefaultVectorComp_.def : ch.defaultValue);
  double d1 = (ch.hasVectorRanges || ch.preserveDefaults) ? ch.vecDefaults[1]
            : (typeDefaultVectorComp_.has ? typeDefaultVectorComp_.def : ch.defaultValue);
  double d2 = (ch.hasVectorRanges || ch.preserveDefaults) ? ch.vecDefaults[2]
            : (typeDefaultVectorComp_.has ? typeDefaultVectorComp_.def : ch.defaultValue);
  double vmin = typeDefaultVectorComp_.has ? typeDefaultVectorComp_.min : rangeMin_;
  double vmax = typeDefaultVectorComp_.has ? typeDefaultVectorComp_.max : rangeMax_;

  auto it = labelVectorCfg_.find(ch.label);
  if (it != labelVectorCfg_.end()) {
    const VectorCfg& vc = it->second;
    if (vc.uniform) {
      d0 = vc.udef; d1 = vc.udef; d2 = vc.udef;
      vmin = vc.umin; vmax = vc.umax;
    }
    if (vc.comp[0].has) { d0 = vc.comp[0].def; vmin = std::min(vmin, vc.comp[0].min); vmax = std::max(vmax, vc.comp[0].max); }
    if (vc.comp[1].has) { d1 = vc.comp[1].def; vmin = std::min(vmin, vc.comp[1].min); vmax = std::max(vmax, vc.comp[1].max); }
    if (vc.comp[2].has) { d2 = vc.comp[2].def; vmin = std::min(vmin, vc.comp[2].min); vmax = std::max(vmax, vc.comp[2].max); }
  }

  ch.hasVectorRanges = true;
  ch.vecDefaults[0] = d0;
  ch.vecDefaults[1] = d1;
  ch.vecDefaults[2] = d2;
  ch.vecMin = vmin;
  ch.vecMax = vmax;
}

} // namespace ippl
#endif // IPPL_PROXYWRITER_DECL







// inline void ProxyWriter::includeLinMap(const std::string& baseLabel, double timeDefault)
// {
//   // x_row, y_row, z_row as 3D vectors
//   {
//     Channel cx; cx.label = baseLabel + "_x_row"; cx.isVector = true; cx.vecDim = 3; cx.defaultValue = 1.0;
//     if (hasConfig_) applyVectorConfig<3>(cx);
//     channels_.emplace_back(std::move(cx));
//   }
//   {
//     Channel cy; cy.label = baseLabel + "_y_row"; cy.isVector = true; cy.vecDim = 3; cy.defaultValue = 1.0;
//     if (hasConfig_) applyVectorConfig<3>(cy);
//     channels_.emplace_back(std::move(cy));
//   }
//   {
//     Channel cz; cz.label = baseLabel + "_z_row"; cz.isVector = true; cz.vecDim = 3; cz.defaultValue = 1.0;
//     if (hasConfig_) applyVectorConfig<3>(cz);
//     channels_.emplace_back(std::move(cz));
//   }
//   // time scalar, displayed as textbox (no slider)
//   {
//     Channel ct; ct.label = baseLabel + "_time"; ct.isVector = false; ct.defaultValue = timeDefault; ct.scalarAsTextBox = true;
//     if (hasConfig_) applyScalarConfig(ct);
//     channels_.emplace_back(std::move(ct));
//   }
// }

// inline void ProxyWriter::includeLinMapWithDefaults(
//     const std::string& baseLabel,
//     const std::array<double,3>& xDefaults,
//     const std::array<double,3>& yDefaults,
//     const std::array<double,3>& zDefaults,
//     double timeDefault)
// {
//   // x_row
//   {
//     Channel cx; cx.label = baseLabel + "_x_row"; cx.isVector = true; cx.vecDim = 3; cx.defaultValue = 1.0;
//     // Seed component defaults from provided values
//     cx.hasVectorRanges = true;
//     cx.vecDefaults[0] = xDefaults[0]; cx.vecDefaults[1] = xDefaults[1]; cx.vecDefaults[2] = xDefaults[2];
//     cx.preserveDefaults = true;
//     // Use global ranges for initial domain unless overridden by config
//     cx.vecMin = rangeMin_; cx.vecMax = rangeMax_;
//     if (hasConfig_) applyVectorConfig<3>(cx); // allow config to override
//     channels_.emplace_back(std::move(cx));
//   }
//   // y_row
//   {
//     Channel cy; cy.label = baseLabel + "_y_row"; cy.isVector = true; cy.vecDim = 3; cy.defaultValue = 1.0;
//     cy.hasVectorRanges = true;
//     cy.vecDefaults[0] = yDefaults[0]; cy.vecDefaults[1] = yDefaults[1]; cy.vecDefaults[2] = yDefaults[2];
//     cy.preserveDefaults = true;
//     cy.vecMin = rangeMin_; cy.vecMax = rangeMax_;
//     if (hasConfig_) applyVectorConfig<3>(cy);
//     channels_.emplace_back(std::move(cy));
//   }
//   // z_row
//   {
//     Channel cz; cz.label = baseLabel + "_z_row"; cz.isVector = true; cz.vecDim = 3; cz.defaultValue = 1.0;
//     cz.hasVectorRanges = true;
//     cz.vecDefaults[0] = zDefaults[0]; cz.vecDefaults[1] = zDefaults[1]; cz.vecDefaults[2] = zDefaults[2];
//     cz.preserveDefaults = true;
//     cz.vecMin = rangeMin_; cz.vecMax = rangeMax_;
//     if (hasConfig_) applyVectorConfig<3>(cz);
//     channels_.emplace_back(std::move(cz));
//   }
//   // time scalar (textbox)
//   {
//     Channel ct; ct.label = baseLabel + "_time"; ct.isVector = false; ct.defaultValue = timeDefault; ct.scalarAsTextBox = true;
//     ct.preserveScalarDefault = true;
//     if (hasConfig_) applyScalarConfig(ct);
//     channels_.emplace_back(std::move(ct));
//   }
// }

