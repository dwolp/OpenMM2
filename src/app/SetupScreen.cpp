// First-run setup: choose where the original game data comes from.
#include "app/GameData.h"
#include "app/IntroScreen.h"
#include "app/Screens.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/StringUtil.h"
#include "platform/Dialogs.h"

#include <imgui.h>

#include <atomic>
#include <future>
#include <thread>

namespace mm2::app {
namespace {

class SetupScreen final : public Screen {
public:
    explicit SetupScreen(Context& ctx) {
        const std::string current = configuredGameSource(ctx.settings);
        if (!current.empty())
            setPath(current);
        m_suggestions = std::async(std::launch::async, [] { return vfs::suggestGameSources(); });
    }

    ~SetupScreen() override {
        m_cancelImport = true;
        if (m_import.valid())
            m_import.wait();
    }

    void update(Context& ctx, double) override {
        pollBackgroundWork(ctx);

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float scale = ctx.window().displayScale();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({std::min(720.0f * scale, vp->Size.x - 20), 0});
        ImGui::Begin("OpenMM2 setup", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings);

        ImGui::TextWrapped("OpenMM2 plays Midtown Madness 2 using the files from your original game. "
                           "Point it at the game disc, a disc image (.iso, .cue/.bin) or an existing "
                           "installation folder. Nothing is installed or modified there.");
        ImGui::Spacing();

        const bool busy = m_import.valid();
        ImGui::BeginDisabled(busy);
        drawSuggestions();
        ImGui::Spacing();
        ImGui::SeparatorText("Location");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##path", "Path to disc image, disc drive or game folder", m_pathBuf,
                                     sizeof(m_pathBuf), ImGuiInputTextFlags_EnterReturnsTrue))
            setPath(m_pathBuf);
        if (ImGui::IsItemDeactivatedAfterEdit())
            setPath(m_pathBuf);
        if (ImGui::Button("Browse for disc image...")) {
            static const platform::FileFilter filters[] = {{"Disc images", "iso;cue;bin;img;mdf"},
                                                           {"All files", "*"}};
            platform::openFileDialog(&ctx.window(), filters, startDir(), [this, alive = std::weak_ptr(m_alive)](auto path) {
                if (path && alive.lock())
                    setPath(str::fromPath(*path));
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse for disc or folder...")) {
            platform::openFolderDialog(&ctx.window(), startDir(), [this, alive = std::weak_ptr(m_alive)](auto path) {
                if (path && alive.lock())
                    setPath(str::fromPath(*path));
            });
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        drawStatus();

        ImGui::Spacing();
        const bool canCopy = m_check && m_check->ok &&
                             m_check->source->kind != vfs::GameSource::Kind::InstallDirectory;
        ImGui::BeginDisabled(busy || !canCopy);
        ImGui::Checkbox("Copy the game data to this computer (about 400 MB)", &m_copy);
        ImGui::EndDisabled();
        if (canCopy) {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Copies the archives to %s\nso the disc or image is not needed to play.",
                                  str::fromPath(defaultImportDir()).c_str());
        }

        if (busy) {
            ImGui::ProgressBar(static_cast<float>(m_progress.load()), {-1, 0}, "Copying game data...");
        }

        ImGui::Spacing();
        ImGui::Separator();
        const bool ready = m_check && m_check->ok && !busy && !m_checking.valid();
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Continue", {140 * scale, 0}))
            confirm(ctx);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Quit", {100 * scale, 0}))
            ctx.quit = true;
        ImGui::End();
    }

    void drawOverlay(Context& ctx) override {
        // Dark backdrop in the 640x480 space, behind the dialog.
        auto& ov = *ctx.overlay;
        ov.begin(ctx.display.uiScale);
        const auto& l = ov.layout();
        ov.rect(l.left, l.top, l.right - l.left, l.bottom - l.top, render::packColor(14, 18, 38));
        ov.rect(l.left, 330, l.right - l.left, 150, render::packColor(40, 24, 60));
        ov.end();
    }

private:
    void setPath(std::string path) {
        if (path == m_checkedPath && (m_check || m_checking.valid()))
            return;
        std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", path.c_str());
        m_checkedPath = path;
        m_check.reset();
        if (path.empty())
            return;
        m_checking = std::async(std::launch::async, [path] { return checkGameSource(str::toPath(path)); });
    }

    std::filesystem::path startDir() const {
        if (const char* home = std::getenv("HOME"))
            return home;
        if (const char* profile = std::getenv("USERPROFILE"))
            return profile;
        return {};
    }

    void pollBackgroundWork(Context& ctx) {
        auto ready = [](auto& f) { return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
        if (ready(m_suggestions)) {
            m_found = m_suggestions.get();
            if (m_checkedPath.empty() && !m_found.empty())
                setPath(str::fromPath(m_found.front()));
        }
        if (ready(m_checking))
            m_check = m_checking.get();
        if (ready(m_import)) {
            std::string error = m_import.get();
            if (error.empty()) {
                finish(ctx, str::fromPath(defaultImportDir()));
            } else {
                m_importError = error;
                log::error("import: {}", error);
            }
        }
    }

    void drawSuggestions() {
        ImGui::SeparatorText("Found on this computer");
        if (m_suggestions.valid()) {
            ImGui::TextDisabled("Searching for discs and disc images...");
            return;
        }
        if (m_found.empty()) {
            ImGui::TextDisabled("Nothing found automatically. Insert the disc or use Browse below.");
            return;
        }
        for (const auto& p : m_found) {
            const std::string label = str::fromPath(p);
            if (ImGui::Selectable(label.c_str(), label == m_checkedPath))
                setPath(label);
        }
    }

    void drawStatus() {
        if (m_checking.valid()) {
            ImGui::TextDisabled("Checking...");
        } else if (m_check) {
            const ImVec4 colour = m_check->ok ? ImVec4(0.5f, 0.9f, 0.5f, 1) : ImVec4(1.0f, 0.55f, 0.45f, 1);
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::TextWrapped("%s", m_check->message.c_str());
            ImGui::PopStyleColor();
        }
        if (!m_importError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1));
            ImGui::TextWrapped("Copy failed: %s", m_importError.c_str());
            ImGui::PopStyleColor();
        }
    }

    void confirm(Context& ctx) {
        if (!m_copy || m_check->source->kind == vfs::GameSource::Kind::InstallDirectory) {
            finish(ctx, m_checkedPath);
            return;
        }
        m_importError.clear();
        m_progress = 0.0;
        m_cancelImport = false;
        m_import = std::async(std::launch::async, [this, source = *m_check->source]() -> std::string {
            std::string error;
            const bool ok = importGameData(source, defaultImportDir(),
                                           [this](double p) {
                                               m_progress = p;
                                               return !m_cancelImport.load();
                                           },
                                           &error);
            return ok ? std::string() : error;
        });
    }

    void finish(Context& ctx, const std::string& path) {
        const auto check = checkGameSource(str::toPath(path));
        std::string error;
        if (!check.ok || !(ctx.game = loadGameData(*check.source, &error))) {
            m_importError = check.ok ? error : check.message;
            return;
        }
        ctx.settings.gameSource = path;
        mountConfiguredMm1Source(*ctx.game, ctx.settings); // OpenMM2 Enhanced: MM1's cities
        ctx.saveSettings();
        log::info("setup: using {}", check.source->describe());
        ctx.nextScreen = makeIntroScreen(ctx);
    }

    // Dialog callbacks may arrive after this screen is gone; they hold a weak
    // reference to this token and do nothing once it has expired.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    char m_pathBuf[1024] = {};
    std::string m_checkedPath;
    std::future<std::vector<std::filesystem::path>> m_suggestions;
    std::vector<std::filesystem::path> m_found;
    std::future<SourceCheck> m_checking;
    std::optional<SourceCheck> m_check;
    bool m_copy = true;
    std::future<std::string> m_import;
    std::atomic<double> m_progress{0.0};
    std::atomic<bool> m_cancelImport{false};
    std::string m_importError;
};

} // namespace

std::unique_ptr<Screen> makeSetupScreen(Context& ctx) { return std::make_unique<SetupScreen>(ctx); }

} // namespace mm2::app
