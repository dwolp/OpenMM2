#include "phys/vehicle/Mm1CarParams.h"

#include "core/StringUtil.h"

#include <algorithm>
#include <format>

namespace mm2::phys {

namespace {

// The file's structure: blocks "Name [Class :address] {", lists
// "Name [" ... "]" and fields "Name v0 v1 ...".
struct Node {
    std::string name;
    std::string className;
    std::vector<float> values;
    std::vector<Node> children;
    bool block = false;
};

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
            ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            ++i;
        if (i > start)
            out.push_back(line.substr(start, i - start));
    }
    return out;
}

void addNumbers(Node& n, const std::vector<std::string_view>& w, std::size_t from) {
    for (std::size_t i = from; i < w.size(); ++i)
        if (auto f = str::parseDouble(w[i]))
            n.values.push_back(static_cast<float>(*f));
}

Node parseTree(std::string_view text) {
    Node root;
    root.block = true;
    std::vector<Node*> stack{&root};
    Node* list = nullptr; // the list being read, if any
    for (std::string_view rest = text; !rest.empty();) {
        const auto nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        const auto w = words(line);
        if (w.empty() || w[0][0] == '#')
            continue;
        if (list) {
            if (w[0] == "]") {
                list = nullptr;
                continue;
            }
            addNumbers(*list, w, 0);
            if (w.back() == "]")
                list = nullptr;
            continue;
        }
        if (w[0] == "}") {
            if (stack.size() > 1)
                stack.pop_back();
            continue;
        }
        Node n;
        n.name = std::string(w[0]);
        if (w.back() == "{") {
            n.block = true;
            if (w.size() >= 4)
                n.className = std::string(w[1]);
            stack.back()->children.push_back(std::move(n));
            stack.push_back(&stack.back()->children.back());
            continue;
        }
        if (w.size() >= 2 && w[1] == "[") {
            addNumbers(n, w, 2);
            stack.back()->children.push_back(std::move(n));
            list = &stack.back()->children.back();
            if (w.back() == "]")
                list = nullptr;
            continue;
        }
        addNumbers(n, w, 1);
        stack.back()->children.push_back(std::move(n));
    }
    return root;
}

const Node* child(const Node& n, std::string_view name) {
    for (const Node& c : n.children)
        if (c.name == name)
            return &c;
    return nullptr;
}

void read(const Node& n, std::string_view name, float& out) {
    if (const Node* c = child(n, name); c && !c->values.empty())
        out = c->values[0];
}

void read(const Node& n, std::string_view name, int& out) {
    if (const Node* c = child(n, name); c && !c->values.empty())
        out = static_cast<int>(c->values[0]);
}

void read(const Node& n, std::string_view name, Vec3& out) {
    if (const Node* c = child(n, name); c && c->values.size() >= 3)
        out = {c->values[0], c->values[1], c->values[2]};
}

void read(const Node& n, std::string_view name, std::vector<float>& out) {
    if (const Node* c = child(n, name))
        out = c->values;
}

Mm1WheelParams wheel(const Node& n) {
    Mm1WheelParams w;
    read(n, "Spring", w.spring);
    read(n, "Damping", w.damping);
    read(n, "SteeringRatio", w.steeringRatio);
    read(n, "SuspensionBlend", w.suspensionBlend);
    read(n, "BrakeRatio", w.brakeRatio);
    read(n, "StaticFriction", w.staticFriction);
    read(n, "SuspensionLimit", w.suspensionLimit);
    read(n, "RenderableSuspensionLimit", w.renderableSuspensionLimit);
    read(n, "RubberSpring", w.rubberSpring);
    read(n, "RubberDamp", w.rubberDamp);
    read(n, "OptimumSlipPercent", w.optimumSlipPercent);
    read(n, "StaticFric", w.staticFric);
    read(n, "SlidingFric", w.slidingFric);
    w.hasLat = child(n, "RubberSpringLat") != nullptr;
    read(n, "RubberSpringLat", w.rubberSpringLat);
    read(n, "RubberDampLat", w.rubberDampLat);
    return w;
}

} // namespace

std::optional<Mm1CarParams> parseMm1CarParams(std::string_view text, std::string* error) {
    const Node root = parseTree(text);
    const Node* top = nullptr;
    for (const Node& c : root.children)
        if (c.block && c.name == "mmCarSim")
            top = &c;
    if (!top) {
        if (error)
            *error = "no mmCarSim block";
        return std::nullopt;
    }
    const Node& n = *top;
    Mm1CarParams p;
    p.className = n.name;
    read(n, "Mass", p.mass);
    read(n, "InertiaBox", p.inertiaBox);
    read(n, "BodyCG", p.bodyCG);
    read(n, "Drag", p.drag);
    read(n, "Downforce", p.downforce);
    read(n, "BoundFriction", p.boundFriction);
    read(n, "BoundElasticity", p.boundElasticity);
    read(n, "DriftTorque", p.driftTorque);
    read(n, "DrivetrainType", p.drivetrainType);
    read(n, "CarFrictionHandling", p.carFrictionHandling);
    read(n, "MaxDamage", p.maxDamage);
    read(n, "MedDamage", p.medDamage);
    read(n, "RedistHeight", p.redistHeight);
    read(n, "RedistLongRatio", p.redistLongRatio);
    read(n, "SlipPercentThresh", p.slipPercentThresh);
    read(n, "LongSlideMultiplier", p.longSlideMultiplier);
    read(n, "SteerMultiplier", p.steerMultiplier);
    read(n, "DriftThreshold", p.driftThreshold);
    read(n, "SpinThreshold", p.spinThreshold);
    read(n, "SpinStart", p.spinStart);
    read(n, "SpinStop", p.spinStop);
    read(n, "SpinFromMin", p.spinFromMin);
    read(n, "SpinFromMax", p.spinFromMax);
    read(n, "SpinToMin", p.spinToMin);
    read(n, "SpinToMax", p.spinToMax);
    read(n, "SpinStartTime", p.spinStartTime);
    read(n, "SpinEndTime", p.spinEndTime);
    read(n, "FrontDriftFricMultiplier", p.frontDriftFricMultiplier);
    read(n, "BackDriftFricMultiplier", p.backDriftFricMultiplier);
    read(n, "FrontSpinFricMultiplier", p.frontSpinFricMultiplier);
    read(n, "BackSpinFricMultiplier", p.backSpinFricMultiplier);
    read(n, "BrakeFrontFricMultiplier", p.brakeFrontFricMultiplier);
    read(n, "BrakeBackFricMultiplier", p.brakeBackFricMultiplier);
    read(n, "WeebleTorque", p.weebleTorque);
    read(n, "DamageSmokeOffset", p.damageSmokeOffset);
    if (const Node* c = child(n, "DashCamHeadlightOffset"); c && c->values.size() >= 3)
        p.dashCamHeadlightOffset = Vec3{c->values[0], c->values[1], c->values[2]};
    if (const Node* c = child(n, "POVCamHeadlightOffset"); c && c->values.size() >= 3)
        p.povCamHeadlightOffset = Vec3{c->values[0], c->values[1], c->values[2]};

    if (const Node* e = child(n, "Engine")) {
        read(*e, "MaxHorsePower", p.engine.maxHorsePower);
        read(*e, "OptRPM", p.engine.optRPM);
        read(*e, "MaxRPM", p.engine.maxRPM);
        read(*e, "GCL", p.engine.gcl);
    }
    if (const Node* t = child(n, "Trans")) {
        read(*t, "NumGears", p.trans.numGears);
        read(*t, "UpshiftThreshold", p.trans.upshiftThreshold);
        read(*t, "DownshiftThreshold", p.trans.downshiftThreshold);
        read(*t, "GearRatios", p.trans.gearRatios);
        read(*t, "UpshiftRPM", p.trans.upshiftRPM);
        read(*t, "DownshiftRPM", p.trans.downshiftRPM);
        read(*t, "ManualNumGears", p.trans.manualNumGears);
        read(*t, "ManualGearRatios", p.trans.manualGearRatios);
        read(*t, "DownshiftBias", p.trans.downshiftBias);
    }
    if (const Node* a = child(n, "AeroCollide")) {
        read(*a, "CDamp", p.aero.cDamp);
        read(*a, "VelDamp", p.aero.velDamp);
        read(*a, "Vel2Damp", p.aero.vel2Damp);
        read(*a, "AngCDamp", p.aero.angCDamp);
        read(*a, "AngVelDamp", p.aero.angVelDamp);
        read(*a, "AngVel2Damp", p.aero.angVel2Damp);
    }
    if (const Node* w = child(n, "FrontLeft"))
        p.frontLeft = wheel(*w);
    if (const Node* w = child(n, "BackLeft"))
        p.backLeft = wheel(*w);
    return p;
}

} // namespace mm2::phys
