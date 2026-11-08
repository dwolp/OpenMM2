#pragma once

#include "render/DisplaySettings.h"
#include "render/Types.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace mm2::platform {
class Window;
}

namespace mm2::render {

struct FrameStats {
    std::uint32_t drawCalls = 0;
    std::uint32_t pipelineBinds = 0;
    std::uint64_t primitives = 0;
    std::uint64_t transientBytes = 0;
    std::uint32_t apiErrors = 0; // cumulative validation-layer / GL debug errors since creation
};

struct DeviceCreateInfo {
    platform::Window* window = nullptr; // must outlive the device
    DisplaySettings settings;
    // Vulkan pipeline cache file (loaded at startup, saved on destruction).
    std::filesystem::path pipelineCachePath;
};

// A rendering device bound to one window.
//
// Frame structure:
//
//   if (device.beginFrame()) {           // false while minimised
//       device.beginScene(clear);         // 3D: offscreen, MSAA, render scale
//       device.setFrameConstants(camera); device.draw(...); ...
//       device.endScene();
//       device.beginOverlay(clearColor);  // composites the scene to the window
//       device.setFrameConstants(overlayConstants(device.outputExtent()));
//       device.draw(...);                 // UI/HUD/ImGui at native resolution
//       device.endOverlay();
//       device.endFrame();                // present
//   }
//
// The scene pass is optional (menus can draw only the overlay). Inside a
// pass, setViewport()/setScissor()/clear() work on sub-rectangles (e.g. the
// rear-view mirror). Resources may be created at any time and are usable by
// the next draw. Updating a texture or buffer that was already drawn with
// earlier in the same frame is undefined (Vulkan applies uploads before the
// frame's draws, OpenGL in call order): update first, then draw, or use
// uploadTransient() for per-frame data.
class Device {
public:
    virtual ~Device() = default;

    virtual const DeviceInfo& info() const = 0;
    Backend backend() const { return info().backend; }

    // --- Resources ---
    virtual TextureHandle createTexture(const TextureDesc& desc, std::span<const TextureData> mips = {}) = 0;
    // Replaces a rectangle of one mip level. rowPitch 0 = region.width * 4.
    virtual void updateTexture(TextureHandle texture, std::uint32_t mip, const Rect& region, const void* data,
                               std::uint32_t rowPitch = 0) = 0;
    virtual void destroyTexture(TextureHandle texture) = 0;

    // GPU-resident buffer for geometry that persists across frames.
    virtual BufferHandle createBuffer(BufferKind kind, std::size_t size, const void* data = nullptr) = 0;
    virtual void updateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) = 0;
    virtual void destroyBuffer(BufferHandle buffer) = 0;

    // Copies data into per-frame memory (UI, particles, debug lines). The
    // slice is valid until endFrame(). Only call between beginFrame/endFrame.
    virtual BufferSlice uploadTransient(BufferKind kind, std::span<const std::byte> data) = 0;
    template <class T>
    BufferSlice uploadTransient(BufferKind kind, std::span<const T> items) {
        return uploadTransient(kind, std::as_bytes(items));
    }

    // --- Configuration ---
    // Applies vsync, MSAA, anisotropy and render scale (recreating targets if
    // needed). Window mode/size changes are handled by the window; call
    // notifyResized() afterwards (resizes are also detected automatically).
    virtual void applySettings(const DisplaySettings& settings) = 0;
    virtual void notifyResized() = 0;

    // --- Frame ---
    virtual bool beginFrame() = 0;
    virtual Extent2D outputExtent() const = 0; // window drawable, pixels
    virtual Extent2D sceneExtent() const = 0;  // 3D target (render scale applied)

    virtual void beginScene(const ClearValues& clear) = 0;
    virtual void endScene() = 0;
    virtual void beginOverlay(const Vec4& clearColor) = 0;
    virtual void endOverlay() = 0;
    virtual void endFrame() = 0;

    // --- Pass commands (pixel coordinates of the current pass target) ---
    virtual void setViewport(const Viewport& viewport) = 0;
    virtual void setScissor(const Rect* rect) = 0; // nullptr disables
    virtual void clear(const ClearValues& values) = 0; // inside the current viewport
    virtual void setFrameConstants(const FrameConstants& constants) = 0;
    virtual void draw(const DrawCall& call) = 0;
    // Swaps every following draw's front-face winding (a view through a
    // mirrored camera frame, such as the rear-view mirror, whose
    // mmMirror::Cull swaps the cull mode) until turned off again.
    void setFrontFaceFlipped(bool flipped) { m_frontFaceFlipped = flipped; }
    bool frontFaceFlipped() const { return m_frontFaceFlipped; }

    // --- Readback ---
    // Captures the window image at the end of the current frame (or the next
    // one if called outside a frame). readCapture() waits for the GPU and
    // returns the RGBA8 image (top row first).
    virtual void requestCapture() = 0;
    virtual bool readCapture(Image& out) = 0;

    virtual void waitIdle() = 0;
    virtual const FrameStats& stats() const = 0;

    // OpenMM2 image filters for the scene's composite (beginOverlay), from
    // the next composite on. The default filters nothing.
    void setPostProcess(const PostProcess& post) { m_post = post; }
    const PostProcess& postProcess() const { return m_post; }

protected:
    // The state a backend draws with: the call's, with the winding swapped
    // while setFrontFaceFlipped(true) is in force.
    PipelineState effectiveState(const PipelineState& state) const {
        PipelineState s = state;
        if (m_frontFaceFlipped)
            s.frontFace =
                s.frontFace == FrontFace::CounterClockwise ? FrontFace::Clockwise : FrontFace::CounterClockwise;
        return s;
    }

private:
    bool m_frontFaceFlipped = false;
    PostProcess m_post;
};

// Frame constants for overlay drawing in output pixels (origin top-left).
FrameConstants overlayConstants(Extent2D output);

#ifdef OPENMM2_HAS_VULKAN
std::unique_ptr<Device> createVulkanDevice(const DeviceCreateInfo& info, std::string* error);
#endif
#ifdef OPENMM2_HAS_OPENGL
std::unique_ptr<Device> createOpenGLDevice(const DeviceCreateInfo& info, std::string* error);
#endif

} // namespace mm2::render
