#pragma once

// OpenMM2 Enhanced (not in MM2): a Midtown Madness 1 city's animated
// objects (city/<city>/anim.csv): the El train and the airliner.
//
// After MM1's mmAnimMgr / mmAnimSpline, mmAnimTrain / mmAnimTrainCar and
// mmAnimPlane (Open1560's game.asm; GPL-3.0). mmAnimSpline::Init starts an
// object by its file's name: anim_eltrain.csv a train, anim_plane.csv a
// plane, anim_boat.csv a boat (InitBoat does nothing in the release, so
// there are no boats; anim_sailboat.csv matches no name). Each file is a
// list of "x,y,z" points.
//
// The train: six r_L_train cars on a closed Hermite curve through the
// points (TrainHermite; segment i's tangents unit(p[i+1] - p[i-1]) and
// unit(p[i+2] - p[i]) times the segment's length), lapping in 65 seconds
// (SetPathSpeed: the loop's length / 65), each car one box length (its DLP's
// H group) behind the one before, facing along the curve.
//
// The plane: vaboeing on a uniform cubic B-spline through the points
// (CatmullBasis, despite its name), lapping in 60 seconds, its nose along the
// curve's direction (it pitches), 10 m above it (PlaneFudge).
//
// Not ported: the plane's sound (MM2 has none like it; the train uses MM2's
// subway sound), the UFO and airliner swaps (a cheat), collisions (MM1's
// animated objects are mmMatrixInstances the cars do not hit).

#include "audio/game/Ambience.h"
#include "city/Mm1City.h"
#include "core/Math.h"
#include "vfs/Vfs.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::game::world {

// mmAnimSpline::LoadFile: one "x,y,z" point per line.
std::vector<Vec3> parseMm1AnimPath(std::string_view text);

// One segment's cubic per axis: x(t) = ((a t + b) t + c) t + d.
struct Mm1Cubic {
    std::array<Vec4, 3> axes{}; // x, y, z; (a, b, c, d) each
    Vec3 position(float t) const;
    Vec3 direction(float t) const; // the derivative
};

// mmAnimTrain::CalculateCubics: segment i of the closed loop as a Hermite.
Mm1Cubic mm1TrainCubic(const std::vector<Vec3>& points, const std::vector<float>& lengths, std::size_t i);
// mmAnimPlane::CalculateCubics: segment i as a uniform cubic B-spline.
Mm1Cubic mm1PlaneCubic(const std::vector<Vec3>& points, std::size_t i);
// The frame mmAnimTrain / mmAnimPlane::UpdatePathPosition build: z = -unit
// of the direction, x = Y x z (not normalised), y = z x x.
Mat34 mm1AnimFrame(const Vec3& position, const Vec3& direction);

class Mm1Anim {
public:
    // mmAnimTrainCar (its share of mmAnimTrain's update).
    struct TrainCar {
        int startSegment = 0;
        float startT = 0.0f;
        int segment = 0;
        float t = 0.0f;
        Mat34 matrix;
    };
    struct Train {
        std::string model; // r_l_train
        std::vector<Vec3> points;
        std::vector<float> lengths; // segment i: point i to i + 1 (mod count)
        float total = 0.0f;
        float speed = 0.0f;         // metres per second (SetPathSpeed)
        std::vector<Mm1Cubic> cubics;
        std::vector<TrainCar> cars;
    };
    struct Plane {
        std::string model; // vaboeing
        std::vector<Vec3> points;
        std::vector<float> lengths; // count - 1 segments
        float total = 0.0f;
        float speed = 0.0f;
        std::vector<Mm1Cubic> cubics;
        int segment = 0;
        float t = 0.0f;
        Mat34 matrix;
    };

    // mmAnimMgr::Init: city/<city>/anim.csv names the files.
    static std::unique_ptr<Mm1Anim> load(const city::Mm1City& city, const vfs::Vfs& vfs);

    void reset();
    void update(float dt);

    // The El's sound (MM2's "subwaycar", aiSubwayAudio, as a stand-in for
    // MM1's "THEL") at the fourth car, as mmAnimTrain plays THEL there.
    void loadAudio(const vfs::Vfs& vfs, audio::SoundBank& bank, audio::Mixer& mixer,
                   audio::game::Object3DManager* manager);
    void updateAudio(const Mat34& listener, float dt, bool inTunnel);
    void stopAudio();
    ~Mm1Anim();

    const std::optional<Train>& train() const { return m_train; }
    const std::optional<Plane>& plane() const { return m_plane; }

    // Exposed for tests.
    static Train makeTrain(std::vector<Vec3> points, const std::string& model, int cars, float carSpacing);
    static Plane makePlane(std::vector<Vec3> points, const std::string& model);
    static void updateTrain(Train& t, float dt);
    static void updatePlane(Plane& p, float dt);

    static constexpr int kTrainCars = 6;        // mmAnimTrain +0xa0
    static constexpr float kTrainLapSeconds = 65.0f; // mmAnimTrain +0x40
    static constexpr float kPlaneLapSeconds = 60.0f; // mmAnimPlane +0x54
    static constexpr int kTrainSoundCar = 3;    // mmAnimTrain +0xa8

private:
    std::optional<Train> m_train;
    std::optional<Plane> m_plane;
    std::unique_ptr<audio::game::SubwayAudio> m_trainAudio;
};

} // namespace mm2::game::world
