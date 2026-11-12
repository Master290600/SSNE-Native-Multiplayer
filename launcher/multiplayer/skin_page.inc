 void skin_page(ImVec2 p,float w,float h,float s,float t){
  auto* dl=ImGui::GetWindowDrawList();
  heading(dl,p+ImVec2(60*s,194*s),32*s,"Dein Spieler",t);
  ImGui::SetCursorScreenPos(p+ImVec2(w-181*s,193*s));if(ImGui::Button("Zurück",{127*s,34*s}) || ImGui::IsKeyPressed(ImGuiKey_Escape,false))skins_page=false;
  const ImVec2 a=p+ImVec2(60*s,239*s),z=p+ImVec2(w*.50f,h-93*s);theme::panel(dl,a,z);
  text(dl,a+ImVec2(22*s,14*s),24*s,skins::names[player_skin],IM_COL32(255,222,135,255),theme::fonts.bold);
  const ImVec2 image_a=a+ImVec2(12*s,51*s),image_z=ImVec2(z.x-12*s,z.y-70*s),extent=image_z-image_a;
  dl->AddEllipseFilled({(a.x+z.x)*.5f,image_z.y-11*s},{74*s,11*s},IM_COL32(93,111,221,55));
  ImGui::SetCursorScreenPos(image_a);ImGui::InvisibleButton("##skin-orbit",extent,ImGuiButtonFlags_MouseButtonLeft);
  const bool dragging=ImGui::IsItemActive();
  if(dragging){skin_yaw+=ImGui::GetIO().MouseDelta.x*.6f;skin_pitch=std::clamp(skin_pitch+ImGui::GetIO().MouseDelta.y*.3f,-24.f,24.f);}
  if(ImGui::IsItemHovered())skin_zoom=std::clamp(skin_zoom+ImGui::GetIO().MouseWheel*.07f,.7f,1.4f);
  if(skin_rotate && !dragging && !preview)skin_yaw+=ImGui::GetIO().DeltaTime*13;
  if(auto texture=skin_preview.render(player_skin,unsigned(extent.x),unsigned(extent.y),skin_yaw,skin_pitch,skin_zoom))dl->AddImage((ImTextureID)(intptr_t)texture,image_a,image_z,{0,1},{1,0});
  else text(dl,image_a+ImVec2(18*s,35*s),16*s,skin_preview.error(),IM_COL32(255,151,122,255),nullptr,extent.x-36*s);
  ImGui::SetCursorScreenPos({a.x+20*s,z.y-62*s});ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{8*s,4*s});
  if(ImGui::Button("Ansicht zurücksetzen")){skin_yaw=0;skin_pitch=0;skin_zoom=1;}
  ImGui::SameLine(0,14*s);ImGui::Checkbox("Auto-Drehung",&skin_rotate);ImGui::PopStyleVar();
  text(dl,{a.x+22*s,z.y-28*s},13*s,"Ziehen: Drehen   ·   Mausrad: Zoom",IM_COL32(153,171,220,255));
  const float grid_x=p.x+w*.535f,grid_right=p.x+w-54*s;
  text(dl,{grid_x,a.y-1*s},14*s,"ORIGINAL-SKINS  /  11 FIGUREN",IM_COL32(178,191,235,255),theme::fonts.bold);
  ImGui::SetCursorScreenPos({grid_x,a.y+28*s});ImGui::BeginChild("##skin-grid",{grid_right-grid_x,z.y-a.y-28*s},false,ImGuiWindowFlags_NoBackground);
  const float cell=(ImGui::GetContentRegionAvail().x-20*s)/3.f,cell_h=137*s;const auto origin=ImGui::GetCursorScreenPos();
  for(unsigned i=0;i<11;++i){
   const auto pos=origin+ImVec2((i%3)*(cell+10*s),(i/3)*(cell_h+10*s));const auto end=pos+ImVec2(cell,cell_h);
   ImGui::SetCursorScreenPos(pos);const std::string id="##skin-"+std::to_string(i);const bool click=ImGui::InvisibleButton(id.c_str(),{cell,cell_h});const bool over=ImGui::IsItemHovered();
   auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(pos,end,i==player_skin?IM_COL32(49,45,100,245):over?IM_COL32(35,39,82,235):IM_COL32(15,20,55,220),5*s);
   draw->AddRect(pos,end,i==player_skin?IM_COL32(255,196,60,255):IM_COL32(92,113,187,130),5*s,0,(i==player_skin?2:1)*s);
   if(auto thumbnail=skin_preview.thumbnail(i))draw->AddImage((ImTextureID)(intptr_t)thumbnail,pos+ImVec2(5*s,4*s),end-ImVec2(5*s,27*s),{0,1},{1,0});
   text(draw,{pos.x+8*s,end.y-23*s},14*s,skins::names[i],i==player_skin?IM_COL32(255,221,123,255):IM_COL32(218,226,251,255));
   if(click){player_skin=i;skin_yaw=0;skin_pitch=0;skin_zoom=1;save();}
  }
  ImGui::EndChild();
  text(dl,p+ImVec2(60*s,h-67*s),14*s,"Deine Auswahl wird gespeichert und beim Hosten oder Beitreten übernommen.",IM_COL32(163,181,228,255));
 }
