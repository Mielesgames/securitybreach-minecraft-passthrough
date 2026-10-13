#pragma once

#include "Link.h"

#include <d3d11.h>

#include <array>
#include <vector>

// Draws Minecraft's blocks inside Security Breach's picture. Minecraft sends
// its block meshes and texture atlas through the render ring; this keeps them
// on the GPU and draws them from the game's camera.
//
// Everything is drawn over the finished frame. The game's own depth buffer is
// looked up so its walls and floors hide what is behind them; its lighting
// doesn't touch Minecraft's things.
namespace world
{
    // Where the game's camera is, in Unreal units and axes.
    struct Camera
    {
        bool valid{ false };
        double position[3]{};
        float forward[3]{};
        float right[3]{};
        float up[3]{};
        // Tangent of half the horizontal field of view.
        float tan_half_fov{ 1.0f };
    };

    // Game thread, every tick.
    void set_camera(const Camera& camera);

    // Where Minecraft's player stands (feet, in blocks): its body is drawn
    // there in third person. Game thread.
    void set_player_position(double x, double y, double z);

    // The leads characters are held on, as lines from one end to the other
    // (six Unreal coordinates each). Game thread, whenever they change place.
    void set_ropes(const std::vector<std::array<double, 6>>& ropes);

    // Minecraft's night vision effect: brightens the game's picture and
    // lights Minecraft's own things fully. Any thread.
    void set_night_vision(bool on);

    // The calls below belong to the thread that presents.

    // Takes everything Minecraft queued in the render ring. Has to run every
    // frame, drawn or not: a full ring stalls Minecraft.
    void consume(SecurityBreachLink& link, ID3D11Device* device, ID3D11DeviceContext* context);

    // Draws the blocks into `target`. Changes pipeline state without putting
    // it back: the caller saves and restores.
    void draw(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11RenderTargetView* target,
        UINT width,
        UINT height
    );

    // Is there a Minecraft block here that things collide with? Block
    // coordinates; any thread.
    bool solid_at(int x, int y, int z);

    // What burns here: 0 nothing, else a proto::BlockHazard (fire, lava,
    // magma). Block coordinates; any thread.
    int hazard_at(int x, int y, int z);

    // The collision boxes of blocks that aren't full cubes (doors, fences,
    // slabs) that touch the given box, as {minx, miny, minz, maxx, maxy,
    // maxz} in blocks, appended to `out`. Any thread.
    void shapes_in(const double low[3], const double high[3], std::vector<std::array<float, 6>>& out);

    // Lets go of the game's device context again.
    void shutdown();
}
