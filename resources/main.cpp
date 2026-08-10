#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <wrl.h>

#include "WebView2.h"
#include <nlohmann/json.hpp>
#include "../resources.h" // Kök dizindeki kaynak kimlikleri başlık dosyası

using namespace Microsoft::WRL;
namespace fs = std::filesystem;
using json = nlohmann::json;

static ComPtr<ICoreWebView2Controller> webviewController;
static ComPtr<ICoreWebView2> webview;

// .exe dosyasının çalıştığı dizini dinamik alma
fs::path GetExeDirectory() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    return fs::path(buffer).parent_path();
}

// Runtime çalışma klasörü
const fs::path EXE_DIR      = GetExeDirectory();
const fs::path PROJECTS_DIR = EXE_DIR / "Projects";

// String Dönüştürücü Yardımcı Fonksiyonlar
std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// EXE İçine Gömülen HTML Dosyasını Bellekten Okuma
std::string LoadEmbeddedHTML(int resourceId) {
    HMODULE hModule = GetModuleHandle(NULL);
    HRSRC hRes = FindResource(hModule, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    if (!hRes) return "";

    HGLOBAL hData = LoadResource(hModule, hRes);
    if (!hData) return "";

    DWORD size = SizeofResource(hModule, hRes);
    char* pData = static_cast<char*>(LockResource(hData));

    return std::string(pData, size);
}

// Proje Klasör Kurulumu
void InitializeEnvironment() {
    fs::create_directories(PROJECTS_DIR);

    fs::path defaultFile = PROJECTS_DIR / "main.cpp";
    if (!fs::exists(defaultFile)) {
        std::ofstream sample(defaultFile);
        sample << "#include <iostream>\n\nint main() {\n    std::cout << \"Autrited Studio Ready!\";\n    return 0;\n}\n";
    }
}

// Rekürsif Klasör/Dosya Tarama
void ScanDirectoryRecursive(const fs::path& dirPath, json& structureList, int depth = 1) {
    if (!fs::exists(dirPath)) return;

    for (const auto& entry : fs::directory_iterator(dirPath)) {
        json item;
        item["name"] = entry.path().filename().string();
        item["relative_path"] = fs::relative(entry.path(), PROJECTS_DIR).string();
        item["is_directory"] = entry.is_directory();
        item["depth"] = depth;

        structureList.push_back(item);

        if (entry.is_directory()) {
            ScanDirectoryRecursive(entry.path(), structureList, depth + 1);
        }
    }
}

// JavaScript İletişim Mesaj İşleyici
std::string HandleIncomingWebMessage(const std::string& jsonRequest) {
    json response;
    try {
        auto req = json::parse(jsonRequest);
        std::string action = req["action"];

        if (action == "get_workspace_structure") {
            json structureList = json::array();
            ScanDirectoryRecursive(PROJECTS_DIR, structureList);
            response["type"] = "workspace_structure";
            response["structure"] = structureList;
        }
        else if (action == "create_directory") {
            std::string folderName = req["folderName"];
            fs::path newFolderPath = PROJECTS_DIR / folderName;

            if (fs::create_directories(newFolderPath)) {
                response["type"] = "status";
                response["message"] = "Klasör oluşturuldu.";
            } else {
                response["type"] = "status";
                response["message"] = "Klasör oluşturulamadı!";
            }
        }
        else if (action == "create_file") {
            std::string filename = req["filename"];
            fs::path filePath = PROJECTS_DIR / filename;

            fs::create_directories(filePath.parent_path());
            std::ofstream file(filePath);
            file << "// Autrited Studio - " << filename << "\n";
            file.close();

            response["type"] = "status";
            response["message"] = "Dosya oluşturuldu.";
        }
        else if (action == "read_file") {
            std::string relativePath = req["relativePath"];
            fs::path fullPath = PROJECTS_DIR / relativePath;

            std::ifstream file(fullPath);
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());

            response["type"] = "file_content";
            response["filename"] = fullPath.filename().string();
            response["path"] = fullPath.string();
            response["content"] = content;
        }
        else if (action == "save_file") {
            std::string path = req["path"];
            std::string content = req["content"];

            std::ofstream file(path);
            file << content;
            file.close();

            response["type"] = "status";
            response["message"] = "Kaydedildi.";
        }
    } 
    catch (const std::exception& e) {
        response["type"] = "status";
        response["message"] = std::string("Hata: ") + e.what();
    }

    return response.dump();
}

// Pencere Boyutlandırma ve Kapatma İşleyicisi
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (webviewController != nullptr) {
            RECT bounds;
            GetClientRect(hWnd, &bounds);
            webviewController->put_Bounds(bounds);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Ana Giriş Noktası
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    InitializeEnvironment();

    // Pencere Sınıfı Tanımlama
    WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));   // Ana Masaüstü/Alt+Tab Logosu
    wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON)); // Sol Üst Başlık/Görev Çubuğu Logosu
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.lpszClassName = L"AutritedStudioClass";
    RegisterClassEx(&wcex);

    // Ana Uygulama Penceresi Oluşturma
    HWND hWnd = CreateWindow(
        L"AutritedStudioClass", L"Autrited Studio v2.0",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        NULL, NULL, hInstance, NULL
    );

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // WebView2 Motorunu Başlatma
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                env->CreateCoreWebView2Controller(hWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [hWnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                        if (controller != nullptr) {
                            webviewController = controller;
                            webviewController->get_CoreWebView2(&webview);
                        }

                        RECT bounds;
                        GetClientRect(hWnd, &bounds);
                        webviewController->put_Bounds(bounds);

                        EventRegistrationToken token;
                        webview->add_WebMessageReceived(
                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [](ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    PWSTR jsonRaw;
                                    args->get_WebMessageAsJson(&jsonRaw);
                                    
                                    std::string requestUtf8 = WideToUtf8(jsonRaw);
                                    CoTaskMemFree(jsonRaw);

                                    std::string responseUtf8 = HandleIncomingWebMessage(requestUtf8);
                                    webview->PostWebMessageAsJson(Utf8ToWide(responseUtf8).c_str());
                                    return S_OK;
                                }).Get(), &token);

                        // Gömülü html içeriğini yükleme
                        std::string htmlContent = LoadEmbeddedHTML(IDR_INDEX_HTML);
                        webview->NavigateToString(Utf8ToWide(htmlContent).c_str());

                        return S_OK;
                    }).Get());
                return S_OK;
            }).Get());

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}