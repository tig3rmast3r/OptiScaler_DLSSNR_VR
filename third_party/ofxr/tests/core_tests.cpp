#include "xrfg/image.hpp"
#include "xrfg/generation_backpressure.hpp"
#include "xrfg/pose.hpp"
#include "xrfg/luke_guide_eye.hpp"
#include "xrfg/luke_guide_window.hpp"
#include "xrfg/luke_physical_projection.hpp"
#include "xrfg/luke_pose_camera.hpp"
#include "xrfg/luke_world_scale.hpp"
#include "xrfg/luke_camera_units.hpp"
#include "xrfg/luke_ingress_devices.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

[[nodiscard]] bool near(float left, float right, float epsilon = 1.0e-4F) {
    return std::abs(left - right) <= epsilon;
}

[[nodiscard]] xrfg::Quaternion yaw(float degrees) {
    const float half_angle = degrees * std::numbers::pi_v<float> / 360.0F;
    return {0.0F, std::sin(half_angle), 0.0F, std::cos(half_angle)};
}

}  // namespace

int main() {
    // Every combination of missing devices, optional depth, adapter and strict
    // queue/list identity must preserve the old guard's acceptance exactly.
    for (unsigned bits = 0; bits < 1024; ++bits) {
        const auto b = [&](unsigned i) { return (bits & (1u << i)) != 0; };
        const xrfg::LukeIngressDeviceFacts f{b(0),b(1),b(2),b(3),b(4),b(5),b(6),b(7),b(8),b(9)};
        const bool old_admitted = f.output_device && f.motion_device &&
            f.command_device && f.queue_device && f.output_adapter &&
            f.motion_adapter && f.queue_list_identity &&
            (!f.depth_present || (f.depth_device && f.depth_adapter));
        expect((xrfg::luke_ingress_device_failures(f) == 0) == old_admitted,
            "diagnostic device facts must not change admission");
    }
    expect(xrfg::luke_ingress_device_failures({true,true,true,true,true,true,true,true,true,false}) == 256,
        "queue/list identity is independently reported and still rejects");
    const std::array<float, 16> luke_projection{
        0.70020777F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.70020777F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.0000463724F, -1.0F,
        0.0F, 0.0F, 0.190008804F, 0.0F};
    const auto degrees = [](float value) {
        return value * std::numbers::pi_v<float> / 180.0F;
    };
    const auto left_window = xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 2803);
    const auto right_window = xrfg::luke_guide_window(luke_projection,
        degrees(-40), degrees(54), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 2803);
    expect(left_window && right_window && left_window->x == 30 &&
        right_window->x == 345 && left_window->y == 270 &&
        right_window->y == 270 && left_window->width == 1297 &&
        left_window->height == 1402,
        "Horizon eye guide windows follow projection and physical FOV");
    const auto double_resolution = xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        6688, 6688, 3344, 3344, 5188, 5606);
    expect(double_resolution && double_resolution->x == 61 &&
        double_resolution->y == 541 && double_resolution->width == 2594 &&
        double_resolution->height == 2803,
        "guide window scales with resolution instead of using pixel offsets");
    expect(!xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 3000, 2803),
        "mismatched colour extent rejects an unsafe unscaled guide");
    xrfg::LukeGuideWindowFailure rejection{};
    expect(!xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 3063, 3344, &rejection) &&
        rejection == xrfg::LukeGuideWindowFailure::color_width_mismatch,
        "Ghost-sized rejected crop identifies horizontal mismatch without admitting it");
    expect(!xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 3000, &rejection) &&
        rejection == xrfg::LukeGuideWindowFailure::color_height_mismatch,
        "vertical extent rejection is independently observable");
    expect(xrfg::luke_guide_window(luke_projection,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 2803, &rejection) &&
        rejection == xrfg::LukeGuideWindowFailure::none,
        "accepted crop clears prior diagnostic reason and retains original window");
    // Exact V31001 PID36652 projection/FOV, not the Horizon-sized fixture.
    const std::array<float, 16> ghost_projection{
        0.8540806174278259F, 0, 0, 0,
        0, 0.8540806174278259F, 0, 0,
        0, 0, -0.0000010000009069699445F, 0.9999999403953552F,
        0, 0, 10.000008583068848F, 0};
    const auto ghost_window = xrfg::luke_guide_window(ghost_projection,
        -0.8203047513961792F, 0.8203047513961792F,
        0.8639379739761353F, -0.8639379739761353F,
        3344, 3344, 1672, 1672, 3063, 3344, &rejection);
    expect(ghost_window && ghost_window->x == 70 && ghost_window->y == 0 &&
        ghost_window->width == 1532 && ghost_window->height == 1672 &&
        rejection == xrfg::LukeGuideWindowFailure::none,
        "measured positive-Z Ghost projection admits the correct guide crop");
    auto equivalent_lh = luke_projection;
    for (unsigned column = 0; column < 4; ++column)
        equivalent_lh[8 + column] = -equivalent_lh[8 + column];
    const auto lh_window = xrfg::luke_guide_window(equivalent_lh,
        degrees(-54), degrees(40), degrees(44), degrees(-55),
        3344, 3344, 1672, 1672, 2594, 2803);
    expect(lh_window && left_window && lh_window->x == left_window->x &&
        lh_window->y == left_window->y && lh_window->width == left_window->width &&
        lh_window->height == left_window->height,
        "equivalent handedness preserves asymmetric crop geometry");
    auto ghost_physical = ghost_projection;
    expect(xrfg::luke_physical_eye_projection(ghost_physical,
        -0.8203047513961792F, 0.8203047513961792F,
        0.8639379739761353F, -0.8639379739761353F) &&
        near(static_cast<float>(xrfg::luke_projection_ray_ndc(
            std::tan(-0.8203047513961792F), ghost_physical[0],
            ghost_physical[8], ghost_physical[3], ghost_physical[11])), -1.0F) &&
        near(static_cast<float>(xrfg::luke_projection_ray_ndc(
            std::tan(0.8639379739761353F), ghost_physical[5],
            ghost_physical[9], ghost_physical[7], ghost_physical[11])), 1.0F),
        "positive-Z physical projection maps FOV edges to clip boundaries");
    auto degenerate_projection = ghost_projection;
    degenerate_projection[11] = 0;
    expect(!xrfg::luke_guide_window(degenerate_projection,
        -0.8203047513961792F, 0.8203047513961792F,
        0.8639379739761353F, -0.8639379739761353F,
        3344, 3344, 1672, 1672, 3063, 3344) &&
        !xrfg::luke_physical_eye_projection(degenerate_projection,
            -0.8203047513961792F, 0.8203047513961792F,
            0.8639379739761353F, -0.8639379739761353F),
        "missing perspective direction remains rejected");
    auto left_physical = luke_projection;
    auto right_physical = luke_projection;
    expect(xrfg::luke_physical_eye_projection(left_physical,
        degrees(-54), degrees(40), degrees(44), degrees(-55)) &&
        xrfg::luke_physical_eye_projection(right_physical,
        degrees(-40), degrees(54), degrees(44), degrees(-55)),
        "source and destination physical projections are computable");
    const auto projected_x = [](const std::array<float, 16>& matrix,
        float angle) {
        const double tangent = std::tan(angle);
        return (matrix[0] * tangent - matrix[8]) /
            (matrix[3] * tangent - matrix[11]);
    };
    const auto projected_y = [](const std::array<float, 16>& matrix,
        float angle) {
        const double tangent = std::tan(angle);
        return (matrix[5] * tangent - matrix[9]) /
            (matrix[7] * tangent - matrix[11]);
    };
    expect(near(left_physical[0], right_physical[0]) &&
        left_physical[8] < 0.0F && right_physical[8] > 0.0F &&
        near(static_cast<float>(projected_x(left_physical, degrees(-54))), -1) &&
        near(static_cast<float>(projected_x(left_physical, degrees(40))), 1) &&
        near(static_cast<float>(projected_x(right_physical, degrees(-40))), -1) &&
        near(static_cast<float>(projected_x(right_physical, degrees(54))), 1) &&
        near(static_cast<float>(projected_y(left_physical, degrees(44))), 1) &&
        near(static_cast<float>(projected_y(left_physical, degrees(-55))), -1),
        "physical-eye projections map each eye's angular edges to NDC edges");
    auto invalid_physical = luke_projection;
    expect(!xrfg::luke_physical_eye_projection(invalid_physical,
        degrees(0), degrees(0), degrees(44), degrees(-55)),
        "degenerate physical eye FOV is rejected");
    float game_baseline{};
    expect(xrfg::luke_game_eye_baseline(0.0643343F, 1.1F, 1.0F,
        game_baseline) && near(game_baseline, 0.0584857F),
        "metre-scale calibrated camera preserves the Horizon baseline");
    expect(!xrfg::luke_game_eye_baseline(0.0643343F, 0.0F, 1.0F,
        game_baseline), "missing Luke WorldScale rejects AFW camera");
    expect(xrfg::luke_game_eye_baseline(0.06406F, 1.0F, 100.0F, game_baseline) &&
        near(game_baseline, 6.406F), "centimetre-scale producer camera uses its measured units");
    expect(!xrfg::luke_game_eye_baseline(0.06406F, 1.0F, 0, game_baseline),
        "missing calibration never falls back to metres");
    for (float units : {1.0F, 100.0F}) {
        for (float world_scale : {0.5F, 1.0F, 2.0F}) {
            const float baseline = 0.064F * units / world_scale;
            const float velocity = 0.002F * units;
            xrfg::LukeUnitCamera a{{-baseline/2,0,0},{1,0,0},8,true};
            xrfg::LukeUnitCamera b{{baseline/2+8*velocity,0,0},{1,0,0},8,true};
            xrfg::LukeUnitCamera c{{-baseline/2+20*velocity,0,0},{1,0,0},12,true};
            const float measured = xrfg::luke_camera_separation(a,b,c);
            expect(near(measured, baseline), "unequal frame intervals cancel linear head translation in both unit systems");
            xrfg::LukeCameraUnitCalibration calibration;
            for (unsigned i=1;i<=8;++i)
                expect(calibration.observe(1,i,measured)==0, "no admission before independent calibration samples");
            expect(near(calibration.observe(1,9,measured),baseline), "consistent independent observations calibrate camera units");
            expect(near(calibration.observe(1,9,measured),baseline) && calibration.samples()==9,
                "duplicate resolver calls do not add calibration confidence");
            expect(calibration.observe(2,10,measured)==0 && calibration.samples()==1,
                "new NGX epoch requires fresh calibration");
            for(unsigned i=11;i<=18;++i) calibration.observe(2,i,measured);
            expect(near(calibration.observe(2,19,measured*2),baseline),
                "head acceleration does not rescale an established engine unit");
            expect(near(calibration.observe(2,20,0),baseline),
                "frame gap cannot invalidate established unit calibration");
            expect(calibration.observe(3,21,measured*2)==0 && calibration.samples()==1,
                "explicit new epoch permits a fresh unit conversion");
            float physical_baseline{};
            expect(xrfg::luke_game_eye_baseline(.064F, world_scale, units, physical_baseline) &&
                near(physical_baseline, baseline), "WorldScale applied exactly once to stable engine units");
            c.valid=false;
            expect(xrfg::luke_camera_separation(a,b,c)==0,"invalid camera metadata cannot calibrate");
        }
    }
    xrfg::LukeCameraUnitCalibration unstable_units;
    for(unsigned i=1;i<=12;++i)
        expect(unstable_units.observe(1,i,i%2 ? 6.0F : 7.0F)==0,
            "inconsistent acceleration/camera separation is not silently accepted");
    constexpr float physical_ipd = 0.06412F;
    const std::array<float, 3> xr_left{-physical_ipd * 0.5F, 0.0F, 0.0F};
    const std::array<float, 3> xr_right{physical_ipd * 0.5F, 0.0F, 0.0F};
    float measured_ipd{};
    expect(xrfg::luke_xr_eye_baseline(xr_left, xr_right, measured_ipd) &&
        near(measured_ipd, physical_ipd, 1.0e-6F),
        "same-call XR eye positions measure physical IPD");
    for (const float angle : {0.0F, 35.0F, 90.0F, 175.0F, 300.0F}) {
        const float cosine = std::cos(degrees(angle));
        const float sine = std::sin(degrees(angle));
        const std::array<float, 3> center{1.25F + angle * 0.001F,
            1.7F, -2.1F - angle * 0.002F};
        const auto moved_eye = [&](float local_x) {
            return std::array<float, 3>{center[0] + local_x * cosine,
                center[1], center[2] - local_x * sine};
        };
        expect(xrfg::luke_xr_eye_baseline(
            moved_eye(xr_left[0]), moved_eye(xr_right[0]), measured_ipd) &&
            near(measured_ipd, physical_ipd, 1.0e-5F),
            "same-call physical IPD is invariant under common headset translation and yaw");
    }
    auto invalid_eye = xr_left;
    invalid_eye[1] = std::numeric_limits<float>::quiet_NaN();
    expect(!xrfg::luke_xr_eye_baseline(invalid_eye, xr_right, measured_ipd),
        "nonfinite XR eye position rejects physical IPD");
    invalid_eye = xr_right;
    invalid_eye[2] = std::numeric_limits<float>::infinity();
    expect(!xrfg::luke_xr_eye_baseline(xr_left, invalid_eye, measured_ipd),
        "infinite XR eye position rejects physical IPD");
    expect(!xrfg::luke_xr_eye_baseline(xr_left, xr_left, measured_ipd),
        "coincident XR eye positions reject physical IPD");
    expect(!xrfg::luke_xr_eye_baseline({0.0F, 0.0F, 0.0F},
        {0.005F, 0.0F, 0.0F}, measured_ipd) &&
        !xrfg::luke_xr_eye_baseline({0.0F, 0.0F, 0.0F},
        {0.25F, 0.0F, 0.0F}, measured_ipd),
        "unreasonable XR eye separation rejects physical IPD");
    auto current_left = xr_left;
    auto current_right = xr_right;
    current_left[0] += 0.008F;
    current_right[0] += 0.008F;
    float shortened_cross_time{}, lengthened_cross_time{};
    expect(xrfg::luke_xr_eye_baseline(current_left, xr_right,
            shortened_cross_time) &&
        xrfg::luke_xr_eye_baseline(xr_left, current_right,
            lengthened_cross_time) &&
        shortened_cross_time < physical_ipd - 0.007F &&
        lengthened_cross_time > physical_ipd + 0.007F &&
        xrfg::luke_xr_eye_baseline(current_left, current_right, measured_ipd) &&
        near(measured_ipd, physical_ipd, 1.0e-6F),
        "retained cross-time AER peer invents alternating IPD while a same-call pair remains constant");
    const xrfg::LukeCameraBasis camera_basis{{
        {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    xrfg::LukeCameraBasis peer_basis{};
    expect(xrfg::luke_destination_camera_basis(yaw(0.0F), yaw(10.0F),
        camera_basis, peer_basis) &&
        near(peer_basis[0][0], std::cos(degrees(10))) &&
        near(peer_basis[0][2], -std::sin(degrees(10))) &&
        near(peer_basis[2][0], std::sin(degrees(10))),
        "submitted headset yaw rotates only the generated peer camera");
    expect(xrfg::luke_destination_camera_basis(yaw(10.0F), yaw(10.0F),
        camera_basis, peer_basis) && near(peer_basis[0][0], 1.0F) &&
        near(peer_basis[0][2], 0.0F),
        "equal submitted orientations preserve stationary stereo");
    expect(!xrfg::luke_destination_camera_basis({0, 0, 0, 0}, yaw(10.0F),
        camera_basis, peer_basis), "invalid XR orientation rejects camera");
    xrfg::LukeCameraBasis lh_view{}, rh_view{}, lh_peer{}, rh_peer{};
    const xrfg::LukeCameraBasis physical_basis{{
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    expect(xrfg::luke_view_camera_basis(physical_basis, 1.0F, lh_view) &&
        xrfg::luke_view_camera_basis(physical_basis, -1.0F, rh_view) &&
        near(lh_view[2][2], 1.0F) && near(rh_view[2][2], -1.0F),
        "view forward follows producer projection while RH behavior is retained");
    expect(xrfg::luke_destination_view_camera_basis(yaw(0), yaw(10), lh_view,
        1.0F, lh_peer) && xrfg::luke_destination_view_camera_basis(yaw(0), yaw(10),
        rh_view, -1.0F, rh_peer), "both view conventions accept headset yaw");
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column)
            expect(near(lh_peer[row][column], rh_peer[row][column] * (row == 2 ? -1.0F : 1.0F)),
                "LH and RH camera yaw describe the same physical rotation");
    for (float sign : {-1.0F, 1.0F}) {
        xrfg::LukeCameraBasis view{};
        expect(xrfg::luke_view_camera_basis(physical_basis, sign, view),
            "producer forward convention is computable");
        // A point five metres ahead and a peer shifted right must remain
        // in front and move left in the destination view for either matrix.
        const float view_z = 5.0F * view[2][2];
        const float w = sign * view_z;
        expect(w > 0 && near((-0.06F * ghost_projection[0]) / w,
            -0.06F * ghost_projection[0] / 5.0F),
            "stereo disparity and positive clip W agree across handedness");
    }
    expect(!xrfg::luke_view_camera_basis(physical_basis, 0, lh_view) &&
        !xrfg::luke_destination_view_camera_basis(yaw(0), yaw(10), rh_view, 0, rh_peer),
        "degenerate perspective does not select a guessed camera convention");
    const std::array<float, 3> camera_right{0.27F, 0.96F, 0.01F};
    const std::array<float, 3> camera_left_position{0.0F, 0.0F, 0.0F};
    const std::array<float, 3> camera_right_position{0.0162F, 0.0576F, 0.0006F};
    expect(xrfg::luke_guide_index_for_eye(camera_left_position,
        camera_right, camera_right_position, camera_right, 0) == 0,
        "left-first Luke publication selects older left guide");
    expect(xrfg::luke_guide_index_for_eye(camera_left_position,
        camera_right, camera_right_position, camera_right, 1) == 1,
        "left-first Luke publication selects newer right guide");
    expect(xrfg::luke_guide_index_for_eye(camera_right_position,
        camera_right, camera_left_position, camera_right, 0) == 1,
        "right-first Luke publication selects newer left guide");
    expect(xrfg::luke_guide_index_for_eye(camera_right_position,
        camera_right, camera_left_position, camera_right, 1) == 0,
        "right-first Luke publication selects older right guide");
    expect(!xrfg::luke_guide_index_for_eye(camera_left_position,
        camera_right, camera_left_position, camera_right, 0),
        "same-camera publications are not a stereo guide pair");
    expect(xrfg::luke_guide_index_for_serial_eye(
        3969, 3970, 3968, 1, 0) == 0,
        "camera movement cannot skip the expected opposite-eye guide");
    expect(xrfg::luke_guide_index_for_serial_eye(
        3972, 3973, 3971, 1, 0) == 0,
        "the next physical eye selects 3972 before 3973");
    expect(xrfg::luke_guide_index_for_serial_eye(
        3966, 3967, 3966, 0, 1) == 1,
        "one NGX publication cannot be consumed by both eyes");
    expect(!xrfg::luke_guide_index_for_serial_eye(
        3972, 3973, 3973, 1, 0),
        "stale guide pair holds the previous stereo pair");
    const xrfg::Pose start{yaw(0.0F), {0.0F, 1.0F, 2.0F}};
    const xrfg::Pose end{yaw(90.0F), {2.0F, 3.0F, 6.0F}};
    const xrfg::Pose middle = xrfg::interpolate_pose(start, end, 0.5F);

    expect(near(middle.position.x, 1.0F), "position x midpoint");
    expect(near(middle.position.y, 2.0F), "position y midpoint");
    expect(near(middle.position.z, 4.0F), "position z midpoint");
    expect(near(middle.orientation.y, std::sin(std::numbers::pi_v<float> / 8.0F)), "45-degree yaw sine");
    expect(near(middle.orientation.w, std::cos(std::numbers::pi_v<float> / 8.0F)), "45-degree yaw cosine");

    const auto same_rotation_other_sign = xrfg::slerp_shortest(yaw(30.0F), {
        -yaw(30.0F).x,
        -yaw(30.0F).y,
        -yaw(30.0F).z,
        -yaw(30.0F).w,
    }, 0.5F);
    expect(near(std::abs(same_rotation_other_sign.w), std::cos(std::numbers::pi_v<float> / 12.0F)),
           "quaternion sign uses shortest path");

    const xrfg::TimedPose timed_start{100, start};
    const xrfg::TimedPose timed_end{200, end};
    const auto timed_middle = xrfg::midpoint(timed_start, timed_end);
    expect(timed_middle.has_value() && timed_middle->time_ns == 150, "timestamp midpoint");
    expect(!xrfg::midpoint(timed_end, timed_start).has_value(), "reject non-monotonic timestamps");

    const xrfg::Rgba8 black{0, 0, 0, 0};
    const xrfg::Rgba8 white{255, 255, 255, 255};
    const auto gray = xrfg::interpolate_pixel(black, white, 0.5F);
    expect(gray.r == 128 && gray.g == 128 && gray.b == 128 && gray.a == 128, "pixel midpoint rounds correctly");

    using namespace std::chrono_literals;
    expect(!xrfg::should_enter_generation_cooldown(false, 200ms, 8'333'333),
            "FidelityFX ignores compositor end spikes");
    expect(!xrfg::should_enter_generation_cooldown(true, 50ms, 8'333'333),
           "NVIDIA tolerates low-frame-rate compositor pacing");
    expect(!xrfg::should_enter_generation_cooldown(
               true, 75ms, 8'333'333),
           "NVIDIA cooldown has a strict 75 ms minimum threshold");
    expect(xrfg::should_enter_generation_cooldown(true, 75'000'001ns, 8'333'333),
           "NVIDIA retains recovery above the 75 ms minimum threshold");
    expect(!xrfg::should_enter_generation_cooldown(true, 83ms, 13'888'889),
           "NVIDIA also requires more than six display periods");
    expect(xrfg::should_enter_generation_cooldown(true, 84ms, 13'888'889),
           "NVIDIA retains recovery beyond six display periods");
    expect(!xrfg::should_enter_generation_cooldown(true, 68ms, 0),
           "invalid display period cannot start cooldown");

    constexpr std::array uevr_slow_ends{
        68'522us, 18'226us, 22'262us, 19'471us, 19'330us,
        19'863us, 19'893us, 19'440us, 22'551us};
    std::size_t uevr_nvidia_cooldowns = 0;
    for (const auto elapsed : uevr_slow_ends) {
        uevr_nvidia_cooldowns += xrfg::should_enter_generation_cooldown(
            true, elapsed, 8'333'333);
    }
    expect(uevr_nvidia_cooldowns == 0,
           "retained low-rate UEVR timing replay causes no cooldowns");

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All XR Frame Bridge core tests passed\n";
    return EXIT_SUCCESS;
}
