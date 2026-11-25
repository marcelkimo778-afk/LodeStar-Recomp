/* LodeStar.exe - starts Kimo's Lodestar launcher (launcher/launcher.py) with the windowless Python, from the
 * folder this exe is in.  A Windows (GUI) program: no console window ever opens.  It only starts the launcher, so
 * launcher updates never need a new LodeStar.exe.  Built by Engine/Tools/build_lodestar.py.
 *
 * Python: pyw.exe (the Python launcher, picks Python 3), else pythonw.exe on PATH, else a message box. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wchar.h>

static int start(const wchar_t *exe, const wchar_t *dir, const wchar_t *script)
{
    wchar_t found[MAX_PATH], cmd[3 * MAX_PATH];
    if (!SearchPathW(NULL, exe, NULL, MAX_PATH, found, NULL)) return 0;
    _snwprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%s\" \"%s\"", found, script);
    cmd[sizeof cmd / sizeof cmd[0] - 1] = 0;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(found, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show)
{
    (void)inst, (void)prev, (void)args, (void)show;
    wchar_t dir[MAX_PATH], script[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    dir[n ? n - 1 : 0] = 0;                                      /* the exe's folder, no trailing slash */
    _snwprintf(script, sizeof script / sizeof script[0], L"%s\\System\\launcher\\launcher.py", dir);
    script[sizeof script / sizeof script[0] - 1] = 0;
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"System\\launcher\\launcher.py was not found next to LodeStar.exe.\n\n"
                          L"Keep LodeStar.exe in the project folder.", L"Kimo's Lodestar", MB_ICONERROR);
        return 1;
    }
    if (start(L"pyw.exe", dir, script) || start(L"pythonw.exe", dir, script)) return 0;
    MessageBoxW(NULL, L"Python 3 was not found.\n\nInstall Python 3 from python.org (with the \"py launcher\" "
                      L"option) and start LodeStar.exe again.", L"Kimo's Lodestar", MB_ICONERROR);
    return 1;
}
