#include "vfs/Mm1Source.h"

#include "core/Log.h"
#include "core/StringUtil.h"
#include "vfs/AresArchive.h"
#include "vfs/DirectoryFs.h"
#include "vfs/IsoImage.h"
#include "vfs/PrefixFs.h"

#include <format>
#include <system_error>

namespace mm2::vfs {
namespace {

// Below MM2's archives (priority 0) and any add-ons.
constexpr int kMm1Priority = -100;

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

} // namespace

bool mountMm1Source(Vfs& vfs, const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    std::shared_ptr<const RandomAccessFile> core;
    std::string label;
    // UI.AR (menus' pictures: the city maps) beside CORE.AR, when there is one.
    std::shared_ptr<const RandomAccessFile> ui;
    // AUDIO.AR (the sounds: aud/aud22/tireskid1.22k.wav ...) likewise.
    std::shared_ptr<const RandomAccessFile> audio;
    if (std::filesystem::is_directory(path, ec)) {
        DirectoryFs dir(path, 1);
        core = dir.open(std::string_view("core.ar"));
        ui = dir.open(std::string_view("ui.ar"));
        audio = dir.open(std::string_view("audio.ar"));
        label = str::fromPath(path / "core.ar");
    } else if (str::iendsWith(str::fromPath(path.filename()), ".ar")) {
        core = OsFile::open(path);
        label = str::fromPath(path);
    } else {
        std::string err;
        auto iso = IsoImage::open(path, &err);
        if (!iso) {
            setError(error, std::format("{}: {}", str::fromPath(path), err));
            return false;
        }
        core = iso->open(std::string_view("core.ar"));
        ui = iso->open(std::string_view("ui.ar"));
        audio = iso->open(std::string_view("audio.ar"));
        label = str::fromPath(path) + ":core.ar";
    }
    if (!core) {
        setError(error, std::format("no CORE.AR in {}", str::fromPath(path)));
        return false;
    }
    std::string err;
    auto ares = AresArchive::open(std::move(core), label, &err);
    if (!ares) {
        setError(error, std::format("{}: {}", label, err));
        return false;
    }
    log::info("mm1: {} ({} files) as {}", label, ares->entries().size(), kMm1Prefix);
    vfs.mount(std::make_shared<PrefixFs>(std::move(ares), std::string(kMm1Prefix)), kMm1Priority);
    if (ui) {
        if (auto uiAres = AresArchive::open(std::move(ui), label + " (ui.ar)", &err))
            vfs.mount(std::make_shared<PrefixFs>(std::move(uiAres), std::string(kMm1Prefix)), kMm1Priority);
        else
            log::warn("mm1: ui.ar: {}", err);
    }
    if (audio) {
        if (auto audioAres = AresArchive::open(std::move(audio), label + " (audio.ar)", &err))
            vfs.mount(std::make_shared<PrefixFs>(std::move(audioAres), std::string(kMm1Prefix)), kMm1Priority);
        else
            log::warn("mm1: audio.ar: {}", err);
    }
    return true;
}

} // namespace mm2::vfs
