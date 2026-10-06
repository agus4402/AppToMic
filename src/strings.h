#pragma once
#include <windows.h>

// UI text. English is the default; Spanish is used when the Windows display language is Spanish.
struct Strings {
    const wchar_t* appsLabel;
    const wchar_t* refresh;
    const wchar_t* micLabel;
    const wchar_t* appVolLabel;
    const wchar_t* micVolLabel;
    const wchar_t* sendOnOpen;
    const wchar_t* startWithWindows;
    const wchar_t* start;
    const wchar_t* stop;
    const wchar_t* open;
    const wchar_t* exit;
    const wchar_t* noMic;
    const wchar_t* closedSuffix;
    const wchar_t* stopped;
    const wchar_t* sendingFmt;  // %zu apps, %s mic suffix
    const wchar_t* plusMic;
    const wchar_t* echoWarning;
    const wchar_t* trayRunning;
    const wchar_t* trayStopped;
    const wchar_t* cableLost;
    const wchar_t* cableMissing;
    const wchar_t* cableOpenFailedFmt;  // %08X HRESULT
    const wchar_t* balloonTitle;
    const wchar_t* balloonText;
};

inline constexpr Strings kEnglish = {
    L"Apps sent to the virtual microphone:",
    L"Refresh",
    L"Microphone:",
    L"Apps vol.",
    L"Mic vol.",
    L"Send audio on open",
    L"Start with Windows",
    L"Start",
    L"Stop",
    L"Open",
    L"Exit",
    L"(No microphone)",
    L"  (closed)",
    L"Stopped. In Discord/OBS pick \"CABLE Output\" as your microphone.",
    L"Sending to CABLE Input: %zu app(s)%s.",
    L" + microphone",
    L"\n⚠ Your default output is CABLE Input: change it or you'll hear echo.",
    L"AppToMic - sending",
    L"AppToMic - stopped",
    L"Lost the CABLE Input device. Click Start to retry.",
    L"VB-Cable (\"CABLE Input\") not found. Install it from vb-audio.com/Cable and restart.",
    L"Couldn't open CABLE Input (error 0x%08X).",
    L"AppToMic is running in the background",
    L"Click the tray icon to open it.",
};

inline constexpr Strings kSpanish = {
    L"Apps que se envían al micrófono virtual:",
    L"Refrescar",
    L"Micrófono:",
    L"Vol. apps",
    L"Vol. mic",
    L"Enviar audio al abrir",
    L"Iniciar con Windows",
    L"Iniciar",
    L"Detener",
    L"Abrir",
    L"Salir",
    L"(Sin micrófono)",
    L"  (cerrada)",
    L"Detenido. En Discord/OBS elegí \"CABLE Output\" como micrófono.",
    L"Enviando a CABLE Input: %zu app(s)%s.",
    L" + micrófono",
    L"\n⚠ Tu salida por defecto es CABLE Input: cambiala o habrá eco.",
    L"AppToMic - enviando",
    L"AppToMic - detenido",
    L"Se perdió el dispositivo CABLE Input. Tocá Iniciar para reintentar.",
    L"No se encontró VB-Cable (\"CABLE Input\"). Instalalo desde vb-audio.com/Cable y reiniciá.",
    L"No se pudo abrir CABLE Input (error 0x%08X).",
    L"AppToMic está en segundo plano",
    L"Hacé clic en el ícono de la bandeja para abrirlo.",
};

inline const Strings& S() {
    static const Strings& s = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH ? kSpanish : kEnglish;
    return s;
}
