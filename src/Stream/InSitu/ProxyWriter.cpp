#include "ProxyWriter.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <limits>
#include <cctype>
#include <algorithm>

namespace {
inline std::string ltrim(std::string s) {
  size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; return s.substr(i);
}
inline std::string rtrim(std::string s) {
  size_t i = s.size(); while (i > 0 && std::isspace(static_cast<unsigned char>(s[i-1]))) --i; s.resize(i); return s;
}
inline std::string trim(std::string s) { return rtrim(ltrim(std::move(s))); }
inline bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

inline std::string strip_comment(const std::string& s) {
  // Very naive: drop everything after an unbraced '#'
  // We'll ignore '#' if it appears inside { } to keep inline maps intact
  int brace = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '{') ++brace; else if (s[i] == '}') brace = std::max(0, brace-1);
    else if (s[i] == '#' && brace == 0) return s.substr(0, i);
  }
  return s;
}

inline int leading_spaces(const std::string& s) {
  int n = 0; while (n < (int)s.size() && s[n] == ' ') ++n; return n;
}

inline std::string unquote(std::string s) {
  s = trim(std::move(s));
  if (!s.empty() && (s.front() == '"' || s.front() == '\'')) s.erase(s.begin());
  if (!s.empty() && (s.back() == '"' || s.back() == '\'')) s.pop_back();
  return s;
}

inline bool extract_number(const std::string& src, const std::string& key, double& out) {
  // find "key:" and parse following number until a delimiter (',' or '}' or end)
  const std::string pat = key + ":";
  auto pos = src.find(pat);
  if (pos == std::string::npos) return false;
  pos += pat.size();
  // skip spaces
  while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
  // capture sign and digits
  size_t end = pos;
  bool dotSeen = false; bool expSeen = false;
  if (end < src.size() && (src[end] == '+' || src[end] == '-')) ++end;
  while (end < src.size()) {
    char c = src[end];
    if (std::isdigit(static_cast<unsigned char>(c))) { ++end; continue; }
    if (c == '.' && !dotSeen) { dotSeen = true; ++end; continue; }
    if ((c == 'e' || c == 'E') && !expSeen) { expSeen = true; ++end; if (end < src.size() && (src[end] == '+' || src[end] == '-')) ++end; continue; }
    break;
  }
  if (end == pos) return false;
  try {
    out = std::stod(src.substr(pos, end - pos));
    return true;
  } catch (...) { return false; }
}

} // anonymous namespace

namespace ippl {

// -------------------------------- Initialization ---------------------------------

void ProxyWriter::initialize(std::filesystem::path xmlOutputPath,
                             std::string prototypeLabel) {
  outPath_ = std::move(xmlOutputPath);
  prototypeLabel_ = std::move(prototypeLabel);
  resetStreams();
  // Attempt to load default config from the output folder.
  std::filesystem::path baseDir = outPath_.parent_path();
  std::filesystem::path cfg1 = baseDir / "proxys_default_config.yaml";
  std::filesystem::path cfg2 = baseDir / "proxy_default_config.yaml";
  if (std::filesystem::exists(cfg1)) {
    loadConfigFromYamlFile(cfg1.string());
  } else if (std::filesystem::exists(cfg2)) {
    loadConfigFromYamlFile(cfg2.string());
  }
}

void ProxyWriter::initialize(std::filesystem::path xmlOutputPath,
                             const std::string& configYamlPath,
                             std::string prototypeLabel) {
  outPath_ = std::move(xmlOutputPath);
  prototypeLabel_ = std::move(prototypeLabel);
  resetStreams();
  if (!configYamlPath.empty()) {
    loadConfigFromYamlFile(configYamlPath);
  }
}

// Convenience overload: only output path and YAML config path.
// Uses existing defaults for rangeMin_/rangeMax_ and prototypeLabel_.
// removed legacy overload

// removed legacy overload taking conduit node

// --------------------------------- Inclusions ------------------------------------

void ProxyWriter::includeBool(const std::string& label, bool defaultValue) {
  Channel ch; 
  ch.label = label; 
  ch.isArray = false; 
  {
    std::string work = label;
    if (work.rfind("array:", 0) == 0) { ch.isArray = true; work = work.substr(6); }
    ch.propertyName = work;
    auto dp = work.find('.');
    if (dp != std::string::npos) ch.ns = work.substr(0, dp); else ch.ns = work;
  }
  ch.defaultValue = defaultValue ? 1 : 0; ch.isVector = false; ch.isBool = true; ch.vecDim = 1;
  channels_.emplace_back(std::move(ch));
}

void ProxyWriter::includeButton(const std::string& label) {
  Channel ch; 
  ch.label = label; 
  ch.isArray = false; 
  {
    std::string work = label;
    if (work.rfind("array:", 0) == 0) { ch.isArray = true; work = work.substr(6); }
    ch.propertyName = work;
    auto dp = work.find('.');
    if (dp != std::string::npos) ch.ns = work.substr(0, dp); else ch.ns = work;
  }
  ch.defaultValue = 0.0; ch.isVector = false; ch.isBool = false; ch.isButton = true; ch.vecDim = 1;
  channels_.emplace_back(std::move(ch));
}

void ProxyWriter::includeEnum(const std::string& label,
                              const std::vector<std::pair<std::string,int>>& entries,
                              int defaultValue) {
  Channel ch; 
  ch.label = label; 
  ch.isArray = false; 
  {
    std::string work = label;
    if (work.rfind("array:", 0) == 0) { ch.isArray = true; work = work.substr(6); }
    ch.propertyName = work;
    auto dp = work.find('.');
    if (dp != std::string::npos) ch.ns = work.substr(0, dp); else ch.ns = work;
  }
  ch.isEnum = true; ch.defaultInt = defaultValue; ch.vecDim = 1;
  ch.enumEntries = entries;
  channels_.emplace_back(std::move(ch));
}

// ------------------------------- Produce (XML) -----------------------------------

bool ProxyWriter::produce() {
  resetStreams();

  // Header
  header_ << "<ServerManagerConfiguration>\n\n";

  // Sources group with one SourceProxy per channel
  sources_ << "    <ProxyGroup name='sources'>\n";
  for (const auto& ch : channels_) {
    appendSourceProxy(ch);
  }
  sources_ << "    </ProxyGroup>\n\n";

  // Misc group with a single prototype used by PropertyCollection
  misc_ << "    <ProxyGroup name='misc'>\n";
  appendPrototype();
  misc_ << "    </ProxyGroup>\n";

  footer_ << "</ServerManagerConfiguration>\n";

  // Stitch everything into a single buffer
  std::ostringstream full;
  full << header_.str() << sources_.str() << misc_.str() << footer_.str();

  // Ensure directory exists and write file
  std::error_code ec;
  std::filesystem::create_directories(outPath_.parent_path(), ec);
  std::ofstream ofs(outPath_);
  if (!ofs) return false;
  ofs << full.str();
  ofs.close();
  return ofs.good();
}

bool ProxyWriter::produceUnified(const std::string& unifiedProxyName,
                                 const std::string& unifiedGroupLabel) {
  resetStreams();

  // Header
  header_ << "<ServerManagerConfiguration>\n\n";

  sources_ << "    <ProxyGroup name='sources'>\n";
  // Split channels into singles and arrays grouped by namespace
  std::map<std::string, std::vector<const Channel*>> arraysByNs;
  for (const auto& ch : channels_) {
    if (!ch.isArray) continue;
    arraysByNs[ch.ns].push_back(&ch);
  }

  // Unified SCALARS (non-array)
  appendUnifiedSourceProxy(unifiedProxyName, unifiedGroupLabel);

  // Per-array sources
  for (const auto& kv : arraysByNs) {
    appendArraySourceProxy(kv.first, kv.second);
  }
  sources_ << "    </ProxyGroup>\n\n";

  misc_ << "    <ProxyGroup name='misc'>\n";
  appendPrototype();
  misc_ << "    </ProxyGroup>\n";

  footer_ << "</ServerManagerConfiguration>\n";

  // Compose full XML
  std::ostringstream full;
  full << header_.str() << sources_.str() << misc_.str() << footer_.str();

  std::error_code ec;
  std::filesystem::create_directories(outPath_.parent_path(), ec);
  std::ofstream ofs(outPath_);
  if (!ofs) return false;
  ofs << full.str();
  ofs.close();
  return ofs.good();
}

// ---------------------------- Internal XML builders ------------------------------

void ProxyWriter::resetStreams() {
  header_.str(""); header_.clear();
  sources_.str(""); sources_.clear();
  misc_.str(""); misc_.clear();
  footer_.str(""); footer_.clear();
}

void ProxyWriter::appendSourceProxy(const Channel& ch) {
  const std::string& L = ch.label;
  sources_ << "        <SourceProxy class='vtkSteeringDataGenerator' name='SteerableParameters_" << L << "'>\n"
           << "            <IntVectorProperty name='PartitionType' command='SetPartitionType' number_of_elements='1' default_values='1' panel_visibility='never'>\n"
           << "            </IntVectorProperty>\n\n"
           << "            <IntVectorProperty name='FieldAssociation' command='SetFieldAssociation' number_of_elements='1' default_values='0' panel_visibility='never'>\n"
           << "            </IntVectorProperty>\n\n"
           << "            <DoubleVectorProperty name='scaleFactor'\n"
           << "                                  command='SetTuple1Double'\n"
           << "                                  clean_command='Clear'\n"
           << "                                  use_index='1'\n"
           << "                                  initial_string='steerable_field_b_" << L << "'\n"
           << "                                  number_of_elements_per_command='1'\n"
           << "                                  repeat_command='1'\n"
           << "                                  panel_widget='DoubleRange'>\n";
  // Per requirements: no DoubleRangeDomain in the SOURCE section
  sources_ << "            </DoubleVectorProperty>\n\n";
  if(!ch.isBool){
    sources_ << "            <PropertyGroup label='SteerableParameters' panel_widget='PropertyCollection'>\n";
  }
  sources_ << "                <Hints>\n"
           << "                  <PropertyCollectionWidgetPrototype group='misc' name='SteerableParametersPrototype' />\n"
           << "                </Hints>\n"
           << "            </PropertyGroup>\n\n"
           << "            <Hints>\n"
           << "              <CatalystInitializePropertiesWithMesh mesh='steerable_channel_0D_mesh'>\n"
           << "                <Property name='scaleFactor' association='point' array='steerable_field_f_" << L << "' />\n"
           << "              </CatalystInitializePropertiesWithMesh>\n"
           << "            </Hints>\n"
           << "        </SourceProxy>\n\n";
}

void ProxyWriter::appendPrototype() {
  // Prototype for numeric steerables (scalars and vectors)
  misc_ << "      <Proxy name='SteerableNumericsPrototype' label=' Numerics-Collective-Prototype (do not cancel [x] or add new [+]!!)'> \n";
  for (const auto& ch : channels_) {
    if (ch.isBool || ch.isButton || ch.isEnum) continue;
    if (ch.isArray) continue;
    if (ch.propertyName.find('.') != std::string::npos) continue;
    if (ch.isVector) {
      const double d0 = ch.hasVectorRanges ? ch.vecDefaults[0] : ch.defaultValue;
      const double d1 = ch.hasVectorRanges ? ch.vecDefaults[1] : ch.defaultValue;
      const double d2 = ch.hasVectorRanges ? ch.vecDefaults[2] : ch.defaultValue;
      unsigned dim = ch.vecDim;
      std::string def;
      if (ch.isInteger) {
        int i0 = static_cast<int>(d0);
        int i1 = static_cast<int>(d1);
        int i2 = static_cast<int>(d2);
        if (dim == 1) def = std::to_string(i0);
        else if (dim == 2) def = (std::to_string(i0) + " " + std::to_string(i1));
        else def = (std::to_string(i0) + " " + std::to_string(i1) + " " + std::to_string(i2));
        misc_ << "        <IntVectorProperty name='vec" << dim << "_" << ch.label << "' label='" << ch.label << "' number_of_elements='" << dim << "' default_values='" << def << "'>\n";
        // Include IntRangeDomain only if config defines ranges for this label
        auto itIV = labelVectorCfg_.find(ch.label);
        if (itIV != labelVectorCfg_.end() && itIV->second.has) {
          const VectorCfg& vc = itIV->second;
          int mn = static_cast<int>(vc.uniform ? vc.umin : vc.comp[0].min);
          int mx = static_cast<int>(vc.uniform ? vc.umax : vc.comp[0].max);
          misc_ << "          <IntRangeDomain name='range' min='" << mn << "' max='" << mx << "'/>\n";
        }
        misc_ << "        </IntVectorProperty>\n";
      } else {
        if (dim == 1) def = std::to_string(d0);
        else if (dim == 2) def = (std::to_string(d0) + " " + std::to_string(d1));
        else def = (std::to_string(d0) + " " + std::to_string(d1) + " " + std::to_string(d2));
        misc_ << "        <DoubleVectorProperty name='vec" << dim << "_" << ch.label << "' label='" << ch.label << "' number_of_elements='" << dim << "' default_values='" << def << "'>\n";
        // Prefer vector ranges; fallback to scalar ranges if present for this label
        double vminOut; double vmaxOut; bool haveRange = false;
        auto itV = labelVectorCfg_.find(ch.label);
        if (itV != labelVectorCfg_.end() && itV->second.has) {
          const VectorCfg& vc = itV->second;
          if (vc.uniform) { vminOut = vc.umin; vmaxOut = vc.umax; haveRange = true; }
          else { for (int i=0;i<3;i++) if (vc.comp[i].has) { vminOut = vc.comp[i].min; vmaxOut = vc.comp[i].max; haveRange = true; break; } }
        } else {
          auto itS = labelScalarCfg_.find(ch.label);
          if (itS != labelScalarCfg_.end() && itS->second.has) { vminOut = itS->second.min; vmaxOut = itS->second.max; haveRange = true; }
        }
        if (haveRange) {
          misc_ << "          <DoubleRangeDomain name='range' min='" << vminOut << "' max='" << vmaxOut << "'/>\n";
        }
        misc_ << "        </DoubleVectorProperty>\n";
      }
      if (!ch.isInteger && dim == 3) {
        misc_ << "          <Hints>\n";
        misc_ << "            <ShowComponentLabels>\n";
        misc_ << "              <ComponentLabel component='0' label='X'/>\n";
        misc_ << "              <ComponentLabel component='1' label='Y'/>\n";
        misc_ << "              <ComponentLabel component='2' label='Z'/>\n";
        misc_ << "            </ShowComponentLabels>\n";
        misc_ << "          </Hints>\n";
      }
    } else {
      const double sdef = ch.defaultValue;
      if (ch.isInteger) {
        misc_ << "        <IntVectorProperty name='scaleFactor_" << ch.label << "' label='" << ch.label << "' number_of_elements='1' default_values='" << static_cast<int>(sdef) << "'>\n";
        // Include IntRangeDomain only if config defines ranges for this label
        auto itIS = labelScalarCfg_.find(ch.label);
        if (itIS != labelScalarCfg_.end() && itIS->second.has) {
          misc_ << "          <IntRangeDomain name='range' min='" << static_cast<int>(itIS->second.min) << "' max='" << static_cast<int>(itIS->second.max) << "'/>\n";
        }
        misc_ << "        </IntVectorProperty>\n";
      } else {
        misc_ << "        <DoubleVectorProperty name='scaleFactor_" << ch.label << "' label='" << ch.label << "' number_of_elements='1' default_values='" << sdef << "'>\n";
        // Include DoubleRangeDomain only if config defines ranges for this specific label
        auto itS = labelScalarCfg_.find(ch.label);
        if (itS != labelScalarCfg_.end() && itS->second.has) {
          misc_ << "          <DoubleRangeDomain name='range' min='" << itS->second.min << "' max='" << itS->second.max << "'/>\n";
        }
        misc_ << "        </DoubleVectorProperty>\n";
      }
    }
  }
  misc_ << "      </Proxy>\n\n";

  // Prototype for enum steerables
  misc_ << "      <Proxy name='SteerableEnumsPrototype' label='Enums-Collective-Prototype (do not cancel [x] or add new [+]!!)'>\n";
  for (const auto& ch : channels_) {
    if (!ch.isEnum || ch.enumEntries.empty()) continue;
    if (ch.propertyName.find('.') != std::string::npos) continue;
  misc_ << "        <IntVectorProperty name='PrototypeEnum_" << ch.label << "' label='" << ch.label << "' number_of_elements='1' default_values='" << ch.defaultInt << "' immediate_apply='1'>\n";
  misc_ << "          <EnumerationDomain name='enum'>\n";
    for (const auto& [text, val] : ch.enumEntries) {
  misc_ << "            <Entry text='" << text << "' value='" << val << "'/>\n";
    }
    misc_ << "          </EnumerationDomain>\n";
    misc_ << "        </IntVectorProperty>\n";
  }
  misc_ << "      </Proxy>\n";

  // (Removed legacy LinMap prototype; LinMap-like labels are now handled by generic prototypes.)

  // Prototypes per struct namespace for single (non-array) members (numeric / bool / enum / button)
  // For each ns where channels contain 'ns.member' and not isArray, create a prototype named '<ns>Prototype'
  {
    std::map<std::string, std::vector<const Channel*>> structMembers;
    for (const auto& ch : channels_) {
      if (ch.isArray) continue; // singles only
      // include numerics, bools, enums, and buttons
      // detect 'ns.member'
      auto pos = ch.propertyName.find('.');
      if (pos == std::string::npos) continue;
      structMembers[ch.ns].push_back(&ch);
    }
    for (const auto& kv : structMembers) {
      const std::string& ns = kv.first;
      const auto& mems = kv.second;
      misc_ << "      <Proxy name='" << ns << "Prototype' label='" << ns << "'>\n";
      for (const Channel* c : mems) {
        // member name after dot
        std::string member = c->propertyName.substr(c->propertyName.find('.')+1);
        if (c->isButton || c->isBool) {
          // Buttons and bools as checkbox
          misc_ << "        <IntVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='1' default_values='" << (c->defaultValue != 0.0 ? 1 : 0) << "'>\n";
          misc_ << "          <BooleanDomain name='bool'/>\n";
          misc_ << "        </IntVectorProperty>\n";
        } else if (c->isEnum) {
          misc_ << "        <IntVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='1' default_values='" << c->defaultInt << "'>\n";
          misc_ << "          <EnumerationDomain name='enum'>\n";
          for (const auto& ev : c->enumEntries) {
            misc_ << "            <Entry text='" << ev.first << "' value='" << ev.second << "'/>\n";
          }
          misc_ << "          </EnumerationDomain>\n";
          misc_ << "        </IntVectorProperty>\n";
        } else if (c->isVector) {
          const double d0 = c->hasVectorRanges ? c->vecDefaults[0] : c->defaultValue;
          const double d1 = c->hasVectorRanges ? c->vecDefaults[1] : c->defaultValue;
          const double d2 = c->hasVectorRanges ? c->vecDefaults[2] : c->defaultValue;
          unsigned dim = c->vecDim;
          std::string def = (dim == 1 ? std::to_string(d0) : dim == 2 ? (std::to_string(d0) + " " + std::to_string(d1)) : (std::to_string(d0) + " " + std::to_string(d1) + " " + std::to_string(d2)));
          if (c->isInteger) {
            misc_ << "        <IntVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='" << dim << "' default_values='" 
                  << def << "'>\n";
            // Add IntRangeDomain only if config provides a range for this label
            auto itIV = labelVectorCfg_.find(c->label);
            if (itIV != labelVectorCfg_.end() && itIV->second.has) {
              const VectorCfg& vc = itIV->second;
              int mn = static_cast<int>(vc.uniform ? vc.umin : vc.comp[0].min);
              int mx = static_cast<int>(vc.uniform ? vc.umax : vc.comp[0].max);
              misc_ << "          <IntRangeDomain name='range' min='" << mn << "' max='" << mx << "'/>\n";
            }
            misc_ << "        </IntVectorProperty>\n";
          } else {
            misc_ << "        <DoubleVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='" << dim << "' default_values='" 
                  << def << "'>\n";
            // Only include range if provided in config
            auto itV = labelVectorCfg_.find(c->label);
            if (itV != labelVectorCfg_.end() && itV->second.has) {
              double vmin = itV->second.uniform ? itV->second.umin : itV->second.comp[0].min;
              double vmax = itV->second.uniform ? itV->second.umax : itV->second.comp[0].max;
              misc_ << "          <DoubleRangeDomain name='range' min='" << vmin << "' max='" << vmax << "'/>\n";
            }
            misc_ << "        </DoubleVectorProperty>\n";
          }
        } else {
          const double sdef = c->defaultValue;
          if (c->isInteger) {
            misc_ << "        <IntVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='1' default_values='" 
                  << static_cast<int>(sdef) << "'>\n";
            // Only include range if provided in config
            auto itIS = labelScalarCfg_.find(c->label);
            if (itIS != labelScalarCfg_.end() && itIS->second.has) {
              misc_ << "          <IntRangeDomain name='range' min='" << static_cast<int>(itIS->second.min) << "' max='" << static_cast<int>(itIS->second.max) << "'/>\n";
            }
            misc_ << "        </IntVectorProperty>\n";
          } else {
        misc_ << "        <DoubleVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='1' default_values='" 
          << sdef << "'>\n";
        auto itS = labelScalarCfg_.find(c->label);
        if (itS != labelScalarCfg_.end() && itS->second.has) {
      misc_ << "          <DoubleRangeDomain name='range' min='" << itS->second.min << "' max='" << itS->second.max << "'/>\n";
        }
            misc_ << "        </DoubleVectorProperty>\n";
          }
        }
      }
      misc_ << "      </Proxy>\n";
    }
  }

  // Prototypes per ARRAY namespace (for entries with labels starting with 'array:')
  // Create a prototype named '<ns>Prototype' that exposes members as 'ns:member'.
  {
    std::map<std::string, std::vector<const Channel*>> arrayMembers;
    for (const auto& ch : channels_) {
      if (!ch.isArray) continue;
      // Include numerics, bools, enums, and buttons in array prototypes
      auto pos = ch.propertyName.find('.');
      if (pos == std::string::npos) continue; // skip if no member part
      arrayMembers[ch.ns].push_back(&ch);
    }
    for (const auto& kv : arrayMembers) {
      const std::string& ns = kv.first;
      const auto& mems = kv.second;
      misc_ << "      <Proxy name='" << ns << "Prototype' label='" << ns << "'>\n";
      for (const Channel* c : mems) {
        std::string member = c->propertyName.substr(c->propertyName.find('.')+1);
        if (c->isButton || c->isBool) {
          misc_ << "        <IntVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='1' default_values='" << (c->defaultValue != 0.0 ? 1 : 0) << "'>\n";
          misc_ << "          <BooleanDomain name='bool'/>\n";
          misc_ << "        </IntVectorProperty>\n";
        } else if (c->isEnum) {
          misc_ << "        <IntVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='1' default_values='" << c->defaultInt << "'>\n";
          misc_ << "          <EnumerationDomain name='enum'>\n";
          for (const auto& ev : c->enumEntries) {
            misc_ << "            <Entry text='" << ev.first << "' value='" << ev.second << "'/>\n";
          }
          misc_ << "          </EnumerationDomain>\n";
          misc_ << "        </IntVectorProperty>\n";
        } else if (c->isVector) {
          const double d0 = c->hasVectorRanges ? c->vecDefaults[0] : c->defaultValue;
          const double d1 = c->hasVectorRanges ? c->vecDefaults[1] : c->defaultValue;
          const double d2 = c->hasVectorRanges ? c->vecDefaults[2] : c->defaultValue;
          unsigned dim = c->vecDim;
          std::string def;
          if (c->isInteger) {
            int i0 = static_cast<int>(d0);
            int i1 = static_cast<int>(d1);
            int i2 = static_cast<int>(d2);
            if (dim == 1) def = std::to_string(i0);
            else if (dim == 2) def = (std::to_string(i0) + " " + std::to_string(i1));
            else def = (std::to_string(i0) + " " + std::to_string(i1) + " " + std::to_string(i2));
            misc_ << "        <IntVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='" << dim << "' default_values='" << def << "'>\n";
            // Add IntRangeDomain only if config provides a range for this label
            auto itIV = labelVectorCfg_.find(c->label);
            if (itIV != labelVectorCfg_.end() && itIV->second.has) {
              const VectorCfg& vc = itIV->second;
              int mn = static_cast<int>(vc.uniform ? vc.umin : vc.comp[0].min);
              int mx = static_cast<int>(vc.uniform ? vc.umax : vc.comp[0].max);
              misc_ << "          <IntRangeDomain name='range' min='" << mn << "' max='" << mx << "'/>\n";
            }
            misc_ << "        </IntVectorProperty>\n";
          } else {
            if (dim == 1) def = std::to_string(d0);
            else if (dim == 2) def = (std::to_string(d0) + " " + std::to_string(d1));
            else def = (std::to_string(d0) + " " + std::to_string(d1) + " " + std::to_string(d2));
            misc_ << "        <DoubleVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='" << dim << "' default_values='" << def << "'>\n";
            // Only include range if provided in config
            auto itV = labelVectorCfg_.find(c->label);
            if (itV != labelVectorCfg_.end() && itV->second.has) {
              double vmin = itV->second.uniform ? itV->second.umin : itV->second.comp[0].min;
              double vmax = itV->second.uniform ? itV->second.umax : itV->second.comp[0].max;
              misc_ << "          <DoubleRangeDomain name='range' min='" << vmin << "' max='" << vmax << "'/>\n";
            }
            misc_ << "        </DoubleVectorProperty>\n";
          }
        } else {
          const double sdef = c->defaultValue;
          if (c->isInteger) {
            misc_ << "        <IntVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='1' default_values='" << static_cast<int>(sdef) << "'>\n";
            auto itIS = labelScalarCfg_.find(c->label);
            if (itIS != labelScalarCfg_.end() && itIS->second.has) {
              misc_ << "          <IntRangeDomain name='range' min='" << static_cast<int>(itIS->second.min) << "' max='" << static_cast<int>(itIS->second.max) << "'/>\n";
            }
            misc_ << "        </IntVectorProperty>\n";
          } else {
            misc_ << "        <DoubleVectorProperty name='" << ns << ":" << member << "' label='" << member << "' number_of_elements='1' default_values='" << sdef << "'>\n";
            auto itS = labelScalarCfg_.find(c->label);
            if (itS != labelScalarCfg_.end() && itS->second.has) {
              misc_ << "          <DoubleRangeDomain name='range' min='" << itS->second.min << "' max='" << itS->second.max << "'/>\n";
            }
            misc_ << "        </DoubleVectorProperty>\n";
          }
        }
      }
      misc_ << "      </Proxy>\n";
    }
  }

  // Prototypes for ARRAY channels that are basic types (no struct member part)
  // Create one prototype per basic array namespace (ns == propertyName for these),
  // e.g., a prototype 'double_arrayPrototype' containing a single property 'double_array'.
  {
    std::map<std::string, const Channel*> basicByNs;
    for (const auto& ch : channels_) {
      if (!ch.isArray) continue;
      if (ch.propertyName.find('.') != std::string::npos) continue; // struct members handled above
      // For basic arrays, ns equals propertyName
      basicByNs[ch.ns] = &ch;
    }
    for (const auto& kv : basicByNs) {
      const std::string& ns = kv.first;
      const Channel* c = kv.second;
      misc_ << "      <Proxy name='" << ns << "Prototype' label='" << ns << "'>\n";
      const std::string& name = ns; // property name inside prototype
      if (c->isButton || c->isBool) {
        misc_ << "        <IntVectorProperty name='" << name << "' label='" << name << "' number_of_elements='1' default_values='" << (c->defaultValue != 0.0 ? 1 : 0) << "'>\n";
        misc_ << "          <BooleanDomain name='bool'/>\n";
        misc_ << "        </IntVectorProperty>\n";
      } else if (c->isEnum) {
        misc_ << "        <IntVectorProperty name='" << name << "' label='" << name << "' number_of_elements='1' default_values='" << c->defaultInt << "'>\n";
        misc_ << "          <EnumerationDomain name='enum'>\n";
        for (const auto& ev : c->enumEntries) {
          misc_ << "            <Entry text='" << ev.first << "' value='" << ev.second << "'/>\n";
        }
        misc_ << "          </EnumerationDomain>\n";
        misc_ << "        </IntVectorProperty>\n";
    } else if (c->isVector) {
      const double d0 = c->hasVectorRanges ? c->vecDefaults[0] : c->defaultValue;
      const double d1 = c->hasVectorRanges ? c->vecDefaults[1] : c->defaultValue;
      const double d2 = c->hasVectorRanges ? c->vecDefaults[2] : c->defaultValue;
      unsigned dim = c->vecDim;
      std::string def;
      if (c->isInteger) {
        int i0 = static_cast<int>(d0);
        int i1 = static_cast<int>(d1);
        int i2 = static_cast<int>(d2);
        if (dim == 1) def = std::to_string(i0);
        else if (dim == 2) def = (std::to_string(i0) + " " + std::to_string(i1));
        else def = (std::to_string(i0) + " " + std::to_string(i1) + " " + std::to_string(i2));
        misc_ << "        <IntVectorProperty name='" << name << "' label='" << name << "' number_of_elements='" << dim << "' default_values='" << def << "'>\n";
        // Include IntRangeDomain only if config provides one for this array label
        auto itIV = labelVectorCfg_.find(name);
        if (itIV != labelVectorCfg_.end() && itIV->second.has) {
          const VectorCfg& vc = itIV->second;
          int mn = static_cast<int>(vc.uniform ? vc.umin : vc.comp[0].min);
          int mx = static_cast<int>(vc.uniform ? vc.umax : vc.comp[0].max);
          misc_ << "          <IntRangeDomain name='range' min='" << mn << "' max='" << mx << "'/>\n";
        }
        misc_ << "        </IntVectorProperty>\n";
      } else {
        if (dim == 1) def = std::to_string(d0);
        else if (dim == 2) def = (std::to_string(d0) + " " + std::to_string(d1));
        else def = (std::to_string(d0) + " " + std::to_string(d1) + " " + std::to_string(d2));
        misc_ << "        <DoubleVectorProperty name='" << name << "' label='" << name << "' number_of_elements='" << dim << "' default_values='" << def << "'>\n";
        // Include a range ONLY if the config provides one for this label
        auto itV = labelVectorCfg_.find(name);
        if (itV != labelVectorCfg_.end() && itV->second.has) {
          double vmin = itV->second.uniform ? itV->second.umin : itV->second.comp[0].min;
          double vmax = itV->second.uniform ? itV->second.umax : itV->second.comp[0].max;
          misc_ << "          <DoubleRangeDomain name='range' min='" << vmin << "' max='" << vmax << "'/>\n";
        }
        misc_ << "        </DoubleVectorProperty>\n";
      }
        } else {
          const double sdef = c->defaultValue;
          if (c->isInteger) {
            misc_ << "        <IntVectorProperty name='" << name << "' label='" << name << "' number_of_elements='1' default_values='" << static_cast<int>(sdef) << "'>\n";
            // Include IntRangeDomain only if the config provides one for this array label
            auto itIS = labelScalarCfg_.find(name);
            if (itIS != labelScalarCfg_.end() && itIS->second.has) {
              misc_ << "          <IntRangeDomain name='range' min='" << static_cast<int>(itIS->second.min) << "' max='" << static_cast<int>(itIS->second.max) << "'/>\n";
            }
            misc_ << "        </IntVectorProperty>\n";
          } else {
            misc_ << "        <DoubleVectorProperty name='" << name << "' label='" << name << "' number_of_elements='1' default_values='" << sdef << "'>\n";
            // Include a range ONLY if the config provides one for this label
            auto itS = labelScalarCfg_.find(name);
            if (itS != labelScalarCfg_.end() && itS->second.has) {
              misc_ << "          <DoubleRangeDomain name='range' min='" << itS->second.min << "' max='" << itS->second.max << "'/>\n";
            }
            misc_ << "        </DoubleVectorProperty>\n";
          }
        }
      misc_ << "      </Proxy>\n";
    }
  }
}

void ProxyWriter::appendUnifiedSourceProxy(const std::string& proxyName,
                                           const std::string& groupLabel) {
  (void)groupLabel; // currently unused in XML
  sources_ << "        <SourceProxy class='vtkSteeringDataGenerator' name='" << proxyName << "'>\n"
           << "            <IntVectorProperty name='PartitionType' command='SetPartitionType' number_of_elements='1' default_values='1' panel_visibility='never'>\n"
           << "            </IntVectorProperty>\n\n"
           << "            <IntVectorProperty name='FieldAssociation' command='SetFieldAssociation' number_of_elements='1' default_values='0' panel_visibility='never'>\n"
           << "            </IntVectorProperty>\n\n";

  // Add properties per non-array channel
  for (const auto& ch : channels_) {
    if (ch.isArray) continue;
    const std::string& L = ch.label;
    const std::string& P = ch.propertyName.empty() ? ch.label : ch.propertyName;
    if (ch.isBool) {
  sources_ << "            <IntVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='SetTuple1Int'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
              //  << "                                  number_of_elements='1'\n"
               << "                                  default_values='" << (ch.defaultValue != 0.0 ? 1 : 0) << "'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
               << "                                  panel_widget='CheckBox'>\n"
               << "              <BooleanDomain name='bool'/>\n"
               << "            </IntVectorProperty>\n\n";
    } else if (ch.isButton) {
  sources_ << "            <IntVectorProperty name='" << P << "' label='" << P << " '\n"
               << "                                  command='SetTuple1Int'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
              //  << "                                  number_of_elements='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='0'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
               << "                                  immediate_apply='1'\n"
               << "                                  panel_widget='CheckBox'>\n"
               << "              <BooleanDomain name='bool'/>\n"
               << "              <Documentation>\n"
               << "              </Documentation>\n"
               << "            </IntVectorProperty>\n\n";
    } else if (ch.isEnum) {
      sources_ << "            <IntVectorProperty name='" << P << "'\n"
               << "                                  command='SetTuple1Int'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
               << "                                  \n"
              //  << "                                  number_of_elements='1'\n"
               << "                                  default_values='" << ch.defaultInt << "'\n"
               << "                                  immediate_apply='1'\n"
               << "                                  \n"
               << "                                  >\n"
               << "            </IntVectorProperty>\n\n";
    } else if (!ch.isVector) {
      const double sdef = ch.defaultValue;
      if (ch.isInteger) {
        sources_ << "            <IntVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='SetTuple1Int'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
              //  << "                                  number_of_elements='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='" << static_cast<int>(sdef) << "'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
               << "                                  >\n"
               << "            </IntVectorProperty>\n\n";
      } else {
        sources_ << "            <DoubleVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='SetTuple1Double'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
              //  << "                                  number_of_elements='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='" << sdef << "'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
               << "                                  panel_widget='DoubleRange'\n"
               << "                                  >\n"
               << "            </DoubleVectorProperty>\n\n";
      }
    } else {
      const double d0 = ch.hasVectorRanges ? ch.vecDefaults[0] : ch.defaultValue;
      const double d1 = ch.hasVectorRanges ? ch.vecDefaults[1] : ch.defaultValue;
      const double d2 = ch.hasVectorRanges ? ch.vecDefaults[2] : ch.defaultValue;
      unsigned dim = ch.vecDim;
      std::string cmd;
      if (ch.isInteger) cmd = (dim == 1 ? "SetTuple1Int" : dim == 2 ? "SetTuple2Int" : "SetTuple3Int");
      else cmd = (dim == 1 ? "SetTuple1Double" : dim == 2 ? "SetTuple2Double" : "SetTuple3Double");
      std::string def;
      if (ch.isInteger) {
        // format integer defaults without decimals
        int i0 = static_cast<int>(d0);
        int i1 = static_cast<int>(d1);
        int i2 = static_cast<int>(d2);
        def = (dim == 1 ? std::to_string(i0)
                        : dim == 2 ? (std::to_string(i0) + " " + std::to_string(i1))
                                   : (std::to_string(i0) + " " + std::to_string(i1) + " " + std::to_string(i2)));
      } else {
        def = (dim == 1 ? std::to_string(d0)
                        : dim == 2 ? (std::to_string(d0) + " " + std::to_string(d1))
                                   : (std::to_string(d0) + " " + std::to_string(d1) + " " + std::to_string(d2)));
      }
      if (ch.isInteger) {
        sources_ << "            <IntVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='" << cmd << "'\n"
               << "                                  use_index='1'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='" << def << "'\n"
               << "                                  number_of_elements='" << dim << "'\n"
               << "                                  number_of_elements_per_command='" << dim << "'\n"
               << "                                  repeat_command='1'>\n"
               << "            </IntVectorProperty>\n\n";
      } else {
        sources_ << "            <DoubleVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='" << cmd << "'\n"
               << "                                  use_index='1'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='" << def << "'\n"
               << "                                  number_of_elements='" << dim << "'\n"
               << "                                  number_of_elements_per_command='" << dim << "'\n"
               << "                                  repeat_command='1'>\n"
               << "            </DoubleVectorProperty>\n\n";
      }
    }
  }

  // Property groups
  // Property groups per struct namespace (for singles) and a fallback 'Numerics' for loose values
  // Collect struct members (non-array) including numerics, bools, enums, and buttons
  std::map<std::string, std::vector<const Channel*>> structMembers;
  bool hasLooseNumerics = false;
  auto ends_with = [](const std::string& s, const std::string& suf){ return s.size()>=suf.size() && s.compare(s.size()-suf.size(), suf.size(), suf)==0; };
  for (const auto& ch : channels_) {
    if (ch.isArray) continue;
    // include bool, enum, button and numerics in struct groups
    if (ch.propertyName.find('.') != std::string::npos) {
      structMembers[ch.ns].push_back(&ch);
    } else {
      hasLooseNumerics = true;
    }
  }
  for (const auto& kv : structMembers) {
    const std::string& ns = kv.first;
    sources_ << "            <PropertyGroup label='" << ns << "' panel_widget='PropertyCollection'>\n";
    sources_ << "                <Hints>\n";
    sources_ << "                  <PropertyCollectionWidgetPrototype group='misc' name='" << ns << "Prototype' />\n";
    sources_ << "                </Hints>\n";
    for (const Channel* c : kv.second) {
      const std::string member = c->propertyName.substr(c->propertyName.find('.')+1);
      const std::string P = c->propertyName;
      sources_ << "                <Property name='" << P << "' function='" << ns << ":" << member << "' label='" << P << "'/>\n";
    }
    sources_ << "            </PropertyGroup>\n\n";
  }
  if (hasLooseNumerics) {
    sources_ << "            <PropertyGroup label='Numerics' panel_widget='PropertyCollection'>\n";
    sources_ << "                <Hints>\n";
    sources_ << "                  <PropertyCollectionWidgetPrototype group='misc' name='SteerableNumericsPrototype' />\n";
    sources_ << "                </Hints>\n";
    for (const auto& ch : channels_) {
      if (ch.isArray || ch.isBool || ch.isButton || ch.isEnum) continue;
      if (ch.propertyName.find('.') != std::string::npos) continue; // already grouped by struct
      if (ch.isVector) {
        sources_ << "                <Property name='" << ch.propertyName << "' function='vec" << ch.vecDim << "_" << ch.label << "' label='" << ch.propertyName << "'/>\n";
      } else {
        sources_ << "                <Property name='" << ch.propertyName << "' function='scaleFactor_" << ch.label << "' label='" << ch.propertyName << "'/>\n";
      }
    }
    sources_ << "            </PropertyGroup>\n\n";
  }

  // Removed legacy LinMap Property Group; generic grouping applies now.

  bool hasEnums = false;
  for (const auto& ch : channels_) { if (!ch.isArray && ch.propertyName.find('.') == std::string::npos && ch.isEnum && !ch.enumEntries.empty()) { hasEnums = true; break; } }
  if (hasEnums) {
    sources_ << "            <PropertyGroup label='Enums' panel_widget='PropertyCollection'>\n";
    sources_ << "                <Hints>\n";
    sources_ << "                  <PropertyCollectionWidgetPrototype group='misc' name='SteerableEnumsPrototype' />\n";
    sources_ << "                </Hints>\n";
    for (const auto& ch : channels_) {
      if (ch.isArray || ch.propertyName.find('.') != std::string::npos || !ch.isEnum || ch.enumEntries.empty()) continue;
      sources_ << "                <Property name='" << ch.label << "' function='PrototypeEnum_" << ch.label << "' label='" << ch.label << "'/>\n";
    }
    sources_ << "            </PropertyGroup>\n\n";
  }

  bool hasSwitches = false;
  for (const auto& ch : channels_) { if (!ch.isArray && ch.propertyName.find('.') == std::string::npos && ch.isBool) { hasSwitches = true; break; } }
  if (hasSwitches) {
    sources_ << "            <PropertyGroup label='Switches'>\n";
    for (const auto& ch : channels_) {
      if (ch.isArray || ch.propertyName.find('.') != std::string::npos || !ch.isBool) continue;
      sources_ << "                <Property name='" << ch.label << "' function='bool' label='" << ch.label << "' />\n";
    }
    sources_ << "            </PropertyGroup>\n\n";
  }

  bool hasButtons = false;
  for (const auto& ch : channels_) { if (!ch.isArray && ch.propertyName.find('.') == std::string::npos && ch.isButton) { hasButtons = true; break; } }
  if (hasButtons) {
    sources_ << "            <PropertyGroup label='Buttons / Triggers'>\n";
    for (const auto& ch : channels_) {
      if (ch.isArray || ch.propertyName.find('.') != std::string::npos || !ch.isButton) continue;
      sources_ << "                <Property name='" << ch.label <<  "' />\n";
    }
    sources_ << "            </PropertyGroup>\n\n";
  }

  sources_ << "            <Hints>\n";
  sources_ << "              <CatalystInitializePropertiesWithMesh mesh='steerable_channel_0D_mesh'>\n";
  for (const auto& ch : channels_) {
    if (ch.isArray) continue;
    if (ch.isButton) continue;
    sources_ << "                <Property name='" << ch.propertyName << "' association='point' array='steerable_field_f_" << ch.label << "' />\n";
  }
  sources_ << "              </CatalystInitializePropertiesWithMesh>\n";
  sources_ << "            </Hints>\n";

  sources_ << "        </SourceProxy>\n\n";
}

void ProxyWriter::appendArraySourceProxy(const std::string& ns,
                                         const std::vector<const Channel*>& chans) {
  // Per-array source proxy
  sources_ << "        <SourceProxy class='vtkSteeringDataGenerator' name='SteerableParameters_" << ns << "'>\n"
           << "            <IntVectorProperty name='PartitionType' command='SetPartitionType' number_of_elements='1' default_values='1' panel_visibility='never'>\n"
           << "            </IntVectorProperty>\n\n"
           << "            <IntVectorProperty name='FieldAssociation' command='SetFieldAssociation' number_of_elements='1' default_values='0' panel_visibility='never'>\n"
           << "            </IntVectorProperty>\n\n";

  // Determine initial list length for this namespace (0 means no list prepopulation)
  unsigned initLen = 0;
  auto itLen = arrayInitialSize_.find(ns);
  if (itLen != arrayInitialSize_.end()) initLen = itLen->second;

  for (const Channel* c : chans) {
    const std::string& P = c->propertyName; // name without 'array:'
    const std::string& L = c->label;        // full label with 'array:' prefix
    if (c->isButton) {
      // Array buttons should be DoubleVectorProperty with checkbox UI; also pre-size default_values
      sources_ << "            <IntVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='SetTuple1Int'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='";
      if (initLen > 0) {
        for (unsigned i=0;i<initLen;++i) { sources_ << 0; if (i+1<initLen) sources_ << " "; }
      } else {
        sources_ << 0;
      }
      sources_ << "'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
               << "                                  panel_widget='CheckBox'\n"
               << "                                  >\n"
               << "            </IntVectorProperty>\n\n";
    } else if (c->isBool) {
      sources_ << "            <IntVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='SetTuple1Int'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
              //  << "                                  number_of_elements='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='";
      if (initLen > 0) {
        int v = (c->defaultValue != 0.0 ? 1 : 0);
        for (unsigned i=0;i<initLen;++i) { sources_ << v; if (i+1<initLen) sources_ << " "; }
      } else {
        sources_ << (c->defaultValue != 0.0 ? 1 : 0);
      }
      sources_ << "'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
               << "                                  >\n"
               << "            </IntVectorProperty>\n\n";
    } else if (c->isEnum) {
      sources_ << "            <IntVectorProperty name='" << P << "'\n"
               << "                                  command='SetTuple1Int'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
              //  << "                                  number_of_elements='1'\n"
               << "                                  default_values='";
      if (initLen > 0) {
        for (unsigned i=0;i<initLen;++i) { sources_ << c->defaultInt; if (i+1<initLen) sources_ << " "; }
      } else {
        sources_ << c->defaultInt;
      }
      sources_ << "'\n"
               << "                                  immediate_apply='1'>\n"
               << "            </IntVectorProperty>\n\n";
    } else if (c->isVector) {
      const double d0 = c->hasVectorRanges ? c->vecDefaults[0] : c->defaultValue;
      const double d1 = c->hasVectorRanges ? c->vecDefaults[1] : c->defaultValue;
      const double d2 = c->hasVectorRanges ? c->vecDefaults[2] : c->defaultValue;
      unsigned dim = c->vecDim;
      std::string cmd;
      if (c->isInteger) cmd = (dim == 1 ? "SetTuple1Int" : dim == 2 ? "SetTuple2Int" : "SetTuple3Int");
      else cmd = (dim == 1 ? "SetTuple1Double" : dim == 2 ? "SetTuple2Double" : "SetTuple3Double");
      auto writeDef = [&](unsigned count){
        for (unsigned i=0;i<count;++i) {
          if (dim == 1) sources_ << d0;
          else if (dim == 2) sources_ << d0 << " " << d1;
          else sources_ << d0 << " " << d1 << " " << d2;
          if (i+1<count) sources_ << " ";
        }
      };
      if (c->isInteger) {
        sources_ << "            <IntVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='" << cmd << "'\n"
               << "                                  use_index='1'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='";
      } else {
        sources_ << "            <DoubleVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='" << cmd << "'\n"
               << "                                  use_index='1'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='";
      }
      if (initLen > 0) { writeDef(initLen); } else { writeDef(1); }
      sources_ << "'\n"
               << "                                  number_of_elements_per_command='" << dim << "'\n"
               << "                                  repeat_command='1'>\n";
      if (c->isInteger) {
        sources_ << "            </IntVectorProperty>\n\n";
      } else {
        sources_ << "            </DoubleVectorProperty>\n\n";
      }
    } else {
      const double sdef = c->defaultValue;
      if (c->isInteger) {
        sources_ << "            <IntVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='SetTuple1Int'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='";
        if (initLen > 0) {
          for (unsigned i=0;i<initLen;++i) { sources_ << static_cast<int>(sdef); if (i+1<initLen) sources_ << " "; }
        } else {
          sources_ << static_cast<int>(sdef);
        }
        sources_ << "'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
               << "                                  >\n"
               << "            </IntVectorProperty>\n\n";
      } else {
        const double smin = c->hasScalarRange ? c->scalarMin : rangeMin_;
        const double smax = c->hasScalarRange ? c->scalarMax : rangeMax_;
        sources_ << "            <DoubleVectorProperty name='" << P << "' label='" << P << "'\n"
               << "                                  command='SetTuple1Double'\n"
               << "                                  clean_command='Clear'\n"
               << "                                  use_index='1'\n"
               << "                                  initial_string='steerable_field_b_" << L << "'\n"
               << "                                  default_values='";
        if (initLen > 0) {
          for (unsigned i=0;i<initLen;++i) { sources_ << sdef; if (i+1<initLen) sources_ << " "; }
        } else {
          sources_ << sdef;
        }
        sources_ << "'\n"
               << "                                  number_of_elements_per_command='1'\n"
               << "                                  repeat_command='1'\n"
               << "                                  >\n"
               << "            </DoubleVectorProperty>\n\n";
      }
    }
  }

  // Optional property group via per-struct prototype when members have 'ns.member'
  bool hasStructMembers = false;
  for (const Channel* c : chans) { if (c->propertyName.find('.') != std::string::npos) { hasStructMembers = true; break; } }
  if (hasStructMembers) {
    sources_ << "            <PropertyGroup label='" << ns << "' panel_widget='PropertyCollection'>\n";
    sources_ << "                <Hints>\n";
    sources_ << "                  <PropertyCollectionWidgetPrototype group='misc' name='" << ns << "Prototype' />\n";
    sources_ << "                </Hints>\n";
    for (const Channel* c : chans) {
      if (c->propertyName.find('.') == std::string::npos) continue;
      std::string member = c->propertyName.substr(c->propertyName.find('.')+1);
      sources_ << "                <Property name='" << c->propertyName << "' function='" << ns << ":" << member << "' label='" << c->propertyName << "'/>\n";
    }
    sources_ << "            </PropertyGroup>\n\n";
  }

  // Property group for basic-type array channels (no struct member part)
  // Reference the per-ns prototype '<ns>Prototype' and map property by same name.
  bool hasBasicArray = false;
  for (const Channel* c : chans) { if (c->propertyName.find('.') == std::string::npos) { hasBasicArray = true; break; } }
  if (hasBasicArray) {
    sources_ << "            <PropertyGroup label='" << ns << "' panel_widget='PropertyCollection'>\n";
    sources_ << "                <Hints>\n";
    sources_ << "                  <PropertyCollectionWidgetPrototype group='misc' name='" << ns << "Prototype' />\n";
    sources_ << "                </Hints>\n";
    for (const Channel* c : chans) {
      if (c->propertyName.find('.') != std::string::npos) continue;
      const std::string& P = c->propertyName;
      sources_ << "                <Property name='" << P << "' function='" << ns << "' label='" << P << "'/>\n";
    }
    sources_ << "            </PropertyGroup>\n\n";
  }

  // Hints mapping to per-array mesh
  sources_ << "            <Hints>\n";
  sources_ << "              <CatalystInitializePropertiesWithMesh mesh='steerable_channel_1D_mesh_array:" << ns << "'>\n";
  for (const Channel* c : chans) {
    sources_ << "                <Property name='" << c->propertyName << "' association='point' array='steerable_field_f_" << c->label << "' />\n";
  }
  sources_ << "              </CatalystInitializePropertiesWithMesh>\n";
  sources_ << "            </Hints>\n";

  sources_ << "        </SourceProxy>\n\n";
}

// ------------------------------ Config management --------------------------------

void ProxyWriter::setConfigNode(const conduit_cpp::Node n) {
  // Deep copy via YAML serialization, then re-parse into our caches
  std::string ys;
  try {
    ys = n.to_yaml();
  } catch (...) {
    ys.clear();
  }
  if (!ys.empty()) {
    loadConfigFromYamlString(ys);
  }
}

void ProxyWriter::applyScalarConfig(Channel& ch) const {
  // Start from type default if present
  if (typeDefaultScalar_.has) {
    ch.hasScalarRange = true;
    ch.scalarMin = typeDefaultScalar_.min;
    ch.scalarMax = typeDefaultScalar_.max;
    if (!ch.preserveScalarDefault) {
      ch.defaultValue = typeDefaultScalar_.def;
      ch.hasScalarDefault = true;
    }
  }
  auto it = labelScalarCfg_.find(ch.label);
  if (it != labelScalarCfg_.end() && it->second.has) {
    ch.hasScalarRange = true;
    ch.scalarMin = it->second.min;
    ch.scalarMax = it->second.max;
    if (!ch.preserveScalarDefault) {
      ch.defaultValue = it->second.def;
      ch.hasScalarDefault = true;
    }
  }
}

static void parse_inline_map_into(const std::string& inlineMap,
                                  bool& hasAny,
                                  double& vmin, double& vmax, double& vdef) {
  hasAny = false;
  std::string s = inlineMap;
  // ensure braces removed
  auto lb = s.find('{'); if (lb != std::string::npos) s.erase(0, lb+1);
  auto rb = s.rfind('}'); if (rb != std::string::npos) s.erase(rb);
  s = trim(s);
  double tmp;
  if (extract_number(s, "min", tmp)) { vmin = tmp; hasAny = true; }
  if (extract_number(s, "max", tmp)) { vmax = tmp; hasAny = true; }
  if (extract_number(s, "default", tmp)) { vdef = tmp; hasAny = true; }
}

bool ProxyWriter::loadConfigFromYamlFile(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) return false;
  std::ostringstream ss; ss << ifs.rdbuf();
  return loadConfigFromYamlString(ss.str());
}

bool ProxyWriter::loadConfigFromYamlString(const std::string& yaml) {
  // Reset caches to only track per-label ranges
  hasConfig_ = false;
  // Drop support for type defaults and steer_params; keep only label ranges
  typeDefaultScalar_ = {};
  typeDefaultVectorComp_ = {};
  labelScalarCfg_.clear();
  labelVectorCfg_.clear();

  enum class Mode { None, Ranges };
  Mode mode = Mode::None;
  std::string currentLabel;

  std::istringstream iss(yaml);
  std::string rawLine;
  while (std::getline(iss, rawLine)) {
    std::string line = strip_comment(rawLine);
    line = rtrim(line);
    if (line.find_first_not_of(' ') == std::string::npos) continue; // empty
    int indent = leading_spaces(line);
    std::string t = trim(line);

    // Only consider the 'ranges:' section
    if (indent == 0 && t == "ranges:") { mode = Mode::Ranges; currentLabel.clear(); continue; }
    if (mode != Mode::Ranges) continue;

    // simple ranges: label keys under 'ranges:' with min/max pairs
    if (indent == 2) {
      auto pos = t.find(':'); if (pos == std::string::npos) continue;
      currentLabel = unquote(t.substr(0, pos));
      continue;
    }
    if (indent == 4) {
      // inside label map: accept one key per line or inline multiple keys
      if (currentLabel.empty()) continue;
      bool minPresent=false, maxPresent=false, defPresent=false;
      double mn=0.0, mx=0.0, df=0.0, tmp=0.0;
      // Inline parse (handles e.g. "{ min: -1, max: 1 }")
      bool any=false; parse_inline_map_into("{" + t + "}", any, mn, mx, df);
      // Explicit single-key parsing (overrides inline if specific)
      if (starts_with(t, "min:")) { if (extract_number(t, "min", tmp)) { mn = tmp; minPresent = true; } }
      if (starts_with(t, "max:")) { if (extract_number(t, "max", tmp)) { mx = tmp; maxPresent = true; } }
      if (starts_with(t, "default:")) { if (extract_number(t, "default", tmp)) { df = tmp; defPresent = true; } }
      // If inline map contained keys but not detected as single-key lines, infer presence
      if (any && !minPresent && t.find("min:") != std::string::npos) minPresent = true;
      if (any && !maxPresent && t.find("max:") != std::string::npos) maxPresent = true;
      if (any && !defPresent && t.find("default:") != std::string::npos) defPresent = true;

      if (minPresent || maxPresent || defPresent) {
        ScalarCfg& sc = labelScalarCfg_[currentLabel]; sc.has = true;
        if (minPresent) sc.min = mn;
        if (maxPresent) sc.max = mx;
        if (defPresent) sc.def = df;

        VectorCfg& vc = labelVectorCfg_[currentLabel]; vc.has = true; vc.uniform = true;
        if (minPresent) vc.umin = mn;
        if (maxPresent) vc.umax = mx;
        if (defPresent) vc.udef = df;

        hasConfig_ = true;
      }
      continue;
    }
    // If we dedent back to 0, we're out of ranges
    if (indent == 0) { mode = Mode::None; }
  }

  return hasConfig_;
}

} // namespace ippl
