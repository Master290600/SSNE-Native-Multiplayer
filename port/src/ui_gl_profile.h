#pragma once
namespace port {
// Opt-in diagnostics for the pinned ImGui 1.91.5 desktop GL loader only.
// Call after ImGui_ImplOpenGL3_Init, on the render thread.
void profile_ui_gl_calls();
}
