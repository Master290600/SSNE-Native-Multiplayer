#include "gc/editor_effect.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <vector>
namespace {
unsigned checks = 0;
void check(bool value, const char *why) {
  ++checks;
  if (!value)
    throw std::runtime_error(why);
}
struct Comma : std::numpunct<char> {
  char do_decimal_point() const override { return ','; }
};
} // namespace
int main() {
  try {
    using namespace gc::editor;
    std::string why;
    for (const auto kind : {EffectKind::Light, EffectKind::Explosion})
      for (const auto radius : {.1f, .125f, 1.f, 3.1415927f, 63.999f, 64.f}) {
        Effect source;
        source.kind = kind;
        source.radius = radius;
        source.damage = kind == EffectKind::Explosion ? 1234.5678f : 40;
        source.scale = kind == EffectKind::Explosion ? 2.875f : 1;
        source.colour = {1, 127, 255, 255};
        Effect parsed;
        check(decode_effect(encode_effect(source), parsed, why),
              "Valid effect rejected");
        check(parsed.radius == source.radius && parsed.kind == source.kind,
              "Float roundtrip lost precision");
        if (kind == EffectKind::Light)
          check(parsed.colour == source.colour, "RGB roundtrip changed colour");
        else
          check(parsed.damage == source.damage && parsed.scale == source.scale,
                "Explosion roundtrip changed scalars");
      }
    const auto previous = std::locale();
    std::locale::global(std::locale(previous, new Comma));
    Effect value;
    value.radius = 1.25f;
    Effect decoded;
    check(encode_effect(value).find("1.25") != std::string::npos,
          "Encoding used system decimal separator");
    check(decode_effect(encode_effect(value), decoded, why) &&
              decoded.radius == 1.25f,
          "Parsing used system decimal separator");
    std::locale::global(previous);
    const std::vector<std::string> invalid = {
        "",
        "SSNE_EDITOR_EFFECT_V2:L:1:0:0:0",
        "SSNE_EDITOR_EFFECT_V1:Q:1:0:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:1:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:1:0:0:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:nan:0:0:0",
        "SSNE_EDITOR_EFFECT_V1:E:inf:1:1:0",
        "SSNE_EDITOR_EFFECT_V1:L: 1:0:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:1 :0:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:+1:0:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:1,5:0:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:0:0:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:65:0:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:1:256:0:0",
        "SSNE_EDITOR_EFFECT_V1:L:1:-1:0:0",
        "SSNE_EDITOR_EFFECT_V1:E:1:-1:1:0",
        "SSNE_EDITOR_EFFECT_V1:E:1:10001:1:0",
        "SSNE_EDITOR_EFFECT_V1:E:1:1:0:0",
        "SSNE_EDITOR_EFFECT_V1:E:1:1:9:0",
        "SSNE_EDITOR_EFFECT_V1:E:1:1:1:5",
        "SSNE_EDITOR_EFFECT_V1:E:1:1:1:1junk",
        std::string(200, 'A')};
    for (const auto &text : invalid) {
      Effect destination;
      destination.radius = 23;
      const auto old = destination;
      check(!decode_effect(text, destination, why),
            "Malformed effect accepted");
      check(destination == old && !why.empty(),
            "Failed decode modified destination or lost error");
    }
    for (const auto bad : {std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity(), 0.f, 65.f}) {
      Effect item;
      item.radius = bad;
      check(!valid_effect(item, why), "Invalid effect radius accepted");
    }
    std::cout << "PASS editor_effect_schema checks=" << checks << "\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << " checks=" << checks << "\n";
    return 1;
  }
}
