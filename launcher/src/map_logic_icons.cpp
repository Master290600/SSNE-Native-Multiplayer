#include "map_logic_icons.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace mapedit {
void draw_logic_icon(ImDrawList *d, ImVec2 c, float radius, LogicIcon kind,
                     bool selected, bool hovered) {
  if (!d)
    return;
  const float r = std::clamp(radius, 6.f, 32.f);
  const float stroke = std::max(1.2f, r * .12f);
  const auto ink = logic_icon_color(kind);
  auto p = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
  auto line = [&](float x, float y, float x2, float y2) {
    d->AddLine(p(x, y), p(x2, y2), ink, stroke);
  };
  auto circle = [&](float x, float y, float rr, bool filled = false) {
    if (filled)
      d->AddCircleFilled(p(x, y), rr * r, ink, 16);
    else
      d->AddCircle(p(x, y), rr * r, ink, 20, stroke);
  };
  auto box = [&](float x, float y, float x2, float y2, float round = .06f) {
    d->AddRect(p(x, y), p(x2, y2), ink, round * r, 0, stroke);
  };
  auto triangle = [&](float ax, float ay, float bx, float by, float cx,
                      float cy, bool filled = false) {
    if (filled)
      d->AddTriangleFilled(p(ax, ay), p(bx, by), p(cx, cy), ink);
    else
      d->AddTriangle(p(ax, ay), p(bx, by), p(cx, cy), ink, stroke);
  };
  auto arrow = [&](float x, float y, float xx, float yy) {
    line(x, y, xx, yy);
    const float a = std::atan2(yy - y, xx - x), s = .25f;
    line(xx, yy, xx - s * std::cos(a - .65f), yy - s * std::sin(a - .65f));
    line(xx, yy, xx - s * std::cos(a + .65f), yy - s * std::sin(a + .65f));
  };
  d->AddRectFilled(p(-1.05f, -1.05f), p(1.05f, 1.05f), IM_COL32(8, 13, 24, 220),
                   r * .26f);
  if (selected || hovered)
    d->AddRect(p(-1.13f, -1.13f), p(1.13f, 1.13f),
               selected ? IM_COL32(255, 244, 187, 255)
                        : IM_COL32(235, 243, 255, 235),
               r * .29f, 0, selected ? 2.f : 1.4f);
  auto skull = [&](float x, float y, float s) {
    d->AddCircle(p(x, y - .12f * s), .48f * r * s, ink, 16, stroke);
    d->AddRectFilled(p(x - .35f * s, y + .12f * s),
                     p(x + .35f * s, y + .48f * s), ink, .04f * r);
    const auto hole = IM_COL32(8, 13, 24, 255);
    d->AddCircleFilled(p(x - .18f * s, y - .10f * s), .10f * r * s, hole, 8);
    d->AddCircleFilled(p(x + .18f * s, y - .10f * s), .10f * r * s, hole, 8);
    for (float k : {-.2f, 0.f, .2f})
      d->AddLine(p(x + k * s, y + .28f * s), p(x + k * s, y + .48f * s), hole,
                 stroke * .65f);
  };
  switch (kind) {
  case LogicIcon::PlayerSpawn:
    circle(-.2f, -.44f, .21f);
    line(-.2f, -.2f, -.2f, .25f);
    line(-.6f, .04f, .2f, .04f);
    line(-.2f, .25f, -.52f, .67f);
    line(-.2f, .25f, .12f, .67f);
    line(.52f, -.7f, .52f, .66f);
    triangle(.52f, -.7f, .96f, -.48f, .52f, -.25f, true);
    break;
  case LogicIcon::Enemy:
    skull(0, 0, 1.35f);
    break;
  case LogicIcon::WaveSpawner:
    skull(0, -.24f, .85f);
    line(-.7f, .4f, .7f, .4f);
    line(-.55f, .62f, .55f, .62f);
    line(-.4f, .82f, .4f, .82f);
    break;
  case LogicIcon::Pickup:
    d->AddQuad(p(0, -.82f), p(.75f, 0), p(0, .82f), p(-.75f, 0), ink, stroke);
    line(-.32f, 0, .32f, 0);
    line(0, -.32f, 0, .32f);
    break;
  case LogicIcon::Door:
  case LogicIcon::Exit:
    box(-.64f, -.8f, .58f, .8f);
    line(-.39f, -.56f, -.39f, .62f);
    circle(.25f, .14f, .06f, true);
    if (kind == LogicIcon::Exit)
      arrow(-.05f, 0, .94f, 0);
    break;
  case LogicIcon::Lock:
    box(-.58f, -.12f, .58f, .72f);
    d->PathArcTo(p(0, -.12f), r * .41f, 3.14159265f, 6.28318531f, 16);
    d->PathStroke(ink, 0, stroke);
    circle(0, .22f, .10f, true);
    line(0, .25f, 0, .48f);
    break;
  case LogicIcon::AreaTrigger:
    for (int k = 0; k < 3; ++k) {
      float a = -.8f + k * .58f;
      line(a, -.75f, a + .28f, -.75f);
      line(a, .75f, a + .28f, .75f);
    }
    line(-.8f, -.5f, -.8f, .5f);
    line(.8f, -.5f, .8f, .5f);
    arrow(-.45f, 0, .45f, 0);
    break;
  case LogicIcon::Relay:
    line(-.9f, 0, -.43f, 0);
    line(.43f, 0, .9f, 0);
    d->AddQuad(p(.05f, -.8f), p(-.48f, .08f), p(-.02f, .08f), p(-.06f, .8f),
               ink, stroke);
    line(-.06f, .8f, .49f, -.08f);
    line(.49f, -.08f, .03f, -.08f);
    break;
  case LogicIcon::Counter:
    line(-.35f, -.75f, -.5f, .75f);
    line(.35f, -.75f, .2f, .75f);
    line(-.75f, -.25f, .75f, -.25f);
    line(-.8f, .3f, .7f, .3f);
    break;
  case LogicIcon::Timer:
    circle(0, .08f, .67f);
    box(-.22f, -.9f, .22f, -.75f);
    line(0, .08f, 0, -.36f);
    line(0, .08f, .35f, .25f);
    break;
  case LogicIcon::Button:
    box(-.72f, -.66f, .72f, .7f, .14f);
    circle(0, .06f, .4f, true);
    arrow(0, -.95f, 0, -.42f);
    break;
  case LogicIcon::Toggle:
    d->AddRect(p(-.85f, -.4f), p(.85f, .4f), ink, r * .4f, 0, stroke);
    circle(.4f, 0, .26f, true);
    line(-.47f, -.12f, -.47f, .12f);
    break;
  case LogicIcon::Branch:
    d->AddQuad(p(-.65f, -.45f), p(-.2f, 0), p(-.65f, .45f), p(-1.1f, 0), ink,
               stroke);
    line(-.2f, 0, .18f, 0);
    line(.18f, -.56f, .18f, .56f);
    arrow(.18f, -.56f, .87f, -.56f);
    arrow(.18f, .56f, .87f, .56f);
    break;
  case LogicIcon::Camera:
    box(-.75f, -.37f, .3f, .55f);
    circle(-.48f, -.63f, .2f);
    circle(.02f, -.63f, .2f);
    d->AddQuad(p(.3f, -.14f), p(.85f, -.46f), p(.85f, .63f), p(.3f, .32f), ink,
               stroke);
    break;
  case LogicIcon::Sound:
    box(-.84f, -.27f, -.48f, .27f);
    d->AddQuad(p(-.48f, -.27f), p(.0f, -.68f), p(.0f, .68f), p(-.48f, .27f),
               ink, stroke);
    for (float s : {.40f, .72f}) {
      d->PathArcTo(p(-.03f, 0), s * r, -.8f, .8f, 14);
      d->PathStroke(ink, 0, stroke);
    }
    break;
  case LogicIcon::Music:
    circle(-.48f, .54f, .22f, true);
    circle(.48f, .31f, .22f, true);
    line(-.26f, .54f, -.26f, -.6f);
    line(.7f, .31f, .7f, -.82f);
    line(-.26f, -.6f, .7f, -.82f);
    line(-.26f, -.35f, .7f, -.57f);
    break;
  case LogicIcon::Light:
    circle(0, -.18f, .42f);
    line(-.22f, .16f, -.2f, .55f);
    line(.22f, .16f, .2f, .55f);
    line(-.2f, .55f, .2f, .55f);
    line(-.14f, .76f, .14f, .76f);
    for (int k = 0; k < 7; ++k) {
      float a = (k * 3.14159265f / 6) + 3.14159265f;
      line(.6f * std::cos(a), -.18f + .6f * std::sin(a), .83f * std::cos(a),
           -.18f + .83f * std::sin(a));
    }
    break;
  case LogicIcon::Explosion:
    for (int k = 0; k < 16; ++k) {
      float a = k * 6.28318531f / 16, s = k % 2 ? .36f : .9f;
      d->PathLineTo(p(std::cos(a) * s, std::sin(a) * s));
    }
    d->PathStroke(ink, ImDrawFlags_Closed, stroke);
    break;
  case LogicIcon::PathMarker:
    circle(0, -.3f, .47f);
    line(-.43f, -.1f, 0, .85f);
    line(.43f, -.1f, 0, .85f);
    circle(0, -.3f, .12f, true);
    break;
  case LogicIcon::ArrowMarker:
    line(-.21f, .8f, -.21f, -.1f);
    line(.21f, .8f, .21f, -.1f);
    line(-.21f, .8f, .21f, .8f);
    line(-.21f, -.1f, -.65f, -.1f);
    line(.21f, -.1f, .65f, -.1f);
    line(-.65f, -.1f, 0, -.88f);
    line(.65f, -.1f, 0, -.88f);
    break;
  case LogicIcon::Teleport:
    d->AddEllipse(p(-.55f, 0), ImVec2(r * .24f, r * .73f), ink, 0, 24, stroke);
    d->AddEllipse(p(.55f, 0), ImVec2(r * .24f, r * .73f), ink, 0, 24, stroke);
    arrow(-.34f, 0, .36f, 0);
    break;
  case LogicIcon::Copier:
    box(-.72f, -.7f, .26f, .27f);
    box(-.27f, -.27f, .72f, .7f);
    break;
  case LogicIcon::TouchField:
    box(-.8f, -.73f, .8f, .73f, .16f);
    circle(0, 0, .34f);
    line(-.17f, .56f, -.17f, -.19f);
    line(-.17f, -.19f, .10f, -.19f);
    line(.10f, -.19f, .10f, .18f);
    line(.1f, .18f, .35f, .38f);
    break;
  case LogicIcon::Sequence:
    for (int k = 0; k < 3; ++k) {
      float x = -.83f + k * .61f;
      box(x, -.48f, x + .45f, .48f);
    }
    triangle(-.12f, -.22f, -.12f, .22f, .18f, 0, true);
    break;
  case LogicIcon::EnemyPath:
    d->AddEllipse(p(-.35f, -.34f), ImVec2(.18f * r, .32f * r), ink, -.32f, 16,
                  stroke);
    d->AddEllipse(p(.35f, .24f), ImVec2(.18f * r, .32f * r), ink, .32f, 16,
                  stroke);
    circle(-.47f, -.72f, .10f, true);
    circle(.48f, -.12f, .10f, true);
    break;
  case LogicIcon::Message:
    box(-.8f, -.65f, .8f, .4f, .18f);
    line(-.3f, .4f, -.58f, .82f);
    line(-.58f, .82f, .17f, .4f);
    for (float x : {-.4f, 0.f, .4f})
      circle(x, -.1f, .07f, true);
    break;
  case LogicIcon::DamageField:
    triangle(0, -.85f, -.85f, .68f, .85f, .68f);
    line(0, -.35f, 0, .2f);
    circle(0, .43f, .07f, true);
    break;
  case LogicIcon::Particles:
    for (auto s :
         {ImVec2(-.35f, -.35f), ImVec2(.5f, .15f), ImVec2(-.25f, .63f)}) {
      const float rr = s.x == .5f ? .25f : .18f;
      line(s.x - rr, s.y, s.x + rr, s.y);
      line(s.x, s.y - rr, s.x, s.y + rr);
    }
    line(-.9f, .3f, -.62f, .1f);
    line(.1f, -.84f, .1f, -.6f);
    line(.72f, -.54f, .49f, -.35f);
    break;
  case LogicIcon::Destructible:
    box(-.75f, -.72f, .75f, .72f);
    line(.13f, -.72f, -.17f, -.22f);
    line(-.17f, -.22f, .3f, .15f);
    line(.3f, .15f, -.02f, .72f);
    line(-.17f, -.22f, -.6f, .05f);
    break;
  case LogicIcon::Vehicle:
    box(-.83f, -.04f, .83f, .46f);
    line(-.6f, -.04f, -.36f, -.53f);
    line(-.36f, -.53f, .4f, -.53f);
    line(.4f, -.53f, .68f, -.04f);
    circle(-.5f, .51f, .2f, true);
    circle(.5f, .51f, .2f, true);
    line(.05f, -.5f, .05f, -.08f);
    break;
  case LogicIcon::BouncePad:
    box(-.8f, .46f, .8f, .73f);
    line(-.53f, .46f, -.28f, .16f);
    line(-.28f, .16f, -.53f, -.04f);
    line(.53f, .46f, .28f, .16f);
    line(.28f, .16f, .53f, -.04f);
    arrow(0, .1f, 0, -.85f);
    break;
  case LogicIcon::RollingStone:
    circle(.2f, -.05f, .59f);
    line(-.8f, -.55f, -.35f, -.55f);
    line(-.96f, -.14f, -.48f, -.14f);
    line(-.8f, .29f, -.36f, .29f);
    line(.0f, -.5f, .35f, -.33f);
    line(.35f, -.33f, .12f, -.05f);
    line(.12f, -.05f, .45f, .27f);
    break;
  case LogicIcon::Flag:
    line(-.52f, -.83f, -.52f, .85f);
    line(-.78f, .85f, -.18f, .85f);
    d->AddQuad(p(-.52f, -.78f), p(.77f, -.45f), p(.35f, -.12f), p(-.52f, -.02f),
               ink, stroke);
    break;
  case LogicIcon::Statistics:
    box(-.4f, -.75f, .4f, .15f);
    line(-.4f, -.5f, -.75f, -.5f);
    line(-.75f, -.5f, -.75f, -.05f);
    line(-.75f, -.05f, -.38f, .12f);
    line(.4f, -.5f, .75f, -.5f);
    line(.75f, -.5f, .75f, -.05f);
    line(.75f, -.05f, .38f, .12f);
    line(0, .15f, 0, .62f);
    line(-.38f, .77f, .38f, .77f);
    line(-.27f, .62f, .27f, .62f);
    break;
  case LogicIcon::Movie:
    box(-.8f, -.42f, .8f, .7f);
    box(-.8f, -.78f, .8f, -.42f, 0);
    for (float x : {-.55f, -.05f, .45f})
      line(x, -.78f, x + .25f, -.42f);
    triangle(-.2f, -.17f, -.2f, .43f, .33f, .13f, true);
    break;
  case LogicIcon::Gate:
    line(-.9f, -.38f, -.45f, -.38f);
    line(-.9f, .38f, -.45f, .38f);
    line(-.45f, -.7f, -.45f, .7f);
    line(-.45f, -.7f, -.05f, -.7f);
    line(-.45f, .7f, -.05f, .7f);
    d->PathArcTo(p(-.05f, 0), r * .7f, -1.57079633f, 1.57079633f, 20);
    d->PathStroke(ink, 0, stroke);
    arrow(.65f, 0, 1.f, 0);
    break;
  default:
    d->AddQuad(p(-.68f, -.4f), p(0, -.77f), p(.68f, -.4f), p(0, .04f), ink,
               stroke);
    line(-.68f, -.4f, -.68f, .43f);
    line(.68f, -.4f, .68f, .43f);
    line(0, .04f, 0, .83f);
    line(-.68f, .43f, 0, .83f);
    line(.68f, .43f, 0, .83f);
    break;
  }
}
} // namespace mapedit
