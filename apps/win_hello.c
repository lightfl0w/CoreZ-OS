typedef unsigned int DWORD;
typedef void *HANDLE;

__declspec(dllimport) HANDLE GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) int WriteFile(HANDLE hFile, const void *buf,
                                    DWORD count, DWORD *written, void *ov);
__declspec(dllimport) void ExitProcess(DWORD code);

#define STD_OUTPUT_HANDLE ((DWORD)-11)

void entry(void) {
    static const char msg[] = "Hello, World!\n";
    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(out, msg, sizeof(msg) - 1, &written, 0);
    ExitProcess(0);
}