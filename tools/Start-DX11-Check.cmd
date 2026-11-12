@echo off
setlocal
cd /d "%~dp0"
echo SSNE-RECOOP DirectX 11 Grafiktest
echo Dies ist ein Renderer-Grundtest, noch kein Spiel- oder Multiplayer-Start.
echo Erst werden Pixeltests ausgefuehrt, dann erscheint das Testbild fuer 30 Sekunden.
echo ESC oder Fenster schliessen beendet die Anzeige.
"%~dp0SSNE-RECOOP-DX11-Check.exe" --demo --output "%~dp0dx11-result.png"
set "test_result=%errorlevel%"
if errorlevel 1 (
    echo DX11-Test fehlgeschlagen. Fehlermeldung oben beachten.
) else (
    echo DX11-Grundtest bestanden. Die GX-Spielgrafik ist noch nicht angebunden.
)
pause
exit /b %test_result%
