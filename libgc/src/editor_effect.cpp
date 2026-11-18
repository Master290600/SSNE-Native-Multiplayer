#include "gc/editor_effect.h"
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace gc::editor {
bool is_effect(std::string_view value) {
  return value.starts_with("SSNE_EDITOR_EFFECT_");
}
bool valid_effect(const Effect &v, std::string &error) {
  if (v.kind != EffectKind::Light && v.kind != EffectKind::Explosion)
    error = "Unknown editor effect kind";
  else if (!std::isfinite(v.radius) || v.radius < .1f || v.radius > 64)
    error = "Effect radius must be between 0.1 and 64 metres";
  else if (!std::isfinite(v.damage) || v.damage < 0 || v.damage > 10000)
    error = "Explosion damage must be between 0 and 10000";
  else if (!std::isfinite(v.scale) || v.scale < .1f || v.scale > 8)
    error = "Explosion scale must be between 0.1 and 8";
  else if (v.style > 4)
    error = "Unknown native explosion style";
  else if (v.colour[3] != 255)
    error = "Point lights require an opaque native RGB colour";
  else {
    error.clear();
    return true;
  }
  return false;
}
namespace {
std::string scalar(float v) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<float>::max_digits10) << v;
  return out.str();
}
template <class T> bool number(std::string_view text, T &out) {
  if (text.empty() || text.size() > 24)
    return false;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool number(std::string_view text, float &out) {
  if (text.empty() || text.size() > 24 || text.front() == '+' ||
      text.find_first_of(" \t\r\n") != text.npos)
    return false;
  std::istringstream in{std::string(text)};
  in.imbue(std::locale::classic());
  in >> std::noskipws >> out;
  return !in.fail() && in.peek() == std::char_traits<char>::eof();
}
} // namespace
std::string encode_effect(const Effect &v) {
  std::string error;
  if (!valid_effect(v, error))
    throw std::invalid_argument(error);
  auto out = std::string(effect_prefix);
  if (v.kind == EffectKind::Light)
    out += "L:" + scalar(v.radius) + ":" + std::to_string(v.colour[0]) + ":" +
           std::to_string(v.colour[1]) + ":" + std::to_string(v.colour[2]);
  else
    out += "E:" + scalar(v.radius) + ":" + scalar(v.damage) + ":" +
           scalar(v.scale) + ":" + std::to_string(v.style);
  if (out.size() > effect_max_bytes)
    throw std::invalid_argument("Editor effect is too long");
  return out;
}
bool decode_effect(std::string_view text, Effect &destination,
                   std::string &error) {
  auto fail = [&](const char *why) {
    error = why;
    return false;
  };
  if (text.size() > effect_max_bytes || !text.starts_with(effect_prefix))
    return fail("Unsupported editor effect schema");
  text.remove_prefix(effect_prefix.size());
  std::array<std::string_view, 5> parts;
  for (size_t i = 0; i < parts.size(); ++i) {
    const auto end = text.find(':');
    parts[i] = text.substr(0, end);
    if (i + 1 == parts.size()) {
      if (end != text.npos)
        return fail("Extra editor effect fields");
    } else {
      if (end == text.npos)
        return fail("Incomplete editor effect");
      text.remove_prefix(end + 1);
    }
  }
  Effect v;
  if (!number(parts[1], v.radius))
    return fail("Invalid editor effect radius");
  if (parts[0] == "L") {
    v.kind = EffectKind::Light;
    for (unsigned i = 0; i < 3; ++i) {
      unsigned channel;
      if (!number(parts[2 + i], channel) || channel > 255)
        return fail("Invalid editor light RGB colour");
      v.colour[i] = uint8_t(channel);
    }
  } else if (parts[0] == "E") {
    v.kind = EffectKind::Explosion;
    if (!number(parts[2], v.damage) || !number(parts[3], v.scale) ||
        !number(parts[4], v.style))
      return fail("Invalid editor explosion value");
  } else
    return fail("Unknown editor effect kind");
  if (!valid_effect(v, error))
    return false;
  destination = v;
  return true;
}
} // namespace gc::editor
