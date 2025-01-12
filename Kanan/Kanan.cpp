#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <Config.hpp>
#include <String.hpp>
#include <Utility.hpp>

#include "FontData.hpp"
#include "Log.hpp"
#include "Kanan.hpp"
#include "../Kanan/metrics_gui/metrics_gui.h"

using namespace std;

IMGUI_IMPL_API LRESULT  ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace kanan {
    unique_ptr<Kanan> g_kanan{ nullptr };

    //metrics
    MetricsGuiMetric frameTimeMetric("Frame time", "s", MetricsGuiMetric::USE_SI_UNIT_PREFIX);

    MetricsGuiPlot frameTimePlot;


    Kanan::Kanan(string path)
        : m_path{ move(path) },
        m_uiConfigPath{ m_path + "/ui.ini" },
        m_d3d9Hook{ nullptr },
        m_dinputHook{ nullptr },
        m_wmHook{ nullptr },
        m_game{ nullptr },
        m_mods{ m_path },
        m_isUIOpen{ true },
        m_isLogOpen{ false },
        m_isAboutOpen{ false },
        m_isInitialized{ false },
        m_areModsReady{ false },
        m_areModsLoaded{ false },
        m_wnd{ nullptr },
        m_isUIOpenByDefault{ true }
    {
        log("Entering Kanan constructor.");

        //
        // Hook D3D9 and set the callbacks.
        //
        log("Hooking D3D9...");

        m_d3d9Hook = make_unique<D3D9Hook>();

        m_d3d9Hook->onPresent = [this](auto&) { onFrame(); };
        m_d3d9Hook->onPreReset = [](auto&) { ImGui_ImplDX9_InvalidateDeviceObjects(); };
        m_d3d9Hook->onPostReset = [](auto&) { ImGui_ImplDX9_CreateDeviceObjects(); };

        if (!m_d3d9Hook->isValid()) {
            error("Failed to hook D3D9.");
        }

        //
        // We initialize mods now because this constructor is still being executed
        // from the startup thread so we can take as long as necessary to do so here.
        //
        initializeMods();


        //metrics settings

        frameTimeMetric.mSelected = true;

        frameTimePlot.mBarRounding = 0.f;    // amount of rounding on bars
        frameTimePlot.mRangeDampening = 0.95f;  // weight of historic range on axis range [0,1]
        frameTimePlot.mInlinePlotRowCount = 2;      // height of DrawList() inline plots, in text rows
        frameTimePlot.mPlotRowCount = 5;      // height of DrawHistory() plots, in text rows
        frameTimePlot.mVBarMinWidth = 6;      // min width of bar graph bar in pixels
        frameTimePlot.mVBarGapWidth = 1;      // width of bar graph inter-bar gap in pixels
        frameTimePlot.mShowAverage = true;  // draw horizontal line at series average
        frameTimePlot.mShowInlineGraphs = false;  // show history plot in DrawList()
        frameTimePlot.mShowOnlyIfSelected = false;  // draw show selected metrics
        frameTimePlot.mShowLegendDesc = true;   // show series description in legend
        frameTimePlot.mShowLegendColor = true;   // use series color in legend
        frameTimePlot.mShowLegendUnits = true;   // show units in legend values
        frameTimePlot.mShowLegendAverage = false;  // show series average in legend
        frameTimePlot.mShowLegendMin = true;   // show plot y-axis minimum in legend
        frameTimePlot.mShowLegendMax = true;   // show plot y-axis maximum in legend
        frameTimePlot.mBarGraph = true;  // use bars to draw history
        frameTimePlot.mStacked = true;  // stack series when drawing history
        frameTimePlot.mSharedAxis = false;  // use first series' axis range
        frameTimePlot.mFilterHistory = true;   // allow single plot point to represent more than on history value

        frameTimePlot.AddMetric(&frameTimeMetric);

        log("Leaving Kanan constructor.");
    }

    void Kanan::initializeMods() {
        if (m_areModsReady) {
            return;
        }

        //
        // Initialize the Game object.
        //
        log("Creating the Game object...");

        m_game = make_unique<Game>();

        //
        // Initialize all the mods.
        //
        log("Loading mods...");

        m_mods.loadMods();

        log("Done initializing.");

        m_areModsReady = true;
    }

    void Kanan::onInitialize() {
        if (m_isInitialized) {
            return;
        }

        log("Beginning intialization... ");

        // Grab the HWND from the device's creation parameters.
        log("Getting window from D3D9 device...");

        auto device = m_d3d9Hook->getDevice();
        D3DDEVICE_CREATION_PARAMETERS creationParams{};

        device->GetCreationParameters(&creationParams);

        m_wnd = creationParams.hFocusWindow;

        //
        // ImGui.
        //
        log("Initializing ImGui...");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        auto& io = ImGui::GetIO();

        io.IniFilename = m_uiConfigPath.c_str();
        io.Fonts->AddFontFromMemoryCompressedTTF(g_font_compressed_data, g_font_compressed_size, 16.0f);
        ImGuiFreeType::BuildFontAtlas(io.Fonts, 0);

        if (!ImGui_ImplWin32_Init(m_wnd)) {
            error("Failed to initialize ImGui.");
        }

        if (!ImGui_ImplDX9_Init(device)) {
            error("Failed to initialize ImGui.");
        }

        ImGui::StyleColorsDark();

        //
        // DInputHook.
        //
        log("Hooking DInput...");

        m_dinputHook = make_unique<DInputHook>(m_wnd);

        m_dinputHook->onKeyDown = [this](DInputHook& dinput, DWORD key) {
            for (auto&& mod : m_mods.getMods()) {
                mod->onKeyDown(key);
            }
        };
        m_dinputHook->onKeyUp = [this](DInputHook& dinput, DWORD key) {
            for (auto&& mod : m_mods.getMods()) {
                mod->onKeyUp(key);
            }
        };

        if (!m_dinputHook->isValid()) {
            error("Failed to hook DInput.");
        }

        //
        // WindowsMessageHook.
        //
        log("Hooking the windows message procedure...");

        m_wmHook = make_unique<WindowsMessageHook>(m_wnd);

        m_wmHook->onMessage = [this](auto wnd, auto msg, auto wParam, auto lParam) {
            return onMessage(wnd, msg, wParam, lParam);
        };

        if (!m_wmHook->isValid()) {
            error("Failed to hook windows message procedure.");
        }

        //
        // Time critical mods.
        //
        log("Loading time critical mods...");

        m_mods.loadTimeCriticalMods();

        m_isInitialized = true;
    }

    void Kanan::onFrame() {
        if (!m_isInitialized) {
            onInitialize();
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();


        //update our metrics with framerate data
        frameTimeMetric.AddNewValue(1.f / ImGui::GetIO().Framerate);
        frameTimePlot.UpdateAxes();

        if (m_areModsReady) {
            // Make sure the config for all the mods gets loaded.
            if (!m_areModsLoaded) {
                loadConfig();
            }

            for (const auto& mod : m_mods.getMods()) {
                mod->onFrame();
            }

            if (wasKeyPressed(VK_INSERT)) {
                m_isUIOpen = !m_isUIOpen;

                // Save the config whenever the menu closes.
                if (!m_isUIOpen) {
                    saveConfig();
                }
            }

            if (m_isUIOpen) {
                // Block input if the user is interacting with the UI.
                auto& io = ImGui::GetIO();

                if (io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput) {
                    m_dinputHook->ignoreInput();
                }
                else {
                    m_dinputHook->acknowledgeInput();
                }

                drawUI();

                if (m_isLogOpen) {
                    drawLog(&m_isLogOpen);
                }

                if (m_isAboutOpen) {
                    drawAbout();
                }
            }
            else {
                // UI is closed so always pass input to the game.
                m_dinputHook->acknowledgeInput();
            }

            //draw metric window
            if (m_ismetricsopen) {
                Drawmetrics();
            }


        }
        else {
            ImGui::OpenPopup("Loading...");
            ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2}, ImGuiCond_Always, ImVec2{0.5f, 0.5f});
            ImGui::SetNextWindowSize(ImVec2{ 450.0f, 200.0f });

            if (ImGui::BeginPopupModal("Loading...", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
                ImGui::TextWrapped("Kanan is currently setting things up. Please wait a moment...");
                ImGui::EndPopup();
            }

            auto& io = ImGui::GetIO();

            if (io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput) {
                m_dinputHook->ignoreInput();
            }
            else {
                m_dinputHook->acknowledgeInput();
            }
        }

        ImGui::EndFrame();






        // This fixes mabi's Film Style Post Shader making ImGui render as a black box.
        auto device = m_d3d9Hook->getDevice();

        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    bool Kanan::onMessage(HWND wnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (m_isUIOpen) {
            if (ImGui_ImplWin32_WndProcHandler(wnd, message, wParam, lParam) != 0) {
                // If the user is interacting with the UI we block the message from going to the game.
                auto& io = ImGui::GetIO();

                if (io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput) {
                    return false;
                }
            }
        }

        for (auto& mod : m_mods.getMods()) {
            if (!mod->onMessage(wnd, message, wParam, lParam)) {
                return false;
            }
        }

        return true;
    }

    void Kanan::loadConfig() {
        log("Loading config %s/config.txt", m_path.c_str());

        Config cfg{ m_path + "/config.txt" };

        m_isUIOpenByDefault = cfg.get<bool>("UI.OpenByDefault").value_or(true);
        m_isUIOpen = m_isUIOpenByDefault;

        for (auto& mod : m_mods.getMods()) {
            mod->onConfigLoad(cfg);
        }

        // Patch mods.
        for (auto& mods : m_mods.getPatchMods()) {
            for (auto& mod : mods.second) {
                mod->onConfigLoad(cfg);
            }
        }

        log("Config loading done.");

        m_areModsLoaded = true;
    }

    void Kanan::saveConfig() {
        log("Saving config %s/config.txt", m_path.c_str());

        Config cfg{};

        cfg.set<bool>("UI.OpenByDefault", m_isUIOpenByDefault);

        for (auto& mod : m_mods.getMods()) {
            mod->onConfigSave(cfg);
        }

        // Patch mods.
        for (auto& mods : m_mods.getPatchMods()) {
            for (auto& mod : mods.second) {
                mod->onConfigSave(cfg);
            }
        }

        if (!cfg.save(m_path + "/config.txt")) {
            log("Failed to save the config %s/config.txt", m_path.c_str());
        }

        log("Config saving done.");
    }

    void Kanan::drawUI() {
        ImGui::SetNextWindowSize(ImVec2{ 450.0f, 200.0f }, ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("Kanan's New Mabinogi Mod", &m_isUIOpen, ImGuiWindowFlags_MenuBar)) {
            ImGui::End();
            return;
        }

        //
        // Menu bar
        //
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save Config")) {
                    saveConfig();
                }
                if (ImGui::MenuItem("Force close Game")) {
                    ExitProcess(0);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Show Log", nullptr, &m_isLogOpen);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Settings")) {
                ImGui::MenuItem("UI Open By Default", nullptr, &m_isUIOpenByDefault);
                ImGui::MenuItem("Metrics", nullptr, &m_ismetricsopen);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help")) {
                ImGui::MenuItem("About Kanan", nullptr, &m_isAboutOpen);
                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        // 
        // Rest of the UI
        //
        ImGui::TextWrapped(
            "Input to the game is blocked while interacting with this UI. "
            "Press the INSERT key to toggle this UI. "
            "Configuration is saved every time the INSERT key is used to close the UI. "
            "You can also save the configuration by using File->Save Config. "
        );
        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Patches")) {
            for (auto& mod : m_mods.getMods()) {
                mod->onPatchUI();
            }

            // Patch mods.
            for (auto& mods : m_mods.getPatchMods()) {
                auto& category = mods.first;

                if (!category.empty() && !ImGui::TreeNode(category.c_str())) {
                    continue;
                }

                for (auto& mod : mods.second) {
                    mod->onPatchUI();
                }

                if (!category.empty()) {
                    ImGui::TreePop();
                }
            }
        }

        for (const auto& mod : m_mods.getMods()) {
            mod->onUI();
        }

        ImGui::End();
    }

    void Kanan::drawAbout() {
        ImGui::SetNextWindowSize(ImVec2{ 475.0f, 275.0f }, ImGuiCond_Appearing);

        if (!ImGui::Begin("About", &m_isAboutOpen)) {
            ImGui::End();
            return;
        }

        ImGui::Text("Kanan's New Mabinogi Mod");
        ImGui::Text("https://github.com/cursey/kanan-new");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Please come by the repository and let us know if there are "
            "any problems or mods you would like to see added. Contributors "
            "are always welcome!"
        );
        ImGui::Spacing();
        ImGui::Text("Kanan uses the following third-party libraries");
        ImGui::Text("    Dear ImGui (https://github.com/ocornut/imgui)");
        ImGui::Text("    FreeType (https://www.freetype.org/)");
        ImGui::Text("    JSON for Modern C++ (https://github.com/nlohmann/json)");
        ImGui::Text("    MinHook (https://github.com/TsudaKageyu/minhook)");
        ImGui::Text("    Roboto Font (https://fonts.google.com/specimen/Roboto)");
        ImGui::Text("    Metrics GUI (https://github.com/GameTechDev/MetricsGui)");

        ImGui::End();
    }



    void Kanan::Drawmetrics() {

        //different style for if kanan is open or if kanan is closed. if metric is showing is true it will always render on screen.
        ImGui::SetNextWindowSize(ImVec2{ 619.0f, 186.0f }, ImGuiCond_FirstUseEver);
        if (m_isUIOpen) {
            if (!ImGui::Begin("Metrics display", &m_ismetricsopen)) {
                ImGui::End();
                return;
            }
        }
        else {
            if (!ImGui::Begin("Metrics display", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {
                ImGui::End();
                return;
            }
        }


        frameTimePlot.DrawList();
        frameTimePlot.DrawHistory();



        ImGui::End();
    }
}
