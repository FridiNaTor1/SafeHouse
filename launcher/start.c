// SafeHouse.exe at the top of the Windows packages (the setup's folder and the portable zip): starts
// bin\safehouse.exe with the same arguments, so the folder has a program to open instead of a script.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

// The command line after the program's own name (CommandLineToArgvW's rules for argument 0).
static const wchar_t* arguments(const wchar_t* line) {
    if (*line == L'"') {
        for (++line; *line && *line != L'"'; ++line) {}
        if (*line) ++line;
    } else {
        while (*line && *line != L' ' && *line != L'\t') ++line;
    }
    while (*line == L' ' || *line == L'\t') ++line;
    return line;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR ansiArguments, int show) {
    (void)instance; (void)previous; (void)ansiArguments; (void)show;
    static wchar_t program[32768];
    const wchar_t launcher[] = L"bin\\safehouse.exe";
    DWORD n = GetModuleFileNameW(NULL, program, 32768);
    wchar_t* slash = n && n < 32768 ? wcsrchr(program, L'\\') : NULL;
    if (!slash || (size_t)(slash + 1 - program) + wcslen(launcher) >= 32768) return 1;
    wcscpy(slash + 1, launcher);

    const wchar_t* rest = arguments(GetCommandLineW());
    size_t size = wcslen(program) + wcslen(rest) + 4;
    wchar_t* line = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, size * sizeof(wchar_t));
    if (!line) return 1;
    wcscpy(line, L"\"");
    wcscat(line, program);
    wcscat(line, L"\" ");
    wcscat(line, rest);

    STARTUPINFOW startup = { sizeof startup };
    PROCESS_INFORMATION process;
    if (!CreateProcessW(program, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        MessageBoxW(NULL, L"Could not start bin\\safehouse.exe.\n\nKeep SafeHouse.exe beside the bin folder it came with.",
                    L"SafeHouse", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
